#include <Engine/VFX/VFXSimulationProgram.hpp>

#include <Engine/Core/Formats/Shader.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Core/ShaderCompiler/ShaderCompiler.hpp>

#include <Common/Core/Constants.hpp>

#include <format>

namespace Desert::VFX
{
    namespace
    {
        // The program around the fragment. {0} = the program's name, {1} = the fragment (the Particle block's
        // body, which includes the contract). Every storage function is the SoA address of the header comment; the
        // main is VFX-07's step over one range (ParticleSimulate.shader) with the stack in place of the fixed
        // integration.
        constexpr std::string_view kProgram =
             "Shader \"{0}\"\n"
             "{{\n"
             "    Compute\n"
             "    {{\n"
             "        LocalSize(64, 1, 1);\n"
             "\n"
             "        #include <Common/ParticlePool.glslh>\n"
             "\n"
             "        Buffer(0) Floats\n        {{\n            float F[];\n        }};\n"
             "        Buffer(1) Ints\n        {{\n            int I[];\n        }};\n"
             "        ReadBuffer(2) Params\n        {{\n            vec4 P[];\n        }};\n"
             "        ReadBuffer(3) Curves\n        {{\n            float C[];\n        }};\n"
             "\n"
             "        struct VFXStep\n"
             "        {{\n"
             "            uint IdBase;\n"
             "            uint Seed;\n"
             "            uint Budget;\n"
             "            uint ChannelFirst;\n"
             "            uint ChannelCount;\n"
             "        }};\n"
             "        ReadBuffer(4) StepTable\n        {{\n            VFXStep u_Steps[];\n        }};\n"
             "        ReadBuffer(5) FreeList\n        {{\n            uint u_Free[];\n        }};\n"
             "        Buffer(6) AliveList\n        {{\n            uint u_Alive[];\n        }};\n"
             "        ReadBuffer(7) Counters\n        {{\n            ParticleDrawSlot u_Slots[];\n        }};\n"
             "\n"
             "        struct VFXSlotState\n"
             "        {{\n"
             "            uint Id;    // the particle's id (the random stream's key)\n"
             "            uint Alive; // 1 while alive; the compact frees a slot whose Alive is 0\n"
             "        }};\n"
             "        Buffer(8) Slots\n        {{\n            VFXSlotState u_SlotState[];\n        }};\n"
             "\n"
             "        PushConstant PushConstants\n"
             "        {{\n"
             "            vec4  u_Time;   // x = the fixed step length (seconds), y = the emitter's age "
             "(seconds)\n"
             "            uvec4 u_Counts; // x = the range's capacity, y = step, z = the range's pool base, w = "
             "0\n"
             "            uvec4 u_Bases;  // x = float base, y = int base, z = param row base, w = curve base\n"
             "        }};\n"
             "\n"
             "{1}\n"
             "        vec4 VFX_Param( uint slot )\n"
             "        {{\n"
             "            return P[u_Bases.z + slot];\n"
             "        }}\n"
             "        float VFX_ReadFloat( uint particle, uint component )\n"
             "        {{\n"
             "            return F[u_Bases.x + component * u_Counts.x + particle];\n"
             "        }}\n"
             "        int VFX_ReadInt( uint particle, uint component )\n"
             "        {{\n"
             "            return I[u_Bases.y + component * u_Counts.x + particle];\n"
             "        }}\n"
             "        void VFX_WriteFloat( uint particle, uint component, float value )\n"
             "        {{\n"
             "            F[u_Bases.x + component * u_Counts.x + particle] = value;\n"
             "        }}\n"
             "        void VFX_WriteInt( uint particle, uint component, int value )\n"
             "        {{\n"
             "            I[u_Bases.y + component * u_Counts.x + particle] = value;\n"
             "        }}\n"
             "        float VFX_CurveLUT( uint index )\n"
             "        {{\n"
             "            return C[u_Bases.w + index];\n"
             "        }}\n"
             "\n"
             "        void main()\n"
             "        {{\n"
             "            const uint t = gl_GlobalInvocationID.x;\n"
             "            if ( t >= u_Counts.x )\n"
             "                return;\n"
             "            const uint base      = u_Counts.z;\n"
             "            const uint step      = u_Counts.y;\n"
             "            const uint slot      = step & 1u;\n"
             "            const uint alive     = u_Slots[slot].VertexCount / 6u;\n"
             "            const uint freeCount = u_Slots[slot].FreeCount;\n"
             "            VFXSim     sim;\n"
             "            sim.DeltaTime  = u_Time.x;\n"
             "            sim.EmitterAge = u_Time.y;\n"
             "            sim.Seed       = u_Steps[step].Seed;\n"
             "            sim.Step       = step;\n"
             "            if ( t < alive )\n"
             "            {{\n"
             "                const uint i   = u_Alive[2u * base + slot * u_Counts.x + t];\n"
             "                sim.ParticleId = u_SlotState[i].Id;\n"
             "                sim.Spawned    = false;\n"
             "                sim.Kill       = false;\n"
             "                VFX_SimulateParticle( i - base, sim );\n"
             "                if ( sim.Kill )\n"
             "                    u_SlotState[i].Alive = 0u; // the next compact frees it\n"
             "            }}\n"
             "            if ( t < min( u_Steps[step].Budget, freeCount ) )\n"
             "            {{\n"
             "                const uint i = u_Free[base + freeCount - 1u - t];\n"
             "                // Appended after the alive entries: compact step+1 scans alive + spawned.\n"
             "                u_Alive[2u * base + slot * u_Counts.x + alive + t] = i;\n"
             "                sim.ParticleId = u_Steps[step].IdBase + t;\n"
             "                sim.Spawned    = true;\n"
             "                sim.Kill       = false;\n"
             "                VFX_SimulateParticle( i - base, sim );\n"
             "                u_SlotState[i].Id    = sim.ParticleId;\n"
             "                u_SlotState[i].Alive = sim.Kill ? 0u : 1u;\n"
             "            }}\n"
             "        }}\n"
             "    }}\n"
             "}}\n";

