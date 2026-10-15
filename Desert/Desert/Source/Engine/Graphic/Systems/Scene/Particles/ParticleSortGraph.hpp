#pragma once

#include "ParticleSortKey.hpp"

#include <Engine/Core/Formats/ShaderProgramMeta.hpp>
#include <Engine/Graphic/RDG/RDGBindingDecl.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Graphic::System
{
    // THE GRAPH SIDE of the translucent sprite sort (VFX-08), device-free so RenderGraphCompile builds the same
    // nodes from the same declarations. Per VIEW (the key is the view depth): every emitter whose sprite cell
    // blends (Translucent / Additive) gets one Compute node per ParticleSortStages stage, named
    // "Particles: Sort {emitter} {stage}"; an Opaque / Masked emitter is drawn from AliveList unsorted and gets
    // none. The nodes read the pool, the alive list and the emitter's draw slot (written by the last Compact),
    // write the view's SortKeys and SortedAlive (graph-transient), and the draw then binds SortedAlive in place of
    // AliveList: the graph orders Compact -> Sort -> draw from those declarations alone.

    [[nodiscard]] constexpr bool ParticleSpritesSort( const Core::Formats::SurfaceBlendMode blend )
    {
        return Core::Formats::IsTranslucentBlend( blend );
    }

    // ParticleSort.shader's push block: u_Range, u_Step, u_ViewOrigin, u_ViewForward (four 16-byte rows).
    inline constexpr std::uint32_t kParticleSortPushBytes = 64;

    // What one sort orders an emitter's drawn alive half by (ParticleSort.shader u_Step.w):
    //   View   - back to front along the view (a translucent / additive sprite or mesh draw), into SortedAlive;
    //   Ribbon - by RibbonID, then newest first by normalised age (UE's ribbon link order), into RibbonOrder:
    //            the ribbon draw joins consecutive entries of one RibbonID into segments.
    // An emitter with a blended draw and a ribbon draw gets both, each into its own buffer at the same offset.
    enum class ParticleSortKind : std::uint32_t
    {
        View   = 0,
        Ribbon = 1,
    };

    // One emitter as the sort sees it: its index in the view's emitter list, its cell's blend mode (read for a View
    // sort), its range's capacity (MaxParticles), where its drawn alive half and draw slot are, and the order.
    struct ParticleSortEmitter
    {
        std::uint32_t                   Emitter     = 0;
        Core::Formats::SurfaceBlendMode Blend       = Core::Formats::SurfaceBlendMode::Opaque;
        std::uint32_t                   Capacity    = 0;
        std::uint32_t                   AliveOffset = 0; // the drawn alive half's first entry in AliveList
        std::uint32_t                   Slot        = 0; // the drawn draw slot (index into Counters)
        ParticleSortKind                Kind        = ParticleSortKind::View;
    };

    // One sorted emitter's share of the view's key buffer.
    struct ParticleSortRange
    {
        std::uint32_t Emitter     = 0;
        std::uint32_t AliveOffset = 0;
        std::uint32_t Slot        = 0;
        std::uint32_t KeyBase     = 0;
        std::uint32_t Length      = 0; // ParticleSortLength( Capacity ): the bitonic network's N
        ParticleSortKind Kind     = ParticleSortKind::View;
    };

    struct ParticleSortPlan
    {
        std::vector<ParticleSortRange> Ranges;
        std::uint32_t                  KeyCount = 0; // the view's SortKeys length (uvec2 each)
    };

    [[nodiscard]] inline ParticleSortPlan PlanParticleSort( const std::vector<ParticleSortEmitter>& emitters )
    {
        ParticleSortPlan plan;
        for ( const ParticleSortEmitter& emitter : emitters )
        {
            const bool sorts = emitter.Kind == ParticleSortKind::Ribbon || ParticleSpritesSort( emitter.Blend );
            if ( !sorts || emitter.Capacity == 0 )
                continue;
            const std::uint32_t length = ParticleSortLength( emitter.Capacity );
            plan.Ranges.push_back(
                 { emitter.Emitter, emitter.AliveOffset, emitter.Slot, plan.KeyCount, length, emitter.Kind } );
            plan.KeyCount += length;
        }
        return plan;
    }

    [[nodiscard]] inline std::string ParticleSortPassName( const std::uint32_t emitter, const std::uint32_t stage,
                                                           const ParticleSortKind kind = ParticleSortKind::View )
    {
        return kind == ParticleSortKind::View ? std::format( "Particles: Sort {} {}", emitter, stage )
                                              : std::format( "Particles: Ribbon Order {} {}", emitter, stage );
    }

    // The buffers one sort stage binds: the emitter's attribute columns and draw layout (where its particles are),
    // the pool's alive list and the emitter's draw slots (imported), and the view's two (transient).
    struct ParticleSortBuffers
    {
        RDG::BufferRef Floats;
        RDG::BufferRef DrawLayout;
        RDG::BufferRef Alive;
        RDG::BufferRef Counters; // the emitter's draw slots
        RDG::BufferRef Keys;
        RDG::BufferRef Sorted; // SortedAlive for a View sort, RibbonOrder for a Ribbon sort
        RDG::BufferRef Ints;   // the emitter's int columns (RibbonID); bound wherever the stage's layout has the slot
    };

    // The view's transient sort buffers: SortKeys (one uvec2 per key of every sorted emitter) and SortedAlive
    // (the alive list's size, so a sorted emitter's indices sit at the same offset the draw's FirstVertex names).
    [[nodiscard]] inline std::pair<RDG::BufferRef, RDG::BufferRef>
    CreateParticleSortBuffers( RDG::Builder& graph, const ParticleSortPlan& plan, const std::uint64_t aliveBytes )
    {
        const RDG::BufferRef keys =
             graph.CreateBuffer( RDG::BufferDesc{ std::uint64_t{ plan.KeyCount } * 2u * sizeof( std::uint32_t ) },
                                 "ParticleSortKeys" );
        const RDG::BufferRef sorted = graph.CreateBuffer( RDG::BufferDesc{ aliveBytes }, "ParticleSortedAlive" );
        return { keys, sorted };
    }

    // The view's ribbon order (ParticleSortKind::Ribbon's output): the alive list's size, the same offsets.
    [[nodiscard]] inline RDG::BufferRef CreateParticleRibbonOrderBuffer( RDG::Builder&       graph,
                                                                         const std::uint64_t aliveBytes )
    {
        return graph.CreateBuffer( RDG::BufferDesc{ aliveBytes }, "ParticleRibbonOrder" );
    }

    // One stage's block. Every stage binds all six slots (one layout); the keys are written by every stage but
    // the last, which reads them and writes the sorted indices. SortedAlive is declared written by every stage so
    // the graph never sees a read of a transient nobody produced and keeps the stages in their declared order.
    inline void DeclareParticleSortStage( RDG::PassBuilder&                                      pass,
                                          const std::shared_ptr<const RDG::ShaderBindingLayout>& layout,
                                          RDG::OtherRouteFill fill, const ParticleSortBuffers& buffers,
                                          const ParticleSortStageKind kind )
    {
        const bool writesSorted = kind == ParticleSortStageKind::Write;
        auto       block        = pass.Bindings( layout, std::move( fill ) );
        if ( buffers.Ints.IsValid() )
            block.Storage( "AttributeInts", buffers.Ints, RDG::Access::StorageRead );
        block
             .Storage( "AttributeFloats", buffers.Floats, RDG::Access::StorageRead )
             .Storage( "DrawLayout", buffers.DrawLayout, RDG::Access::StorageRead )
             .Storage( "AliveList", buffers.Alive, RDG::Access::StorageRead )
             .Storage( "Counters", buffers.Counters, RDG::Access::StorageRead )
             .Storage( "SortKeys", buffers.Keys,
                       writesSorted ? RDG::Access::StorageRead : RDG::Access::StorageWrite )
             .Storage( "SortedAlive", buffers.Sorted, RDG::Access::StorageWrite )
             .PushConstantBytes( kParticleSortPushBytes );
    }
} // namespace Desert::Graphic::System
