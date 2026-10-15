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
        // id and alive flag (Common/ParticlePool.glslh ParticleSlot). A Spawn from Channel newborn carries
        // sim.Channel = 1 + its row of this tick's ChannelSpawns; the stack's engine:SpawnFromChannel module (the
        // compiler appends it to the spawn group) reads the row through VFX_ChannelSpawn - the host only binds the
        // rows. Positions are centimetres: a newborn's Position is relative to the emitter (the shape modules place
        // it around zero, VFX_ChannelSpawn hands a channel position relative to it); a World-space emitter's newborn
        // is moved to the emitter, a Local one is drawn at emitter + Position every step.
        // VFX-PARAMS: the pass runs POST-OPAQUE (SceneRenderer::AddFrameParticlesSimulate after the scene depth
        // resolve), so VFX_SceneDepthCollide reads THIS frame's scene depth (UE: the PostOpaque GPU tick, Scene
        // Depth collision). Events (Common/VFXEvents.glslh): after the stack, a particle raises the emitter's Death
        // (sim.Kill), Collision (sim.Collided) and Location (it lives on) events into its system's event buffer
        // where the emitter's event table names a channel; newborns past the step's CPU budget are the emitter's
        // Event Handlers' spawns, each carrying sim.Event = 1 + the event row engine:SpawnFromEvent reads.
        LocalSize(64, 1, 1);

        #include <Common/ParticlePool.glslh>

        // The pool's bookkeeping (Common/ParticlePool.glslh ParticleSlot): id + alive flag, none of the particle's data.
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

        // VFX-10: this tick's Spawn from Channel particles, one row per particle (VFXWorld PlanEmitterSteps); the
        // struct is Common/VFXParticleContract.glslh's.
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

        // VFX-PARAMS: the emitter's event table and its system's event buffer (Common/VFXEvents.glslh).
        ReadBuffer(10) EventTable
        {
            uvec4 u_EventTable[];
        };

        Buffer(11) Events
        {
            uvec4 u_Events[];
        };

        // VFX-PARAMS: the simulating view's camera, the one its scene depth was drawn with (ParticleRenderer
        // CollisionView): jittered view-projection and its inverse, camera position (cm), w = 1 when u_SceneDepth
        // is the frame's scene depth (0: the frame has none, nothing collides).
        ReadBuffer(12) CollisionView
        {
            mat4 u_ViewProjection;
            mat4 u_InvViewProjection;
            vec4 u_Camera;
        };

        // The single-sample scene depth of this frame (SceneRenderer::GetComputeSceneDepth), reversed Z. Read with
        // texelFetch, never filtered: a filtered depth across a silhouette is a distance where nothing is.
        Uniform(13) sampler2D u_SceneDepth;

        PushConstant PushConstants
        {
            vec4  u_EmitterPos; // xyz = emitter world pos (cm), w = the fixed step length (seconds)
            uvec4 u_Counts;     // x = the range's particle count, y = step, z = the range's pool base, w = local
            uvec4 u_Stack;      // x = the world's serial of this tick's step 0 (the random streams' step),
                                // y = the system's event step serial of step 0, zw = 0
        };

        #include <Common/VFXEvents.glslh>

        uvec4 VFXEventTableRow( uint row )
        {
            return u_EventTable[row];
        }
        uvec4 VFXEventBufferRow( uint index )
        {
            return u_Events[index];
        }

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
        // The row's bound position is world centimetres; the stack works relative to the emitter (see above).
        VFXChannelSpawn VFX_ChannelSpawn( uint row )
        {
            VFXChannelSpawn c = u_ChannelSpawns[row];
            if ( c.Position.w > 0.5 )
                c.Position.xyz -= u_EmitterPos.xyz;
            return c;
        }

        // The InheritVelocity of the handler whose event this invocation's newborn was spawned from (set by main
        // before the stack runs; one particle per invocation).
        float s_EventInheritVelocity = 0.0;

        // The event row a handler's newborn reads: the source's position made relative to the emitter (every
        // emitter of a system sits at the system's position), its velocity scaled by the handler.
        VFXEventSpawn VFX_EventSpawn( uint row )
        {
            VFXEventSpawn e;
            e.Position = vec4( uintBitsToFloat( u_Events[row].xyz ) - u_EmitterPos.xyz, 1.0 );
            e.Velocity = vec4( uintBitsToFloat( u_Events[row + 1u].xyz ) * s_EventInheritVelocity, 0.0 );
            e.Normal   = vec4( uintBitsToFloat( u_Events[row + 2u].xyz ), 0.0 );
            return e;
        }

        // The world point the scene depth holds at pixel @p px (reversed Z: 0 is the far plane / sky).
        vec3 SceneDepthPoint( ivec2 px, ivec2 size, out float deviceDepth )
        {
            px          = clamp( px, ivec2( 0 ), size - 1 );
            deviceDepth = texelFetch( u_SceneDepth, px, 0 ).r;
            const vec2 uv  = ( vec2( px ) + 0.5 ) / vec2( size );
            const vec2 ndc = vec2( uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0 );
            const vec4 h   = u_InvViewProjection * vec4( ndc, deviceDepth, 1.0 );
            return h.xyz / h.w;
        }

        // The surface's tangent along one screen axis at @p centre: the neighbour on the side whose point is
        // nearer the centre's, so a silhouette edge does not bend the normal toward the background.
        vec3 SceneDepthTangent( ivec2 px, ivec2 axis, ivec2 size, vec3 centre )
        {
            float      dPlus;
            float      dMinus;
            const vec3 plus        = SceneDepthPoint( px + axis, size, dPlus );
            const vec3 minus       = SceneDepthPoint( px - axis, size, dMinus );
            const bool plusUsable  = dPlus > 0.0;
            const bool minusUsable = dMinus > 0.0;
            if ( plusUsable && ( !minusUsable || distance( plus, centre ) <= distance( minus, centre ) ) )
                return plus - centre;
            return minusUsable ? centre - minus : vec3( 0.0 );
        }

        bool VFX_SceneDepthCollide( in VFXSim sim, vec3 position, float radius, float thickness, out vec3 contact,
                                    out vec3 normal )
        {
            contact = position;
            normal  = vec3( 0.0, 0.0, 1.0 );
            if ( u_Camera.w < 0.5 )
                return false;
            // The stack's frame (see the header): a Local particle and every newborn are relative to the emitter.
            const vec3 origin = ( u_Counts.w == 1u || sim.Spawned ) ? u_EmitterPos.xyz : vec3( 0.0 );
            const vec3 world  = position + origin;
            const vec4 clip   = u_ViewProjection * vec4( world, 1.0 );
            if ( clip.w <= 0.0 )
                return false; // behind the camera
            const vec2 ndc = clip.xy / clip.w;
            if ( abs( ndc.x ) > 1.0 || abs( ndc.y ) > 1.0 )
                return false; // off screen: the depth holds nothing there
            const ivec2 size = textureSize( u_SceneDepth, 0 );
            const vec2  uv   = vec2( ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5 );
            const ivec2 px   = clamp( ivec2( uv * vec2( size ) ), ivec2( 0 ), size - 1 );
            float       deviceDepth;
            const vec3  surface = SceneDepthPoint( px, size, deviceDepth );
            if ( deviceDepth <= 0.0 )
                return false; // the sky

            const vec3  toParticle       = world - u_Camera.xyz;
            const float particleDistance = length( toParticle );
            const float surfaceDistance  = distance( surface, u_Camera.xyz );
            const float behind           = particleDistance - surfaceDistance;
            if ( behind <= -radius || behind > thickness || particleDistance <= 1e-4 )
                return false;

            const vec3  dx  = SceneDepthTangent( px, ivec2( 1, 0 ), size, surface );
            const vec3  dy  = SceneDepthTangent( px, ivec2( 0, 1 ), size, surface );
            const vec3  n   = cross( dx, dy );
            const vec3  ray = toParticle / particleDistance;
            const float nl  = length( n );
            normal          = nl > 1e-6 ? n / nl : -ray;
            if ( dot( normal, ray ) > 0.0 )
                normal = -normal; // facing the camera
            // The surface point on the particle's own view ray, back in the stack's frame.
            contact = u_Camera.xyz + ray * surfaceDistance - origin;
            return true;
        }

        // Appends one event of @p kind to this step's half of the emitter's channel (Common/VFXEvents.glslh).
        void RaiseEvent( uint kind, vec3 world, vec3 velocity, vec3 normal )
        {
            const VFXEventChannel c      = VFXEventChannelOf( u_EventTable[1u + kind] );
            const uint            serial = u_Stack.y + u_Counts.y;
            const uint            e      = atomicAdd( u_Events[VFXEventHeaderIndex( c, serial )].y, 1u );
            if ( e >= c.Capacity )
                return; // a half holds one event per particle of the source's range: bounded, never reached
            const uint row     = VFXEventRowIndex( c, serial, e );
            u_Events[row]      = uvec4( floatBitsToUint( world ), 0u );
            u_Events[row + 1u] = uvec4( floatBitsToUint( velocity ), 0u );
            u_Events[row + 2u] = uvec4( floatBitsToUint( normal ), 0u );
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
        void Run( uint i, bool spawned, uint id, uint step, uint channel, uint event )
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
            sim.Channel    = channel;
            sim.Event      = event;
            sim.Collided   = false;
            sim.CollisionNormal = vec3( 0.0 );
            VFX_SimulateParticle( n, sim );

            vec3 position = vec3( 0.0 );
#ifdef VFX_ATTR_POSITION
            position = ReadVec3( n, VFX_ATTR_POSITION );
            if ( spawned )
            {
                // A newborn's Position is relative to the emitter: World space moves it there.
                if ( !local )
                {
                    position += u_EmitterPos.xyz;
                    WriteVec3( n, VFX_ATTR_POSITION, position );
                }
            }
#endif
            // The events the emitter raises (its event table, Common/VFXEvents.glslh), at the particle's world
            // position after the step.
            const uvec4 raises = u_EventTable[0];
            if ( raises.x != 0u || raises.y != 0u || raises.z != 0u )
            {
                const vec3 world    = local ? position + u_EmitterPos.xyz : position;
                vec3       velocity = vec3( 0.0 );
#ifdef VFX_ATTR_VELOCITY
                velocity = ReadVec3( n, VFX_ATTR_VELOCITY );
#endif
                if ( raises.x != 0u && sim.Kill )
                    RaiseEvent( VFX_EVENT_DEATH, world, velocity, vec3( 0.0 ) );
                if ( raises.y != 0u && sim.Collided )
                    RaiseEvent( VFX_EVENT_COLLISION, world, velocity, sim.CollisionNormal );
                if ( raises.z != 0u && !sim.Kill )
                    RaiseEvent( VFX_EVENT_LOCATION, world, velocity, vec3( 0.0 ) );
            }
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

            if ( t < alive )
            {
                uint i = u_Alive[2u * base + slot * u_Counts.x + t];
                Run( i, false, u_PoolSlots[i].Id, step, 0u, 0u );
            }

            // The step's newborns: the CPU budget (rate, bursts, channel) first, then the Event Handlers' spawns -
            // the same count Dispatch Args sized the dispatch by.
            const uint serial  = u_Stack.y + step;
            const uint budget  = u_Steps[step].Budget;
            const uint spawned = min( budget + VFXEventSpawnTotal( serial ), freeCount );
            if ( t < spawned )
            {
                uint i = u_Free[base + freeCount - 1u - t];
                // Appended after the alive entries: compact step+1 scans alive + spawned (Dispatch Args).
                u_Alive[2u * base + slot * u_Counts.x + alive + t] = i;
                // 1 + the row a Spawn from Channel newborn reads; 0 = not from the channel.
                const uint channel = t < u_Steps[step].ChannelCount ? u_Steps[step].ChannelFirst + t + 1u : 0u;
                uint       id      = u_Steps[step].IdBase + t;
                uint       event   = 0u;
                if ( t >= budget )
                {
                    // Spawn k of the handlers' spawns: handler h's events in order, SpawnCount particles each.
                    uint k = t - budget;
                    for ( uint h = 0u; h < VFXEventHandlerCount(); ++h )
                    {
                        const VFXEventChannel c     = VFXEventHandlerChannel( h );
                        const uint            each  = VFXEventHandlerSpawnCount( h );
                        const uint            count = VFXEventCount( c, serial - 1u ) * each;
                        if ( k < count )
                        {
                            event                  = 1u + VFXEventRowIndex( c, serial - 1u, k / each );
                            s_EventInheritVelocity = VFXEventHandlerInheritVelocity( h );
                            break;
                        }
                        k -= count;
                    }
                    // The CPU hands out the ids from IdBase up; an event newborn takes the top half of the id space,
                    // keyed by the step and its place in it (distinct for 32768 spawns a step over 65536 steps).
                    id = 0x80000000u | ( ( serial & 0xFFFFu ) << 15u ) | ( ( t - budget ) & 0x7FFFu );
                }
                Run( i, true, id, step, channel, event );
            }
        }
    }
}
