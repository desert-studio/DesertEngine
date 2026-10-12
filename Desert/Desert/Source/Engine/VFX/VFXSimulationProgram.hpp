#pragma once

// THE SIMULATION PROGRAM OF A COMPILED EMITTER STACK (VFX-03e, R1 step 1).
//
// A compiled stack (VFXStackCompiler) is a `Domain Particle` fragment: it defines VFX_SimulateParticle and calls
// the six storage functions of Common/VFXParticleContract.glslh. This file is the engine's host for it, the one
// program a stack runs as (UE: the GPU simulation shader of an emitter, its dataset addressed per
// FNiagaraDataSet's layout). The particles of an emitter's range live PER ATTRIBUTE in the scene pool (SoA,
// layout-driven):
//
//   float component c of range slot s : Floats[FloatBase + c * Capacity + s]
//   int   component c of range slot s : Ints  [IntBase   + c * Capacity + s]
//
// where c is the component index the stack's layout gave the attribute (VFXDataSetLayout, compiled into the
// fragment) and Capacity the range's particle count. The free list, the alive lists and the counters are VFX-07's
// (Common/ParticlePool.glslh, built by the compact pass); a pool slot's liveness and particle id live in the Slots
// buffer, which this program writes (spawn: alive + id; a module's Kill: dead) and the compact reads.

#include <Engine/VFX/VFXStackCompiler.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Desert::VFX
{
    /// The program's storage bindings (set 0). Bound by name by the pipeline; the numbers are the program's text.
    namespace SimulationBinding
    {
        inline constexpr uint32_t Floats    = 0; ///< float F[]: the pool's float components (SoA per range)
        inline constexpr uint32_t Ints      = 1; ///< int I[]: the pool's int32 components (SoA per range)
        inline constexpr uint32_t Params    = 2; ///< vec4 P[]: BuildEmitterParams rows of every emitter
        inline constexpr uint32_t Curves    = 3; ///< float C[]: the curve atlases (BuildCurveAtlas)
        inline constexpr uint32_t StepTable = 4; ///< VFXStep per fixed step (VFXWorld's spawn plan)
        inline constexpr uint32_t FreeList  = 5; ///< VFX-07 free list (pool slot indices)
        inline constexpr uint32_t AliveList = 6; ///< VFX-07 alive lists, two halves per range
        inline constexpr uint32_t Counters  = 7; ///< ParticleDrawSlot x 2 per range
        inline constexpr uint32_t Slots     = 8; ///< VFXSlotState per pool slot: { Id, Alive }
    } // namespace SimulationBinding

    /// How many pool components one emitter range of @p capacity particles takes in each class.
    struct VFXPoolRange
    {
        uint64_t Floats = 0;
        uint64_t Ints   = 0;

        [[nodiscard]] bool operator==( const VFXPoolRange& ) const = default;
    };

    [[nodiscard]] VFXPoolRange PoolRangeOf( const VFXDataSetLayout& layout, uint32_t capacity );

    /// "VFX/Simulate/<Key as 16 hex digits>": the program's name and cache identity. The stack's Key covers its
    /// structure and its layout, so a layout change is a different program.
    [[nodiscard]] std::string SimulationProgramName( const VFXCompiledEmitter& compiled );

    /// Where the program sits for its includes and its cache entry: under the shader root, named by the key.
    [[nodiscard]] std::filesystem::path SimulationProgramPath( const VFXCompiledEmitter& compiled );

    /// The whole `.shader` text of the program: a `Compute` block that defines the contract's storage functions
    /// over the bindings above and runs one fixed step of one range (update every alive particle, spawn the step's
    /// budget). An error when the stack's text is not a Particle fragment.
    [[nodiscard]] Common::ResultStr<std::string> ComposeSimulationProgram( const VFXCompiledEmitter& compiled );

    /// The program's compute stage as the engine compiles it: ComposeSimulationProgram, parsed by DShaderParser.
    [[nodiscard]] Common::ResultStr<std::string> SimulationComputeSource( const VFXCompiledEmitter& compiled );

    /// SPIR-V of the program through the engine's ShaderCompiler (shader cache included); an error carries the
    /// compiler's message.
    [[nodiscard]] Common::ResultStr<std::vector<uint32_t>>
    CompileSimulationProgram( const VFXCompiledEmitter& compiled );
} // namespace Desert::VFX