        std::filesystem::path ProgramPath( const VFXCompiledEmitter& compiled )
        {
            // Under the shader root, so the includer resolves <Common/...> exactly as for a file program.
            return Common::Constants::Path::ShaderDir() / "VFX" /
                   std::format( "Simulate_{:016x}.shader", compiled.Key );
        }
    } // namespace

    VFXPoolRange PoolRangeOf( const VFXDataSetLayout& layout, uint32_t capacity )
    {
        return { uint64_t{ layout.TotalFloatComponents } * capacity,
                 uint64_t{ layout.TotalIntComponents } * capacity };
    }

    std::string SimulationProgramName( const VFXCompiledEmitter& compiled )
    {
        return std::format( "VFX/Simulate/{:016x}", compiled.Key );
    }

    Common::ResultStr<std::string> ComposeSimulationProgram( const VFXCompiledEmitter& compiled )
    {
        const auto parsed = Core::Preprocess::DShaderParser::Parse( compiled.ShaderText );
        if ( !parsed.IsSuccess() )
            return Common::MakeFormattedError<std::string>( "the stack '{}' does not parse: {}",
                                                            compiled.ShaderName, parsed.GetError() );
        const std::string& fragment = parsed.GetValue().Meta.ParticleSource;
        if ( fragment.empty() )
            return Common::MakeFormattedError<std::string>( "the stack '{}' is not a Particle fragment",
                                                            compiled.ShaderName );
        return Common::MakeSuccess( std::format( kProgram, SimulationProgramName( compiled ), fragment ) );
    }

    Common::ResultStr<std::string> SimulationComputeSource( const VFXCompiledEmitter& compiled )
    {
        auto program = ComposeSimulationProgram( compiled );
        if ( !program.IsSuccess() )
            return program;
        const auto parsed = Core::Preprocess::DShaderParser::Parse( program.GetValue() );
        if ( !parsed.IsSuccess() )
            return Common::MakeFormattedError<std::string>( "the simulation program of '{}' does not parse: {}",
                                                            compiled.ShaderName, parsed.GetError() );
        const auto& passes = parsed.GetValue().Passes;
        if ( passes.empty() )
            return Common::MakeFormattedError<std::string>( "the simulation program of '{}' has no pass",
                                                            compiled.ShaderName );
        const auto stage = passes.front().Stages.find( Core::Formats::ShaderStage::Compute );
        if ( stage == passes.front().Stages.end() )
            return Common::MakeFormattedError<std::string>( "the simulation program of '{}' has no Compute stage",
                                                            compiled.ShaderName );
        return Common::MakeSuccess( stage->second );
    }

    Common::ResultStr<std::vector<uint32_t>> CompileSimulationProgram( const VFXCompiledEmitter& compiled )
    {
        const auto source = SimulationComputeSource( compiled );
        if ( !source.IsSuccess() )
            return Common::MakeError<std::vector<uint32_t>>( source.GetError() );
        return Core::ShaderCompiler::CompileGLSLToSPIRV( Core::Formats::ShaderStage::Compute, source.GetValue(),
                                                         ProgramPath( compiled ).string() );
    }
} // namespace Desert::VFX
