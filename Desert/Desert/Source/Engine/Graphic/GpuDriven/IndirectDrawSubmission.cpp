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

    Common::BoolResultStr RecordIndirectGroups( RendererAPI&                             api,
                                                std::span<const RDG::PassBindings* const> groupBindings,
                                                std::span<const MaterialExecutor* const>  groupMaterials,
                                                const IndirectSubmissionMode& mode, const PassDrawTable& pass,
                                                const CulledDraws& draws )
    {
        if ( groupBindings.size() != pass.Groups.size() || groupMaterials.size() != pass.Groups.size() )
            return Common::MakeFormattedError( "indirect groups: {} blocks and {} carriers for {} groups",
                                               groupBindings.size(), groupMaterials.size(), pass.Groups.size() );
        for ( size_t group = 0; group < pass.Groups.size(); ++group )
        {
            const IndirectGroup& indirect = pass.Groups[group];
            if ( indirect.BinCount == 0u )
                continue;
            const RDG::PassBindings* bindings = groupBindings[group];
            if ( bindings == nullptr )
                return Common::MakeFormattedError( "indirect group {} has no binding block", group );
            const std::string_view name = bindings->GetContext().GetPassName();
            if ( !draws.IndirectDraws.IsValid() )
                return Common::MakeFormattedError( "{}: no IndirectDraws table", name );
            if ( indirect.Pipeline == nullptr || indirect.Binding.Vertices == nullptr )
                return Common::MakeFormattedError( "{}: indirect group {} has no {}", name, group,
                                                   indirect.Pipeline == nullptr ? "pipeline" : "geometry" );
            MeshIndirectGroupDraw draw;
            draw.Args = draws.IndirectDraws;
            // This view's records start at FirstIndirectDraw (the views of a pass share one table, InstanceCulling).
            draw.ArgsOffset      = ( uint64_t{ draws.FirstIndirectDraw } + indirect.FirstBin ) * kIndirectDrawStride;
            draw.MaxDraws        = indirect.BinCount;
            draw.Stride          = kIndirectDrawStride;
            draw.MaxDrawsPerCall = mode.MaxDrawsPerCall;
            draw.Material        = groupMaterials[group];
            draw.VisibleRows     = draws.VisibleRowEnd;
            if ( const Common::BoolResultStr recorded =
                      api.RenderMeshIndirectGroup( *bindings, *indirect.Pipeline, indirect.Binding, draw );
                 !recorded )
                return recorded;
        }
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Graphic::GpuDriven
