#include <Engine/Graphic/GpuDriven/IndirectDrawSubmission.hpp>

#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RendererAPI.hpp>

#include <Common/Core/Profiler.hpp>

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

    Common::BoolResultStr RecordIndirectGroups( RendererAPI&                              api,
                                                std::span<const RDG::PassBindings* const> groupBindings,
                                                std::span<const MaterialExecutor* const>  groupMaterials,
                                                const IndirectSubmissionMode& mode, const PassDrawTable& pass,
                                                const CulledDraws&                        draws,
                                                std::span<const GraphicsPipeline* const> groupPipelines )
    {
        if ( groupBindings.size() != pass.Groups.size() || groupMaterials.size() != pass.Groups.size() ||
             ( !groupPipelines.empty() && groupPipelines.size() != pass.Groups.size() ) )
            return Common::MakeFormattedError(
                 "indirect groups: {} blocks, {} carriers and {} pipelines for {} groups", groupBindings.size(),
                 groupMaterials.size(), groupPipelines.size(), pass.Groups.size() );
        for ( size_t group = 0; group < pass.Groups.size(); ++group )
        {
            const IndirectGroup& indirect = pass.Groups[group];
            if ( indirect.BinCount == 0u )
                continue;
            const GraphicsPipeline* const pipeline =
                 groupPipelines.empty() ? indirect.Pipeline : groupPipelines[group];
            if ( !groupPipelines.empty() && pipeline == nullptr )
                continue; // not drawn by this call (the caller's override)
            const RDG::PassBindings* bindings = groupBindings[group];
            if ( bindings == nullptr )
                return Common::MakeFormattedError( "indirect group {} has no binding block", group );
            const std::string_view name = bindings->GetContext().GetPassName();
            if ( !draws.IndirectDraws.IsValid() )
                return Common::MakeFormattedError( "{}: no IndirectDraws table", name );
            if ( pipeline == nullptr || indirect.Binding.Vertices == nullptr )
                return Common::MakeFormattedError( "{}: indirect group {} has no {}", name, group,
                                                   pipeline == nullptr ? "pipeline" : "geometry" );
            MeshIndirectGroupDraw draw;
            draw.Args = draws.IndirectDraws;
            // This view's records start at FirstIndirectDraw (the views of a pass share one table,
            // InstanceCulling).
            draw.ArgsOffset = ( uint64_t{ draws.FirstIndirectDraw } + indirect.FirstBin ) * kIndirectDrawStride;
            draw.MaxDraws   = indirect.BinCount;
            draw.Stride          = kIndirectDrawStride;
            draw.MaxDrawsPerCall = mode.MaxDrawsPerCall;
            draw.Material        = groupMaterials[group];
            draw.VisibleRows     = draws.VisibleRowEnd;
            if ( Common::Profiling::MeshDrawSplitOn() && pass.BinLabels.size() == pass.Bins.size() )
                draw.SplitLabels = std::span<const DrawSplitLabel>( pass.BinLabels )
                                        .subspan( indirect.FirstBin, indirect.BinCount );
            if ( const Common::BoolResultStr recorded =
                      api.RenderMeshIndirectGroup( *bindings, *pipeline, indirect.Binding, draw );
                 !recorded )
                return recorded;
        }
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Graphic::GpuDriven
