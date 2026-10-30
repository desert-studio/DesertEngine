#include "MeshWeldEdgesOperation.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/Operations/MergeCoincidentMeshEdges.hpp"
#include "Engine/Geometry/MeshRegionOperation.hpp"

#include <spdlog/fmt/fmt.h>

#include <cmath>
#include <numbers>

namespace Desert::Geometry
{
    Common::ResultStr<DeformOutcome> WeldMeshEdges( const DynamicMesh3& input, const WeldEdgesSettings& settings )
    {
        if ( !std::isfinite( settings.Tolerance ) || settings.Tolerance < 1e-8f || settings.Tolerance > 1000.0f )
            return Common::MakeFormattedError<DeformOutcome>(
                 "Weld Edges: Tolerance {} cm is outside [1e-8, 1000]", settings.Tolerance );
        DeformOutcome            out{ input };
        MergeCoincidentMeshEdges merger( &out.Mesh );
        merger.m_MergeVertexTolerance    = static_cast<double>( settings.Tolerance );
        merger.m_OnlyUniquePairs         = settings.OnlyUniquePairs;
        merger.m_bWeldAttrsOnMergedEdges = settings.WeldAttributes;
        // UE FWeldMeshEdgesOp's split-attribute thresholds: UVs 0.01 apart, normals / tangents 0.1 degrees.
        merger.m_SplitAttributeWelder.m_UVDistSqrdThreshold = 0.01f * 0.01f;
        merger.m_SplitAttributeWelder.m_NormalVecDotThreshold =
             std::abs( 1.0f - std::cos( 0.1f * std::numbers::pi_v<float> / 180.0f ) );
        merger.m_SplitAttributeWelder.m_TangentVecDotThreshold =
             merger.m_SplitAttributeWelder.m_NormalVecDotThreshold;
        if ( !merger.Apply() )
            return Common::MakeFormattedError<DeformOutcome>(
                 "Weld Edges: the merge failed with {} of {} open edges left", merger.m_FinalNumBoundaryEdges,
                 merger.m_InitialNumBoundaryEdges );
        // Each merge turns two open edges into one interior edge.
        const int32_t pairs = ( merger.m_InitialNumBoundaryEdges - merger.m_FinalNumBoundaryEdges ) / 2;
        if ( pairs > 0 )
            if ( auto tangents = RecomputeTangentSpace( out.Mesh, "Weld Edges" ); !tangents.IsSuccess() )
                return Common::MakeError<DeformOutcome>( tangents.GetError() );
        out.MovedVertices  = pairs;
        out.PinnedVertices = merger.m_FinalNumBoundaryEdges;
        out.Report = fmt::format( "{} open edges, {} edge pairs welded (tolerance {} cm), {} open edges remain",
                                  merger.m_InitialNumBoundaryEdges, pairs, settings.Tolerance,
                                  merger.m_FinalNumBoundaryEdges );
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Geometry
