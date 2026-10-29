#include <Engine/Graphic/GpuDriven/IndirectDrawSubmission.hpp>

#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RendererAPI.hpp>

#include <algorithm>

namespace Desert::Graphic::GpuDriven
{
    IndirectSubmissionMode SelectIndirectSubmission( const IndirectDrawCapabilities& capabilities )
    {
        if ( capabilities.MultiDrawIndirect )
            return { .Submission      = IndirectSubmission::DrawIndexedIndirectMulti,
                     .MaxDrawsPerCall = std::max( capabilities.MaxDrawIndirectCount, 1u ) };
        return { .Submission = IndirectSubmission::DrawIndexedIndirectPerRecord, .MaxDrawsPerCall = 1u };
    }

    Common::BoolResultStr RecordIndirectGroups( RendererAPI& api, const RDG::PassBindings& bindings,
                                                const IndirectSubmissionMode& mode, const PassDrawTable& pass,
                                                const CulledDraws& draws )
    {
        if ( !draws.IndirectDraws.IsValid() )
            return Common::MakeFormattedError( "{}: no IndirectDraws table", bindings.GetContext().GetPassName() );
        for ( size_t group = 0; group < pass.Groups.size(); ++group )
        {
            const IndirectGroup& indirect = pass.Groups[group];
            if ( indirect.BinCount == 0u )
                continue;
            if ( indirect.Pipeline == nullptr || indirect.Binding.Vertices == nullptr )
                return Common::MakeFormattedError( "{}: indirect group {} has no {}",
                                                   bindings.GetContext().GetPassName(), group,
                                                   indirect.Pipeline == nullptr ? "pipeline" : "geometry" );
            MeshIndirectGroupDraw draw;
            draw.Args       = draws.IndirectDraws;
            draw.ArgsOffset = uint64_t{ indirect.FirstBin } * kIndirectDrawStride;
            draw.MaxDraws   = indirect.BinCount;
            draw.Stride          = kIndirectDrawStride;
            draw.MaxDrawsPerCall = mode.MaxDrawsPerCall;
            if ( const Common::BoolResultStr recorded =
                      api.RenderMeshIndirectGroup( bindings, *indirect.Pipeline, indirect.Binding, draw );
                 !recorded )
                return recorded;
        }
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Graphic::GpuDriven
