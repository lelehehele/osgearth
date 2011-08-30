/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2008-2010 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include <osgEarthFeatures/ExtrudeGeometryFilter>
#include <osgEarthSymbology/MeshSubdivider>
#include <osgEarthSymbology/MeshConsolidator>
#include <osgEarth/ECEF>
#include <osg/ClusterCullingCallback>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osgUtil/Tessellator>
#include <osgUtil/Optimizer>
#include <osgUtil/SmoothingVisitor>
#include <osg/Version>
#include <osgEarth/Version>

#define LC "[ExtrudeGeometryFilter] "

using namespace osgEarth;
using namespace osgEarth::Features;
using namespace osgEarth::Symbology;

namespace
{
    // Calculates the rotation angle of a shape. This conanically applies to
    // buildings; it finds the longest edge and compares its angle to the
    // x-axis to determine a rotation value. This method is used so we can 
    // properly rotate textures for rooftop application.
    float getApparentRotation( const Geometry* geom )
    {
        Segment n;
        double  maxLen2 = 0.0;
        ConstSegmentIterator i( geom, true );
        while( i.hasMore() )
        {
            Segment s = i.next();
            double len2 = (s.second - s.first).length2();
            if ( len2 > maxLen2 ) 
            {
                maxLen2 = len2;
                n = s;
            }
        }

        const osg::Vec3d& p1 = n.first.x() < n.second.x() ? n.first : n.second;
        const osg::Vec3d& p2 = n.first.x() < n.second.x() ? n.second : n.first;

        return atan2( p2.y()-p1.y(), p2.x()-p1.x() );
    }
}

//------------------------------------------------------------------------

ExtrudeGeometryFilter::ExtrudeGeometryFilter() :
_maxAngle_deg       ( 5.0 ),
_mergeGeometry      ( true ),
_wallAngleThresh_deg( 60.0 ),
_styleDirty         ( true )
{
    //NOP
}

void
ExtrudeGeometryFilter::setStyle( const Style& style )
{
    _style      = style;
    _styleDirty = true;
}

void
ExtrudeGeometryFilter::reset( const FilterContext& context )
{
    _cosWallAngleThresh = cos( _wallAngleThresh_deg );
    _geodes.clear();

    if ( _styleDirty )
    {
        const StyleSheet* sheet = context.getSession()->styles();

        _wallSkinSymbol    = 0L;
        _wallPolygonSymbol = 0L;
        _roofSkinSymbol    = 0L;
        _roofPolygonSymbol = 0L;
        _extrusionSymbol   = 0L;

        _extrusionSymbol = _style.get<ExtrusionSymbol>();
        if ( _extrusionSymbol.valid() )
        {
            // make a copy of the height expression so we can use it:
            if ( _extrusionSymbol->heightExpression().isSet() )
            {
                _heightExpr = *_extrusionSymbol->heightExpression();
            }

            // account for MSL-relative height:
            if ( _extrusionSymbol->heightReference() == ExtrusionSymbol::HEIGHT_REFERENCE_MSL )
            {
                _heightOffsetExpr = NumericExpression("[__max_z]");
            }
            
            // attempt to extract the wall symbols:
            if ( _extrusionSymbol->wallStyleName().isSet() && sheet != 0L )
            {
                const Style* wallStyle = sheet->getStyle( *_extrusionSymbol->wallStyleName(), false );
                if ( wallStyle )
                {
                    _wallSkinSymbol = wallStyle->get<SkinSymbol>();
                    _wallPolygonSymbol = wallStyle->get<PolygonSymbol>();
                }
            }

            // attempt to extract the rooftop symbols:
            if ( _extrusionSymbol->roofStyleName().isSet() && sheet != 0L )
            {
                const Style* roofStyle = sheet->getStyle( *_extrusionSymbol->roofStyleName(), false );
                if ( roofStyle )
                {
                    _roofSkinSymbol = roofStyle->get<SkinSymbol>();
                    _roofPolygonSymbol = roofStyle->get<PolygonSymbol>();
                }
            }       
        }

        // backup plan for skin symbols:
        const SkinSymbol* skin = _style.get<SkinSymbol>();
        if ( skin )
        {
            if ( !_wallSkinSymbol.valid() )
                _wallSkinSymbol = skin;
            if ( !_roofSkinSymbol.valid() )
                _roofSkinSymbol = skin;
        }

        // backup plan for poly symbols:
        const PolygonSymbol* poly = _style.get<PolygonSymbol>();
        if ( poly )
        {
            if ( !_wallPolygonSymbol.valid() )
                _wallPolygonSymbol = poly;
            if ( !_roofPolygonSymbol.valid() )
                _roofPolygonSymbol = poly;
        }

        _styleDirty = false;
    }
}

