#include <Engine/Graphic/GpuDriven/IndirectDrawSubmission.hpp>

#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RendererAPI.hpp>

#include <algorithm>

namespace Desert::Graphic::GpuDriven
{
    IndirectSubmissionMode SelectIndirectSubmission( const IndirectDrawCapabilities& capabilities )
    {
        if ( capabilities.DrawIndirectCount )
            return { .Submission      = IndirectSubmission::DrawIndexedIndirectCount,
                     .MaxDrawsPerCall = std::max( capabilities.MaxDrawIndirectCount, 1u ) };
        if ( capabilities.MultiDrawIndirect )
            return { .Submission      = IndirectSubmission::DrawIndexedIndirectFixed,
                     .MaxDrawsPerCall = std::max( capabilities.MaxDrawIndirectCount, 1u ) };
        return { .Submission = IndirectSubmission::DrawIndexedIndirectPerRecord, .MaxDrawsPerCall = 1u };
    }

    Common::BoolResultStr RecordIndirectGroups( RendererAPI& api, const RDG::PassBindings& bindings,
                                                const IndirectSubmissionMode& mode, const PassDrawTable& pass,
                                                const CulledDraws& draws )
    {
        const bool counted = mode.Submission == IndirectSubmission::DrawIndexedIndirectCount;
        if ( counted != draws.GroupDrawCounts.IsValid() )
            return Common::MakeFormattedError(
                 "{}: the {} path {} a GroupDrawCounts buffer", bindings.GetContext().GetPassName(),
                 counted ? "drawIndirectCount" : "fixed-table", counted ? "needs" : "takes no" );
        if ( !draws.IndirectDraws.IsValid() )
            return Common::MakeFormattedError( "{}: no IndirectDraws table", bindings.GetContext().GetPassName() );
        for ( size_t group = 0; group < pass.Groups.size(); ++group )
        {
            const IndirectGroup& indirect = pass.Groups[group];
            if ( indirect.BinCount == 0u )
                continue;
            if ( indirect.Pipeline == nullptr || indirect.Geometry == nullptr )
                return Common::MakeFormattedError( "{}: indirect group {} has no {}",
                                                   bindings.GetContext().GetPassName(), group,
                                                   indirect.Pipeline == nullptr ? "pipeline" : "geometry" );
            MeshIndirectGroupDraw draw;
            draw.Args       = draws.IndirectDraws;
            draw.ArgsOffset = uint64_t{ indirect.FirstBin } * kIndirectDrawStride;
            draw.MaxDraws   = indirect.BinCount;
            draw.Stride     = kIndirectDrawStride;
            if ( counted )
            {
                draw.Counts      = draws.GroupDrawCounts;
                draw.CountOffset = uint64_t{ group } * sizeof( uint32_t );
                // vkCmdDrawIndexedIndirectCount's maxDrawCount bounds the GPU count, it is not split: a group
                // past the device limit is refused rather than drawn short.
                if ( indirect.BinCount > mode.MaxDrawsPerCall )
                    return Common::MakeFormattedError(
                         "{}: indirect group {} of {} bins passes the device's maxDrawIndirectCount {}",
                         bindings.GetContext().GetPassName(), group, indirect.BinCount, mode.MaxDrawsPerCall );
            }
            draw.MaxDrawsPerCall = mode.MaxDrawsPerCall;
            if ( const Common::BoolResultStr recorded =
                      api.RenderMeshIndirectGroup( bindings, *indirect.Pipeline, *indirect.Geometry, draw );
                 !recorded )
                return recorded;
        }
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Graphic::GpuDriven
