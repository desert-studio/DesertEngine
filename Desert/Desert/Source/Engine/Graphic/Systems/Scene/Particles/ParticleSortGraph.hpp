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
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct ParticleSortShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, AttributeFloats );
        DESERT_SHADER_PARAMETER( StorageBuffer, AttributeInts );
        DESERT_SHADER_PARAMETER( StorageBuffer, DrawLayout );
        DESERT_SHADER_PARAMETER( StorageBuffer, AliveList );
        DESERT_SHADER_PARAMETER( StorageBuffer, Counters );
        DESERT_SHADER_PARAMETER( StorageBuffer, SortKeys );
        DESERT_SHADER_PARAMETER( StorageBuffer, SortedAlive );
        DESERT_SHADER_PARAMETER( StorageBuffer, RibbonOrder );
    };

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

    // ParticleSort.shader's push block: SortRange, Step, ViewOrigin, ViewForward (four 16-byte rows).
    inline constexpr std::uint32_t kParticleSortPushBytes = 64;

    // One emitter as the sort sees it: its index in the view's emitter list, its cell's blend mode, its range's
    // capacity (MaxParticles) and where its drawn alive half and draw slot are.
    struct ParticleSortEmitter
    {
        std::uint32_t                   Emitter     = 0;
        Core::Formats::SurfaceBlendMode Blend       = Core::Formats::SurfaceBlendMode::Opaque;
        std::uint32_t                   Capacity    = 0;
        std::uint32_t                   AliveOffset = 0; // the drawn alive half's first entry in AliveList
        std::uint32_t                   Slot        = 0; // the drawn draw slot (index into Counters)
        bool Ribbon = false; // the ribbon link order (into RibbonOrder), whatever the blend
    };

    // One sorted emitter's share of the view's key buffer.
    struct ParticleSortRange
    {
        std::uint32_t Emitter     = 0;
        std::uint32_t AliveOffset = 0;
        std::uint32_t Slot        = 0;
        std::uint32_t KeyBase     = 0;
        std::uint32_t Length      = 0; // ParticleSortLength( Capacity ): the bitonic network's N
        bool          Ribbon      = false;
    };

    struct ParticleSortPlan
    {
        std::vector<ParticleSortRange> Ranges;
        std::uint32_t                  KeyCount = 0; // the view's SortKeys length (uvec4 each)
    };

    [[nodiscard]] inline ParticleSortPlan PlanParticleSort( const std::vector<ParticleSortEmitter>& emitters )
    {
        ParticleSortPlan plan;
        for ( const ParticleSortEmitter& emitter : emitters )
        {
            if ( ( !emitter.Ribbon && !ParticleSpritesSort( emitter.Blend ) ) || emitter.Capacity == 0 )
                continue;
            const std::uint32_t length = ParticleSortLength( emitter.Capacity );
            plan.Ranges.push_back(
                 { emitter.Emitter, emitter.AliveOffset, emitter.Slot, plan.KeyCount, length, emitter.Ribbon } );
            plan.KeyCount += length;
        }
        return plan;
    }

    [[nodiscard]] inline std::string ParticleSortPassName( const std::uint32_t emitter, const std::uint32_t stage,
                                                           const bool ribbon = false )
    {
        return ribbon ? std::format( "Particles: Ribbon Order {} {}", emitter, stage )
                      : std::format( "Particles: Sort {} {}", emitter, stage );
    }

    // The buffers one sort stage binds: the emitter's attribute columns and draw layout (where its particles are),
    // the pool's alive list and the emitter's draw slots (imported), and the view's two (transient).
    struct ParticleSortBuffers
    {
        RDG::BufferRef Floats;
        RDG::BufferRef Ints; // the RibbonIdBinding's column (a ribbon's Keys stage)
        RDG::BufferRef DrawLayout;
        RDG::BufferRef Alive;
        RDG::BufferRef Counters; // the emitter's draw slots
        RDG::BufferRef Keys;
        RDG::BufferRef Sorted;
        RDG::BufferRef Ribbon; // the view's RibbonOrder (uvec4 per alive-list entry)
    };

    // The view's transient sort buffers: SortKeys (one uvec4 per key of every sorted emitter) and SortedAlive
    // (the alive list's size, so a sorted emitter's indices sit at the same offset the draw's FirstVertex names).
    [[nodiscard]] inline std::pair<RDG::BufferRef, RDG::BufferRef>
    CreateParticleSortBuffers( RDG::Builder& graph, const ParticleSortPlan& plan, const std::uint64_t aliveBytes )
    {
        const RDG::BufferRef keys =
             graph.CreateBuffer( RDG::BufferDesc{ std::uint64_t{ plan.KeyCount } * 4u * sizeof( std::uint32_t ) },
                                 "ParticleSortKeys" );
        const RDG::BufferRef sorted = graph.CreateBuffer( RDG::BufferDesc{ aliveBytes }, "ParticleSortedAlive" );
        return { keys, sorted };
    }

    // One stage's block. Every stage binds all eight slots (one layout); the keys are written by every stage but
    // the last, which reads them and writes the sorted indices (a ribbon's Init and Final read them; its Scan
    // stages ping-pong through them and RibbonOrder). SortedAlive is declared written by every stage so the graph
    // never sees a read of a transient nobody produced and keeps the stages in their declared order.
    inline void DeclareParticleSortStage( RDG::PassBuilder&                                      pass,
                                          const std::shared_ptr<const RDG::ShaderBindingLayout>& layout,
                                          RDG::OtherRouteFill fill, const ParticleSortBuffers& buffers,
                                          const ParticleSortStageKind kind )
    {
        const bool writesSorted = kind == ParticleSortStageKind::Write || kind == ParticleSortStageKind::RibbonInit ||
                                  kind == ParticleSortStageKind::RibbonFinal;
        pass.Bindings( layout, std::move( fill ) )
             .Storage( ParticleSortShaderParameters::AttributeFloats, buffers.Floats, RDG::Access::StorageRead )
             .Storage( ParticleSortShaderParameters::AttributeInts, buffers.Ints, RDG::Access::StorageRead )
             .Storage( ParticleSortShaderParameters::DrawLayout, buffers.DrawLayout, RDG::Access::StorageRead )
             .Storage( ParticleSortShaderParameters::AliveList, buffers.Alive, RDG::Access::StorageRead )
             .Storage( ParticleSortShaderParameters::Counters, buffers.Counters, RDG::Access::StorageRead )
             .Storage( ParticleSortShaderParameters::SortKeys, buffers.Keys,
                       writesSorted ? RDG::Access::StorageRead : RDG::Access::StorageWrite )
             .Storage( ParticleSortShaderParameters::SortedAlive, buffers.Sorted, RDG::Access::StorageWrite )
             .Storage( ParticleSortShaderParameters::RibbonOrder, buffers.Ribbon, RDG::Access::StorageWrite )
             .PushConstantBytes( kParticleSortPushBytes );
    }
} // namespace Desert::Graphic::System
