// DesertAsset {"Kind":"Shader","Guid":"5194037468f4cb1e73120f7432ee6cf2","Versions":{"SHDR":1},"Dependencies":[]}
Shader "ParticleSimulate"
{
    Compute
    {
        // "Particles: Spawn+Update {s}" (VFX-07, VFX-HOST): one fixed VFX step of ONE emitter over its range of the
        // world's particle pool - the HOST program of the emitter's compiled module stack (Engine/VFX
        // VFXStackCompiler). The stack arrives as Generated/VFXEmitterStack.glslh, substituted per emitter by the
        // ShaderVariant the renderer acquires (ParticleWorldGpu StackHostSource); the file on disk is the empty
        // stack. The host defines the contract's storage (Common/VFXParticleContract.glslh): the attributes are
        // SoA in the emitter's Floats / Ints buffers, component c of range-local particle n at c * Count + n
        // (UE FNiagaraDataBuffer), the parameter rows in Params, the curve atlas in Curves.
        //   * update: thread t < alive count runs the stack on the particle at AliveList[Base + t];
        //   * spawn: thread t < min(budget, free count) runs it as a newborn (the spawn group, then the update
        //     group, UE's spawn-and-update in one step) on FreeList[Base + FreeCount - 1 - t] with id IdBase + t.
        // A module's Kill (UpdateAge at Lifetime) clears the slot's Alive flag: the next compact frees the particle.
        // The attributes are the particle's ONE store: the sprite vertex path and the sort read them where the
        // emitter's ParticleDrawLayout says (Common/ParticleDrawLayout.glslh); the host keeps only the pool slot's
        // id and alive flag (Common/ParticleState.glslh). A Spawn from Channel newborn hands its record's direction, colour and scalars to the
        // stack in VFXSim (the Initialize / velocity modules read them, UE's Data Channel read); only the bound
        // position is the host's, as the shape modules place a newborn around zero. Positions are centimetres: a World-space emitter's newborn is moved to the emitter, a
        // Local one is drawn at emitter + Position every step.
        LocalSize(64, 1, 1);

        #include <Common/ParticleState.glslh>
        #include <Common/ParticlePool.glslh>

        // The pool's bookkeeping (Common/ParticleState.glslh): id + alive flag, none of the particle's data.
        Buffer(0) PoolSlots
        {
            ParticleSlot u_PoolSlots[];
        };

        struct VFXStep
        {
            uint IdBase; // id of the first particle this step may spawn
            uint Seed;   // the emitter instance's seed (system xor entity xor emitter)
            uint Budget; // how many particles this step spawns
            uint ChannelFirst; // VFX-10: spawn t < ChannelCount is Spawn from Channel particle ChannelFirst + t
            uint ChannelCount;
        };

        ReadBuffer(1) StepTable
        {
            VFXStep u_Steps[];
        };

        ReadBuffer(2) FreeList
        {
            uint u_Free[];
        };

        Buffer(3) AliveList
        {
            uint u_Alive[];
        };

        ReadBuffer(4) Counters
        {
            ParticleDrawSlot u_Slots[];
        };

        // VFX-10: this tick's Spawn from Channel particles, one record per particle (VFXWorld PlanEmitterSteps).
        struct VFXChannelSpawn
        {
            vec4 Position;  // xyz world cm, w = 1 when the module binds a Position field
            vec4 Direction; // xyz start velocity direction, w = 1 when bound
            vec4 Color;     // linear rgba, bound when Scalars.z = 1
            vec4 Scalars;   // x = lifetime seconds, y = base-size scale (1 when unbound), z = 1 when the colour is
                            // bound, w = 1 when the lifetime is
        };

        ReadBuffer(5) ChannelSpawns
        {
            VFXChannelSpawn u_ChannelSpawns[];
        };

        // The emitter's attributes, SoA by component class (VFXDataSetLayout), one column per component.
        Buffer(6) AttributeFloats
        {
            float u_AttrFloats[];
        };

        Buffer(7) AttributeInts
        {
            int u_AttrInts[];
        };

        // One vec4 per parameter slot (BuildEmitterParams): module input values, never in the program text.
        ReadBuffer(8) StackParams
        {
            vec4 u_StackParams[];
        };

        // The system's curve atlas (BuildCurveAtlas).
        ReadBuffer(9) StackCurves
        {
            float u_StackCurves[];
        };

        PushConstant PushConstants
        {
            vec4  u_EmitterPos; // xyz = emitter world pos (cm), w = the fixed step length (seconds)
            uvec4 u_Counts;     // x = the range's particle count, y = step, z = the range's pool base, w = local
            uvec4 u_Stack;      // x = the world's serial of this tick's step 0 (the random streams' step), yzw = 0
        };

        #include <Generated/VFXEmitterStack.glslh>

        vec4 VFX_Param( uint slot )
        {
            return u_StackParams[slot];
        }
        float VFX_ReadFloat( uint particle, uint component )
        {
            return u_AttrFloats[component * u_Counts.x + particle];
        }
        int VFX_ReadInt( uint particle, uint component )
        {
            return u_AttrInts[component * u_Counts.x + particle];
        }
        void VFX_WriteFloat( uint particle, uint component, float value )
        {
            u_AttrFloats[component * u_Counts.x + particle] = value;
        }
        void VFX_WriteInt( uint particle, uint component, int value )
        {
            u_AttrInts[component * u_Counts.x + particle] = value;
        }
        float VFX_CurveLUT( uint index )
        {
            return u_StackCurves[index];
        }

        vec3 ReadVec3( uint n, uint c )
        {
            return vec3( VFX_ReadFloat( n, c ), VFX_ReadFloat( n, c + 1u ), VFX_ReadFloat( n, c + 2u ) );
        }
        void WriteVec3( uint n, uint c, vec3 v )
        {
            VFX_WriteFloat( n, c, v.x );
            VFX_WriteFloat( n, c + 1u, v.y );
            VFX_WriteFloat( n, c + 2u, v.z );
        }

        // Runs the stack on pool particle @p i: its attributes, then its slot (id, alive).
        void Run( uint i, bool spawned, uint id, uint step, VFXChannelSpawn channel )
        {
            const uint n     = i - u_Counts.z; // range-local: the attribute columns are the emitter's
            const bool local = u_Counts.w == 1u;

            VFXSim sim;
            sim.DeltaTime  = u_EmitterPos.w;
            sim.EmitterAge = 0.0;
            sim.Seed       = u_Steps[step].Seed;
            sim.ParticleId = id;
            sim.Step       = u_Stack.x + step;
            sim.Spawned    = spawned;
            sim.Kill       = false;
            sim.ChannelDirection = channel.Direction;
            sim.ChannelColor     = channel.Color;
            sim.ChannelScalars   = channel.Scalars;
            VFX_SimulateParticle( n, sim );

            vec3 position = vec3( 0.0 );
#ifdef VFX_ATTR_POSITION
            position = ReadVec3( n, VFX_ATTR_POSITION );
            if ( spawned )
            {
                // A newborn's Position is relative to the emitter (the shape modules place it around zero).
                if ( channel.Position.w > 0.5 )
                    position = local ? channel.Position.xyz - u_EmitterPos.xyz : channel.Position.xyz;
                else if ( !local )
                    position += u_EmitterPos.xyz;
                WriteVec3( n, VFX_ATTR_POSITION, position );
            }
#endif
            // Death is the stack's (a module's Kill, UpdateAge at Lifetime); a stack without one never kills.
            u_PoolSlots[i] = ParticleSlot( id, sim.Kill ? 0u : 1u );
        }

        void main()
        {
            uint t = gl_GlobalInvocationID.x;
            if ( t >= u_Counts.x )
                return;

            uint base      = u_Counts.z;
            uint step      = u_Counts.y;
            uint slot      = step & 1u;
            uint alive     = u_Slots[slot].VertexCount / 6u;
            uint freeCount = u_Slots[slot].FreeCount;

            VFXChannelSpawn none;
            none.Position  = vec4( 0.0 );
            none.Direction = vec4( 0.0 );
            none.Color     = vec4( 1.0 );
            none.Scalars   = vec4( 0.0, 1.0, 0.0, 0.0 );

            if ( t < alive )
            {
                uint i = u_Alive[2u * base + slot * u_Counts.x + t];
                Run( i, false, u_PoolSlots[i].Id, step, none );
            }

            if ( t < min( u_Steps[step].Budget, freeCount ) )
            {
                uint i = u_Free[base + freeCount - 1u - t];
                // Appended after the alive entries: compact step+1 scans alive + spawned (Dispatch Args).
                u_Alive[2u * base + slot * u_Counts.x + alive + t] = i;
                VFXChannelSpawn channel = none;
                if ( t < u_Steps[step].ChannelCount )
                    channel = u_ChannelSpawns[u_Steps[step].ChannelFirst + t];
                Run( i, true, u_Steps[step].IdBase + t, step, channel );
            }
        }
    }
}