namespace
{
    osg::Vec3d 
    toECEF( const osg::Vec3d& input, const SpatialReference* srs, const osg::Matrixd& world2local )
    {
        osg::Vec3d ecef;
        srs->transformToECEF( input, ecef );
        return ecef * world2local;
    }
}

bool
ExtrudeGeometryFilter::extrudeGeometry(const Geometry*         input,
                                       double                  height,
                                       double                  heightOffset,
                                       bool                    flatten,
                                       osg::Geometry*          walls,
                                       osg::Geometry*          roof,
                                       osg::Geometry*          base,
                                       const osg::Vec4&        wallColor,
                                       const osg::Vec4&        roofColor,
                                       const SkinResource*     wallSkin,
                                       const SkinResource*     roofSkin,
                                       FilterContext&          cx )
{
    //todo: establish reference frame for going to geocentric. This will ultimately
    // passed in to the function.
    const SpatialReference* srs = cx.extent()->getSRS();

    // whether to convert the final geometry to localized ECEF
    bool makeECEF = cx.getSession()->getMapInfo().isGeocentric();


    bool made_geom = false;

    double tex_width_m   = wallSkin ? *wallSkin->imageWidth() : 1.0;
    double tex_height_m  = wallSkin ? *wallSkin->imageHeight() : 1.0;
    bool   tex_repeats_y = wallSkin ? *wallSkin->isTiled() : false;
    bool   useColor      = !wallSkin || wallSkin->texEnvMode() != osg::TexEnv::DECAL;

    bool isPolygon = input->getComponentType() == Geometry::TYPE_POLYGON;

    unsigned pointCount = input->getTotalPointCount();
    unsigned numVerts = 2 * pointCount;

    // create all the OSG geometry components
    osg::Vec3Array* verts = new osg::Vec3Array( numVerts );
    walls->setVertexArray( verts );

    osg::Vec2Array* texcoords = 0L;
    if ( wallSkin )
    { 
        texcoords = new osg::Vec2Array( numVerts );
        walls->setTexCoordArray( 0, texcoords );
    }

    if ( useColor )
    {
        osg::Vec4Array* colors = new osg::Vec4Array( 1 );
        (*colors)[0] = wallColor;
        walls->setColorArray( colors );
        walls->setColorBinding( osg::Geometry::BIND_OVERALL );
    }

    osg::Vec3Array* roofVerts    = 0L;
    osg::Vec2Array* topTexcoords = 0L;
    float           roofRotation = 0.0f;

    if ( roof )
    {
        roofVerts = new osg::Vec3Array( pointCount );
        roof->setVertexArray( roofVerts );

        //todo: use colors for cap? depends on whether there's a roof texture.
        osg::Vec4Array* roofColors = new osg::Vec4Array( 1 );
        (*roofColors)[0] = roofColor;
        roof->setColorArray( roofColors );
        roof->setColorBinding( osg::Geometry::BIND_OVERALL );

        if ( roofSkin )
        {
            topTexcoords = new osg::Vec2Array( pointCount );
            roof->setTexCoordArray( 0, topTexcoords );
            roofRotation = getApparentRotation( input );
        }
    }

    osg::Vec3Array* baseVerts = NULL;
    if ( base )
    {
        baseVerts = new osg::Vec3Array( pointCount );
        base->setVertexArray( baseVerts );
    }

    unsigned wallVertPtr    = 0;
    unsigned roofVertPtr    = 0;
    unsigned baseVertPtr    = 0;

    double     targetLen = -DBL_MAX;
    osg::Vec3d minLoc(DBL_MAX, DBL_MAX, DBL_MAX);
    double     minLoc_len = DBL_MAX;
    osg::Vec3d maxLoc(0,0,0);
    double     maxLoc_len = 0;

    // Initial pass over the geometry does two things:
    // 1: Calculate the minimum Z across all parts.
    // 2: Establish a "target length" for extrusion
    ConstGeometryIterator zfinder( input );
    while( zfinder.hasMore() )
    {
        const Geometry* geom = zfinder.next();
        for( Geometry::const_iterator m = geom->begin(); m != geom->end(); ++m )
        {
            osg::Vec3d m_point = *m;

            if ( m_point.z() + height > targetLen )
                targetLen = m_point.z() + height;

            if (m_point.z() < minLoc.z())
                minLoc = m_point;

            if (m_point.z() > maxLoc.z())
                maxLoc = m_point;
        }
    }

    // apply the height offsets
    height    -= heightOffset;
    targetLen -= heightOffset;

    // now generate the extruded geometry.
    ConstGeometryIterator iter( input );
    while( iter.hasMore() )
    {
        const Geometry* part = iter.next();

        double tex_height_m_adj = tex_height_m;

        unsigned wallPartPtr = wallVertPtr;
        unsigned roofPartPtr = roofVertPtr;
        unsigned basePartPtr = baseVertPtr;
        double   partLen     = 0.0;
        double   maxHeight   = 0.0;

        maxHeight = targetLen - minLoc.z();

        //Adjust the texture height so it is a multiple of the maximum height
        double div = osg::round(maxHeight / tex_height_m);
        if (div == 0) div = 1; //Prevent divide by zero
        tex_height_m_adj = maxHeight / div;

        osg::DrawElementsUInt* idx = new osg::DrawElementsUInt( GL_TRIANGLES );

        for( Geometry::const_iterator m = part->begin(); m != part->end(); ++m )
        {
            const osg::Vec3d& basePt = *m; // point = *m; //cx.toWorld( *m );

            // calculate our extrusion vector for this point.
            osg::Vec3d roofPt;

            if ( flatten )
                roofPt.set( basePt.x(), basePt.y(), targetLen );
            else
                roofPt.set( basePt.x(), basePt.y(), basePt.z() + height );
            
            // add to the approprate vertex lists:
            int p = wallVertPtr;

            if ( makeECEF )
            {
                osg::Vec3d basePtECEF = toECEF( basePt, srs, _world2local );
                osg::Vec3d roofPtECEF = toECEF( roofPt, srs, _world2local );
                if ( base )
                    (*baseVerts)[baseVertPtr++] = basePtECEF;
                if ( roof )
                    (*roofVerts)[roofVertPtr++] = roofPtECEF;
    
                (*verts)[p] = roofPtECEF;
                (*verts)[p+1] = basePtECEF;
            }
            else
            {
                if ( base )
                    (*baseVerts)[baseVertPtr++] = basePt;
                if ( roof )
                    (*roofVerts)[roofVertPtr++] = roofPt;

                (*verts)[p] = roofPt;
                (*verts)[p+1] = basePt;
            }
            
            partLen += wallVertPtr > wallPartPtr ? ((*verts)[p] - (*verts)[p-2]).length() : 0.0;
            double h = tex_repeats_y ? -((*verts)[p] - (*verts)[p+1]).length() : -tex_height_m_adj;

            if ( wallSkin )
            {
                (*texcoords)[p].set( partLen/tex_width_m, 0.0f );
                (*texcoords)[p+1].set( partLen/tex_width_m, h/tex_height_m_adj );
            }

            // form the 2 triangles
            if ( (m+1) == part->end() )
            {
                if ( isPolygon )
                {
                    // end of the wall; loop around to close it off.
                    idx->push_back(wallVertPtr); 
                    idx->push_back(wallVertPtr+1);
                    idx->push_back(wallPartPtr);

                    idx->push_back(wallVertPtr+1);
                    idx->push_back(wallPartPtr+1);
                    idx->push_back(wallPartPtr);
                }
                else
                {
                    //nop - no elements required at the end of a line
                }
            }
            else
            {
                idx->push_back(wallVertPtr); 
                idx->push_back(wallVertPtr+1);
                idx->push_back(wallVertPtr+2); 

                idx->push_back(wallVertPtr+1);
                idx->push_back(wallVertPtr+3);
                idx->push_back(wallVertPtr+2);
            }

            wallVertPtr += 2;
            made_geom = true;
        }

        walls->addPrimitiveSet( idx );

        if ( roof )
        {
            roof->addPrimitiveSet( new osg::DrawArrays(
                osg::PrimitiveSet::LINE_LOOP,
                roofPartPtr, roofVertPtr - roofPartPtr ) );
        }
        if ( base )
        {
            // reverse the base verts:
            int len = baseVertPtr - basePartPtr;
            for( int i=basePartPtr; i<len/2; i++ )
                std::swap( (*baseVerts)[i], (*baseVerts)[basePartPtr+(len-1)-i] );

            base->addPrimitiveSet( new osg::DrawArrays(
                osg::PrimitiveSet::LINE_LOOP,
                basePartPtr, baseVertPtr - basePartPtr ) );
        }
    }

    return made_geom;
}

