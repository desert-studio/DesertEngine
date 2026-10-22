// UE 5.8 Engine/Plugins/Runtime/MeshModelingToolset/Source/MeshModelingTools/Private/Sculpting/
// MeshMoveBrushOps.h (FMoveBrushOp), MeshSmoothingBrushOps.h (FSmoothBrushOp: UniformCentroid lerp) and
// MeshSculptFalloffs.cpp (the falloff band), adapted to an in-place stamp over the whole vertex set.
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

        // The ROI and its new positions first, then written (Smooth reads unmoved neighbours).
        std::vector<std::pair<int, glm::dvec3>> moves;
        for ( const int vid : mesh.VertexIndicesItr() )
        {
            const glm::dvec3 p = mesh.GetVertex( vid );
            const double     d = glm::length( p - centre );
            if ( d >= radius )
                continue;
            const double w = SculptFalloffWeight( settings, d );
            if ( w <= 0.0 )
                continue;
            if ( isMove )
            {
                moves.emplace_back( vid, p + w * moveBy );
                continue;
            }
            glm::dvec3 sum( 0.0 );
            int        count = 0;
            mesh.EnumerateVertexVertices( vid,
                                          [&]( int32_t nb )
                                          {
                                              sum += mesh.GetVertex( nb );
                                              ++count;
                                          } );
            if ( count == 0 )
                continue;
            const double s = std::clamp( w * settings.Strength, 0.0, 1.0 );
            moves.emplace_back( vid, glm::mix( p, sum / static_cast<double>( count ), s ) );
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
