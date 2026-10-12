#pragma once

#include <Engine/Graphic/RDG/RDGBindingDecl.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace Desert::Graphic::System
{
    // THE GRAPH SIDE of a Spawn+Update node (VFX-03g), device-free so RenderGraphCompile builds the same blocks
    // from the same declarations ParticleRenderer::DeclareSimulateBindings makes. One block per emitter that runs
    // the step, bound to the layout of the program that emitter dispatches: the legacy ParticleSimulate (AoS
    // Particles + Spawn from Channel) or its compiled stack's simulation program (VFX/Simulate/<key>, SoA Floats /
    // Ints of the pool + its Params rows and Curves atlas, VFX::SimulationBinding).

    // The scene pool as this frame's graph sees it. Floats / Ints are invalid while no compiled stack plays.
    struct ParticlePoolRefs
    {
        RDG::BufferRef Particles;
        RDG::BufferRef Slots;
        RDG::BufferRef Floats;
        RDG::BufferRef Ints;
        RDG::BufferRef Free;
        RDG::BufferRef Alive;
    };

    // One emitter's buffers in this frame's graph and the program it dispatches.
    struct ParticleSimEmitterRefs
    {
        RDG::BufferRef Steps;
        RDG::BufferRef Counters;
        RDG::BufferRef Args;
        RDG::BufferRef Channel; // the legacy program's Spawn from Channel particles
        RDG::BufferRef Params;  // a compiled stack's parameter rows
        RDG::BufferRef Curves;  // a compiled stack's curve atlas
        bool           Stack = false;
        std::shared_ptr<const RDG::ShaderBindingLayout> Layout; // the layout of the program it dispatches
        RDG::OtherRouteFill                             Fill;
        std::uint32_t                                   PushBytes = 0;
    };

    inline void DeclareParticleSimulateBlock( RDG::PassBuilder& pass, const ParticlePoolRefs& pool,
                                              const ParticleSimEmitterRefs& emitter )
    {
        if ( emitter.Stack )
        {
            pass.Bindings( emitter.Layout, emitter.Fill )
                 .Storage( "Floats", pool.Floats, RDG::Access::StorageWrite )
                 .Storage( "Ints", pool.Ints, RDG::Access::StorageWrite )
                 .Storage( "Params", emitter.Params, RDG::Access::StorageRead )
                 .Storage( "Curves", emitter.Curves, RDG::Access::StorageRead )
                 .Storage( "StepTable", emitter.Steps, RDG::Access::StorageRead )
                 .Storage( "FreeList", pool.Free, RDG::Access::StorageRead )
                 .Storage( "AliveList", pool.Alive, RDG::Access::StorageWrite )
                 .Storage( "Counters", emitter.Counters, RDG::Access::StorageRead )
                 .Storage( "Slots", pool.Slots, RDG::Access::StorageWrite )
                 .PushConstantBytes( emitter.PushBytes );
        }
        else
        {
            pass.Bindings( emitter.Layout, emitter.Fill )
                 .Storage( "Particles", pool.Particles, RDG::Access::StorageWrite )
                 .Storage( "StepTable", emitter.Steps, RDG::Access::StorageRead )
                 .Storage( "FreeList", pool.Free, RDG::Access::StorageRead )
                 .Storage( "AliveList", pool.Alive, RDG::Access::StorageWrite )
                 .Storage( "Counters", emitter.Counters, RDG::Access::StorageRead )
                 .Storage( "ChannelSpawns", emitter.Channel, RDG::Access::StorageRead )
                 .Storage( "Slots", pool.Slots, RDG::Access::StorageWrite )
                 .PushConstantBytes( emitter.PushBytes );
        }
        // As many groups as the step's Dispatch Args wrote.
        pass.Read( emitter.Args, RDG::Access::IndirectArgs );
    }

    // The Spawn+Update node of one step: a block per emitter running it, in order. Returns the blocks declared
    // (the executor's GetBindingBlock indices are 0 .. count - 1 in the same order).
    inline std::uint32_t DeclareParticleSimulateStep( RDG::PassBuilder& pass, const ParticlePoolRefs& pool,
                                                      std::span<const ParticleSimEmitterRefs> running )
    {
        for ( const ParticleSimEmitterRefs& emitter : running )
            DeclareParticleSimulateBlock( pass, pool, emitter );
        return static_cast<std::uint32_t>( running.size() );
    }
} // namespace Desert::Graphic::System