#if 0
bool
ExtrudeGeometryFilter::extrudeGeometry(const Geometry*         input,
                                       double                  height,
                                       double                  heightOffset,
                                       bool                    flatten,
                                       osg::Geometry*          walls,
                                       osg::Geometry*          roof,
                                       osg::Geometry*          base,
                                       const osg::Vec4&        wallColor,
                                       const osg::Vec4&        roofColor,
                                       const SkinResource*     wallSkin,
                                       const SkinResource*     roofSkin,
                                       FilterContext&          cx )
{
    bool made_geom = false;

    double tex_width_m   = wallSkin ? *wallSkin->imageWidth() : 1.0;
    double tex_height_m  = wallSkin ? *wallSkin->imageHeight() : 1.0;
    bool   tex_repeats_y = wallSkin ? *wallSkin->isTiled() : false;
    bool   useColor      = !wallSkin || wallSkin->texEnvMode() != osg::TexEnv::DECAL;

    bool isPolygon = input->getComponentType() == Geometry::TYPE_POLYGON;

    unsigned pointCount = input->getTotalPointCount();
    unsigned numVerts = 2 * pointCount;

    // create all the OSG geometry components
    osg::Vec3Array* verts = new osg::Vec3Array( numVerts );
    walls->setVertexArray( verts );

    osg::Vec2Array* texcoords = 0L;
    if ( wallSkin )
    { 
        texcoords = new osg::Vec2Array( numVerts );
        walls->setTexCoordArray( 0, texcoords );
    }

    if ( useColor )
    {
        osg::Vec4Array* colors = new osg::Vec4Array( 1 );
        (*colors)[0] = wallColor;
        walls->setColorArray( colors );
        walls->setColorBinding( osg::Geometry::BIND_OVERALL );
    }

    osg::Vec3Array* roofVerts    = 0L;
    osg::Vec2Array* topTexcoords = 0L;
    float           roofRotation = 0.0f;

    if ( roof )
    {
        roofVerts = new osg::Vec3Array( pointCount );
        roof->setVertexArray( roofVerts );

        //todo: use colors for cap? depends on whether there's a roof texture.
        osg::Vec4Array* roofColors = new osg::Vec4Array( 1 );
        (*roofColors)[0] = roofColor;
        roof->setColorArray( roofColors );
        roof->setColorBinding( osg::Geometry::BIND_OVERALL );

        if ( roofSkin )
        {
            topTexcoords = new osg::Vec2Array( pointCount );
            roof->setTexCoordArray( 0, topTexcoords );
            roofRotation = getApparentRotation( input );
        }
    }

    osg::Vec3Array* baseVerts = NULL;
    if ( base )
    {
        baseVerts = new osg::Vec3Array( pointCount );
        base->setVertexArray( baseVerts );
    }

    unsigned wallVertPtr    = 0;
    unsigned roofVertPtr    = 0;
    unsigned baseVertPtr    = 0;

    double     targetLen = -DBL_MAX;
    osg::Vec3d minLoc(DBL_MAX, DBL_MAX, DBL_MAX);
    double     minLoc_len = DBL_MAX;
    osg::Vec3d maxLoc(0,0,0);
    double     maxLoc_len = 0;

    // Initial pass over the geometry does two things:
    // 1: Calculate the minimum Z across all parts.
    // 2: Establish a "target length" for extrusion
    ConstGeometryIterator zfinder( input );
    while( zfinder.hasMore() )
    {
        const Geometry* geom = zfinder.next();
        for( Geometry::const_iterator m = geom->begin(); m != geom->end(); ++m )
        {
            // Find the minimum Z
            osg::Vec3d m_world = cx.toWorld( *m );
            if ( cx.isGeocentric() )
            {
                osg::Vec3d p_vec = m_world;
                osg::Vec3d e_vec = p_vec;
                e_vec.normalize();
                p_vec = p_vec + (e_vec * height);

                double m_world_len = m_world.length();
                if ( m_world_len < minLoc_len )
                {
                    minLoc = m_world;
                    minLoc_len = m_world_len;
                }

                if ( m_world_len > maxLoc_len )
                {
                    maxLoc = m_world;
                    maxLoc_len = m_world_len;
                }

                double p_ex_len = p_vec.length();
                if ( p_ex_len > targetLen )
                    targetLen = p_ex_len;
            }
            else
            {
                if ( m_world.z() + height > targetLen )
                    targetLen = m_world.z() + height;

                if (m_world.z() < minLoc.z())
                    minLoc = m_world;

                if ( m_world.z() > maxLoc.z())
                    maxLoc = m_world;
            }
        }
    }

    // apply the height offsets
    height -= heightOffset;
    targetLen -= heightOffset;

    // now generate the extruded geometry.
    ConstGeometryIterator iter( input );
    while( iter.hasMore() )
    {
        const Geometry* part = iter.next();

        double tex_height_m_adj = tex_height_m;

        unsigned wallPartPtr = wallVertPtr;
        unsigned roofPartPtr = roofVertPtr;
        unsigned basePartPtr = baseVertPtr;
        double   partLen = 0.0;
        double   maxHeight = 0.0;

        if ( cx.isGeocentric() )
            maxHeight = targetLen - minLoc_len;
        else
            maxHeight = targetLen - minLoc.z();

        //Adjust the texture height so it is a multiple of the maximum height
        double div = osg::round(maxHeight / tex_height_m);
        if (div == 0) div = 1; //Prevent divide by zero
        tex_height_m_adj = maxHeight / div;

        osg::DrawElementsUInt* idx = new osg::DrawElementsUInt( GL_TRIANGLES );

        for( Geometry::const_iterator m = part->begin(); m != part->end(); ++m )
        {
            osg::Vec3d m_world = cx.toWorld( *m );

            // calculate our extrusion vector for this point.
            osg::Vec3d extrudeVec;
            if ( cx.isGeocentric() )
            {
                osg::Vec3d p_vec = m_world;
                if ( flatten )
                {
                    double p_len = p_vec.length();
                    double ratio = targetLen/p_len;
                    p_vec *= ratio;
                }
                else
                {
                    osg::Vec3d unitVec = p_vec;
                    unitVec.normalize();
                    p_vec = p_vec + unitVec*height;
                }

                extrudeVec = p_vec * cx.referenceFrame();
            }
            else
            {
                if ( flatten )
                    extrudeVec.set( m_world.x(), m_world.y(), targetLen );
                else
                    extrudeVec.set( m_world.x(), m_world.y(), m_world.z() + height );

                extrudeVec = extrudeVec * cx.referenceFrame();
            }

            // build the top and base caps
            if ( roof )
            {
                (*roofVerts)[roofVertPtr++] = extrudeVec;
            }
            if ( base )
            {
                (*baseVerts)[baseVertPtr++] = *m;
            }
             
            partLen += wallVertPtr > wallPartPtr ?
                (extrudeVec - (*verts)[wallVertPtr-2]).length() :
                0.0;

            double h;
            if ( tex_repeats_y )
                h = -(extrudeVec - *m).length();
            else
                h = -tex_height_m_adj;

            int p;

            p = wallVertPtr;
            (*verts)[p] = extrudeVec;
            if ( wallSkin )
                (*texcoords)[p].set( partLen/tex_width_m, 0.0f );

            p = wallVertPtr + 1;
            (*verts)[p] = *m;
            if ( wallSkin )
                (*texcoords)[p].set( partLen/tex_width_m, h/tex_height_m_adj );

            // form the 2 triangles
            if ( (m+1) == part->end() )
            {
                if ( isPolygon )
                {
                    // end of the wall; loop around to close it off.
                    idx->push_back(wallVertPtr); 
                    idx->push_back(wallVertPtr+1);
                    idx->push_back(wallPartPtr);

                    idx->push_back(wallVertPtr+1);
                    idx->push_back(wallPartPtr+1);
                    idx->push_back(wallPartPtr);
                }
                else
                {
                    //nop - no elements required at the end of a line
                }
            }
            else
            {
                idx->push_back(wallVertPtr); 
                idx->push_back(wallVertPtr+1);
                idx->push_back(wallVertPtr+2); 

                idx->push_back(wallVertPtr+1);
                idx->push_back(wallVertPtr+3);
                idx->push_back(wallVertPtr+2);
            }

            wallVertPtr += 2;
            made_geom = true;
        }

        walls->addPrimitiveSet( idx );

        if ( roof )
        {
            roof->addPrimitiveSet( new osg::DrawArrays(
                osg::PrimitiveSet::LINE_LOOP,
                roofPartPtr, roofVertPtr - roofPartPtr ) );
        }
        if ( base )
        {
            // reverse the base verts:
            int len = baseVertPtr - basePartPtr;
            for( int i=basePartPtr; i<len/2; i++ )
                std::swap( (*baseVerts)[i], (*baseVerts)[basePartPtr+(len-1)-i] );

            base->addPrimitiveSet( new osg::DrawArrays(
                osg::PrimitiveSet::LINE_LOOP,
                basePartPtr, baseVertPtr - basePartPtr ) );
        }
    }

    return made_geom;
}
#endif

