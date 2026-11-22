#pragma once

#include <cstdint>

namespace Desert::Assets::Serialization
{
    struct VFXSystemData;
}

namespace Desert::VFX
{
    struct VFXDebugStats;

    // THE RENDER SIDE OF ONE EFFECTS WORLD (VFX-07b), as the world sees it: the world (VFXWorld.hpp) owns one and
    // the renderer's particle pool (ParticleWorldGpu) implements it. Its own header so the renderer names the
    // interface without the world's clock, spawn and channel headers.
    class WorldGpuState
    {
    public:
        virtual ~WorldGpuState() = default;

        // The render side's columns of @p stats (GpuCounts, and per emitter GpuRead / Alive / Free / FramesOld /
        // Events).
        virtual void FillDebugStats( VFXDebugStats& stats ) const = 0;

        // VFX-PRESETS2-4 (UE FShaderCompilingManager / UNiagaraSystem::IsReadyToRun): whether every emitter
        // program @p system needs is built, so placement @p uuid can start over on it now. False = at least one
        // compiles in the background (this call starts what is not in flight yet): the world keeps playing the
        // system the placement plays and asks again on its next tick. The last structure asked for a placement
        // wins: a compile only an earlier structure of that placement wanted is dropped, its result discarded.
        virtual bool ProgramsReady( std::uint64_t uuid, const Assets::Serialization::VFXSystemData& system ) = 0;
    };
} // namespace Desert::VFX
