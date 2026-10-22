// UE 5.8 Engine/Plugins/Runtime/MeshModelingToolset/Source/MeshModelingTools/Private/Sculpting/
// MeshMoveBrushOps.h (FMoveBrushOp), MeshSmoothingBrushOps.h (FSmoothBrushOp: UniformCentroid lerp),
// MeshInflateBrushOps.h (FInflateBrushOp), MeshPlaneBrushOps.h (FFlattenBrushOp: lerp to the region plane),
// MeshPinchBrushOps.h (FPinchBrushOp: tangential pull to the stamp) and MeshSculptFalloffs.cpp (the falloff
// band), adapted to an in-place stamp over the whole vertex set.
#include "Engine/Geometry/MeshSculptBrushes.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/MeshRegionOperation.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace Desert::Geometry
{
    const char* ToString( SculptBrush brush )
    {
        switch ( brush )
        {
            case SculptBrush::Move:
                return "Move";
            case SculptBrush::Smooth:
                return "Smooth";
            case SculptBrush::Inflate:
                return "Inflate";
            case SculptBrush::Flatten:
                return "Flatten";
            case SculptBrush::Pinch:
                return "Pinch";
        }
        return "?";
    }

    const char* ToString( SculptFalloff falloff )
    {
        switch ( falloff )
        {
            case SculptFalloff::Smooth:
                return "Smooth";
            case SculptFalloff::Linear:
                return "Linear";
            case SculptFalloff::Inverse:
                return "Inverse";
            case SculptFalloff::Round:
                return "Round";
        }
        return "?";
    }

    double SculptFalloffWeight( const SculptSettings& settings, double distance )
    {
        const double radius = settings.Radius;
        if ( radius <= 0.0 || distance >= radius )
            return 0.0;
        const double inner = ( 1.0 - std::clamp( static_cast<double>( settings.FalloffAmount ), 0.0, 1.0 ) ) * radius;
        if ( distance <= inner )
            return 1.0;
        const double t = std::clamp( ( distance - inner ) / ( radius - inner ), 0.0, 1.0 );
        switch ( settings.FalloffType )
        {
            case SculptFalloff::Linear:
                return 1.0 - t;
            case SculptFalloff::Inverse:
                return ( 1.0 - t ) * ( 1.0 - t );
            case SculptFalloff::Round:
                return std::sqrt( std::max( 0.0, 1.0 - t * t ) );
            case SculptFalloff::Smooth:
            default:
                return 1.0 - t * t * ( 3.0 - 2.0 * t );
        }
    }

    Common::ResultStr<int32_t> ApplySculptStamp( DynamicMesh3& mesh, const SculptSettings& settings,
                                                 const SculptStamp& stamp )
    {
        if ( !( settings.Radius > 0.0f ) )
            return Common::MakeFormattedError<int32_t>( "the brush radius must be above zero" );
        if ( !( settings.Strength >= 0.0f && settings.Strength <= 1.0f ) )
            return Common::MakeFormattedError<int32_t>( "the brush strength must be within 0..1" );

        const double radius   = settings.Radius;
        int32_t      moved    = 0;
        const bool   isMove   = settings.Brush == SculptBrush::Move;
        const auto   centre   = isMove ? stamp.PrevCenter : stamp.Center;
        const auto   moveBy   = stamp.Center - stamp.PrevCenter;
        if ( isMove && glm::dot( moveBy, moveBy ) <= 0.0 )
            return Common::MakeSuccess<int32_t>( 0 );

        // The ROI (vertex, weight) first: every new position is computed from the unmoved mesh.
        std::vector<std::pair<int, double>> roi;
        for ( const int vid : mesh.VertexIndicesItr() )
        {
            const double d = glm::length( mesh.GetVertex( vid ) - centre );
            if ( d >= radius )
                continue;
            const double w = SculptFalloffWeight( settings, d );
            if ( w > 0.0 )
                roi.emplace_back( vid, w );
        }

        // Flatten's region plane (UE's stamp RegionPlane): through the weighted ROI centroid, across the
        // weighted mean of the ROI's vertex normals.
        glm::dvec3 planePoint( 0.0 ), planeNormal( 0.0 );
        if ( settings.Brush == SculptBrush::Flatten )
        {
            double weightSum = 0.0;
            for ( const auto& [vid, w] : roi )
            {
                planePoint += w * mesh.GetVertex( vid );
                planeNormal += w * MeshNormals::ComputeVertexNormal( mesh, vid );
                weightSum += w;
            }
            if ( weightSum <= 0.0 || glm::dot( planeNormal, planeNormal ) <= 1e-18 )
                return Common::MakeSuccess<int32_t>( 0 );
            planePoint /= weightSum;
            planeNormal = glm::normalize( planeNormal );
        }

        const double                            strength = settings.Strength;
        std::vector<std::pair<int, glm::dvec3>> moves;
        moves.reserve( roi.size() );
        for ( const auto& [vid, w] : roi )
        {
            const glm::dvec3 p = mesh.GetVertex( vid );
            switch ( settings.Brush )
            {
                case SculptBrush::Move:
                    moves.emplace_back( vid, p + w * moveBy );
                    break;
                case SculptBrush::Smooth:
                {
                    glm::dvec3 sum( 0.0 );
                    int        count = 0;
                    mesh.EnumerateVertexVertices( vid,
                                                  [&]( int32_t nb )
                                                  {
                                                      sum += mesh.GetVertex( nb );
                                                      ++count;
                                                  } );
                    if ( count > 0 )
                        moves.emplace_back( vid, glm::mix( p, sum / static_cast<double>( count ),
                                                           std::clamp( w * strength, 0.0, 1.0 ) ) );
                    break;
                }
                case SculptBrush::Inflate:
                {
                    const glm::dvec3 n = MeshNormals::ComputeVertexNormal( mesh, vid );
                    if ( glm::dot( n, n ) > 1e-18 )
                        moves.emplace_back( vid, p + ( w * strength * kSculptInflateStep * radius ) * n );
                    break;
                }
                case SculptBrush::Flatten:
                {
                    const glm::dvec3 onPlane = p - glm::dot( p - planePoint, planeNormal ) * planeNormal;
                    moves.emplace_back(
                         vid, glm::mix( p, onPlane, std::clamp( w * strength * kSculptPlaneRate, 0.0, 1.0 ) ) );
                    break;
                }
                case SculptBrush::Pinch:
                {
                    // Towards the centre, with the pull's component along the vertex normal removed (the
                    // surface draws together into a crease instead of collapsing into the brush).
                    const glm::dvec3 n    = MeshNormals::ComputeVertexNormal( mesh, vid );
                    glm::dvec3       pull = centre - p;
                    if ( glm::dot( n, n ) > 1e-18 )
                        pull -= glm::dot( pull, n ) * n;
                    moves.emplace_back( vid, p + std::clamp( w * strength * kSculptPlaneRate, 0.0, 1.0 ) * pull );
                    break;
                }
            }
        }
        for ( const auto& [vid, p] : moves )
        {
            mesh.SetVertex( vid, p );
            ++moved;
        }
        if ( moved > 0 )
        {
            if ( mesh.HasAttributes() && mesh.Attributes()->PrimaryNormals() != nullptr )
                MeshNormals::QuickRecomputeOverlayNormals( mesh );
            if ( mesh.HasVertexNormals() )
                MeshNormals::QuickComputeVertexNormals( mesh );
        }
        return Common::MakeSuccess( moved );
    }

    Common::BoolResultStr FinishSculptStroke( DynamicMesh3& mesh )
    {
        return RecomputeTangentSpace( mesh, "Vertex Sculpt" );
    }
} // namespace Desert::Geometry