bool
ExtrudeGeometryFilter::process( FeatureList& features, FilterContext& context )
{
    for( FeatureList::iterator f = features.begin(); f != features.end(); ++f )
    {
        Feature* input = f->get();

        GeometryIterator iter( input->getGeometry(), false );
        while( iter.hasMore() )
        {
            Geometry* part = iter.next();

            osg::ref_ptr<osg::Geometry> walls = new osg::Geometry();
            //walls->setUseVertexBufferObjects(true);
            
            osg::ref_ptr<osg::Geometry> rooflines = 0L;
            
            if ( part->getType() == Geometry::TYPE_POLYGON )
            {
                rooflines = new osg::Geometry();
                //rooflines->setUseVertexBufferObjects(true);

                // prep the shapes by making sure all polys are open:
                static_cast<Polygon*>(part)->open();
            }

            // calculate the extrusion height:
            float height;

            if ( _heightCallback.valid() )
            {
                height = _heightCallback->operator()(input, context);
            }
            else if ( _extrusionSymbol->heightExpression().isSet() )
            {
                height = input->eval( _heightExpr );
            }
            else
            {
                height = *_extrusionSymbol->height();
            }

            // calculate the height offset from the base:
            float offset = 0.0;
            if ( _heightOffsetExpr.isSet() )
            {
                offset = input->eval( _heightOffsetExpr.mutable_value() );
            }

            osg::StateSet* wallStateSet = 0L;

            // calculate the wall texturing:
            SkinResource* wallSkin = 0L;
            if ( _wallSkinSymbol.valid() )
            {
                if ( _wallResLib.valid() )
                {
                    SkinSymbol querySymbol( *_wallSkinSymbol.get() );
                    querySymbol.objectHeight() = height;
                    SkinResourceVector candidates;
                    _wallResLib->getSkins( &querySymbol, candidates );

                    if ( candidates.size() == 1 )
                    {
                        wallSkin = candidates[0].get();
                    }
                    else if ( candidates.size() > 1 )
                    {
                        // select one at random:
                        int index = ::rand() % candidates.size();
                        wallSkin = candidates[index].get();
                    }
                }

                else
                {
                    //TODO: simple single texture?
                }
            }

            // calculate the rooftop texture:
            SkinResource* roofSkin = 0L;
            if ( _roofSkinSymbol.valid() )
            {
                if ( _roofResLib.valid() )
                {
                    SkinSymbol querySymbol( *_roofSkinSymbol.get() );
                    SkinResourceVector candidates;
                    _roofResLib->getSkins( &querySymbol, candidates );

                    if ( candidates.size() == 1 )
                    {
                        roofSkin = candidates[0].get();
                    }
                    else if ( candidates.size() > 1 )
                    {
                        // select one at random:
                        int index = ::rand() % candidates.size();
                        roofSkin = candidates[index].get();
                    }
                }

                else
                {
                    //TODO: simple single texture?
                }
            }

            // calculate the colors:
            osg::Vec4f wallColor(1,1,1,1), roofColor(1,1,1,1);

            if ( _wallPolygonSymbol.valid() )
            {
                wallColor = _wallPolygonSymbol->fill()->color();
            }
            if ( _roofPolygonSymbol.valid() )
            {
                roofColor = _roofPolygonSymbol->fill()->color();
            }

            // Create the extruded geometry!
            if (extrudeGeometry( 
                    part, height, offset, 
                    *_extrusionSymbol->flatten(),
                    walls.get(), rooflines.get(), 0L, 
                    wallColor, roofColor,
                    wallSkin, roofSkin,
                    context ) )
            {      
                if ( wallSkin )
                {
                    wallStateSet = context.resourceCache()->getStateSet( wallSkin );
                }

                // generate per-vertex normals, altering the geometry as necessary to avoid
                // smoothing around sharp corners
    #if OSG_MIN_VERSION_REQUIRED(2,9,9)
                //Crease angle threshold wasn't added until
                osgUtil::SmoothingVisitor::smooth(
                    *walls.get(), 
                    osg::DegreesToRadians(_wallAngleThresh_deg) );            
    #else
                osgUtil::SmoothingVisitor::smooth(*walls.get());            
    #endif

                // tessellate and add the roofs if necessary:
                if ( rooflines.valid() )
                {
                    osgUtil::Tessellator tess;
                    tess.setTessellationType( osgUtil::Tessellator::TESS_TYPE_GEOMETRY );
                    tess.setWindingType( osgUtil::Tessellator::TESS_WINDING_ODD ); //POSITIVE );
                    tess.retessellatePolygons( *(rooflines.get()) );

                    // generate default normals (no crease angle necessary; they are all pointing up)
                    // TODO do this manually; probably faster
                    osgUtil::SmoothingVisitor::smooth( *rooflines.get() );

                    // texture the rooflines if necessary
                    //applyOverlayTexturing( rooflines.get(), input, env );

                    // mark this geometry as DYNAMIC because otherwise the OSG optimizer will destroy it.
                    // TODO: why??
                    rooflines->setDataVariance( osg::Object::DYNAMIC );
                }

                std::string name;
                if ( !_featureNameExpr.empty() )
                    name = input->eval( _featureNameExpr );

                // find the geode for the active stateset:
                osg::Geode* geode = _geodes[wallStateSet].get();
                if ( !geode ) {
                    geode = new osg::Geode();
                    geode->setStateSet( wallStateSet );
                    _geodes[wallStateSet] = geode;
                }

                geode->addDrawable( walls.get() );
                if ( !name.empty() )
                    walls->setName( name );

                if ( rooflines.valid() )
                {
                    // For now, sort the rooftops into the "state-set-less" geode. Later these will
                    // sort into other geodes based on rooftop texturing
                    osg::Geode* noTexGeode = _geodes[0L].get();
                    if ( !noTexGeode ) {
                        noTexGeode = new osg::Geode();
                        _geodes[0L] = noTexGeode;
                    }
                    
                    noTexGeode->addDrawable( rooflines.get() );
                    if ( !name.empty() )
                        rooflines->setName( name );
                }
            }   
        }
    }

    return true;
}

osg::Node*
ExtrudeGeometryFilter::push( FeatureList& input, FilterContext& context )
{
    reset( context );

    // minimally, we require an extrusion symbol.
    if ( !_extrusionSymbol.valid() )
    {
        OE_WARN << LC << "Missing required extrusion symbolology; geometry will be empty" << std::endl;
        return new osg::Group();
    }

    // establish the active resource library, if applicable.
    _wallResLib = 0L;
    _roofResLib = 0L;

    const StyleSheet* sheet = context.getSession()->styles();

    if ( sheet != 0L )
    {
        if ( _wallSkinSymbol.valid() && _wallSkinSymbol->libraryName().isSet() )
        {
            _wallResLib = sheet->getResourceLibrary( *_wallSkinSymbol->libraryName() );
            if ( !_wallResLib.valid() )
            {
                OE_WARN << LC << "Unable to load resource library '" << *_wallSkinSymbol->libraryName() << "'"
                    << "; wall geometry will not be textured." << std::endl;
            }
        }

        if ( _roofSkinSymbol.valid() && _roofSkinSymbol->libraryName().isSet() )
        {
            _roofResLib = sheet->getResourceLibrary( *_roofSkinSymbol->libraryName() );
            if ( !_roofResLib.valid() )
            {
                OE_WARN << LC << "Unable to load resource library '" << *_roofSkinSymbol->libraryName() << "'"
                    << "; roof geometry will not be textured." << std::endl;
            }
        }
    }

    // calculate the localization matrices (_local2world and _world2local)
    computeLocalizers( context );

    // push all the features through the extruder.
    bool ok = process( input, context );

    // convert everything to triangles and combine drawables.    
    if ( _mergeGeometry == true && _featureNameExpr.empty() )
    {
        for( SortedGeodeMap::iterator i = _geodes.begin(); i != _geodes.end(); ++i )
            MeshConsolidator::run( *i->second.get() );
    }

    // parent geometry with a delocalizer (if necessary)
    osg::Group* group = createDelocalizeGroup();
    
    // combines geometries where the statesets are the same.
    for( SortedGeodeMap::iterator i = _geodes.begin(); i != _geodes.end(); ++i )
        group->addChild( i->second.get() );
    _geodes.clear();

    OE_INFO << LC << "Sorted geometry into " << group->getNumChildren() << " groups" << std::endl;

    //TODO
    // running this after the MC reduces the primitive set count by a huge amount, but I
    // have not figured out why yet.
    if ( _mergeGeometry == true )
    {
        osgUtil::Optimizer o;
        o.optimize( group, osgUtil::Optimizer::MERGE_GEOMETRY );
    }

    return group;
}
