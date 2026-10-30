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
        // PORT-VFX-MODULES: a stack that calls VFX_SceneDepthCollide ticks PostOpaque (VFX::VFXTickStage; SceneRenderer
        // AddFrameParticlesPostOpaque, after the opaque passes and the scene depth resolve), so the depth it reads is
        // THIS frame's, drawn with the matrices CollisionView carries (UE: the PostOpaqueRender GPU tick, Scene Depth
        // collision). Every other stack ticks PreOpaque and the compiler drops the two collision slots from it.
        LocalSize(64, 1, 1);

        #include <Common/ParticleState.glslh>
        #include <Common/ParticlePool.glslh>

        // The pool's bookkeeping (Common/ParticleState.glslh): id + alive flag, none of the particle's data.
        Buffer(0) PoolSlots
        {
            ParticleSlot PoolSlotsData[];
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
            VFXStep Steps[];
        };

        ReadBuffer(2) FreeList
        {
            uint Free[];
        };

        Buffer(3) AliveList
        {
            uint AliveData[];
        };

        ReadBuffer(4) Counters
        {
            ParticleDrawSlot Slots[];
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
            VFXChannelSpawn ChannelSpawnsData[];
        };

        // The emitter's attributes, SoA by component class (VFXDataSetLayout), one column per component.
        Buffer(6) AttributeFloats
        {
            float AttributeFloatsData[];
        };

        Buffer(7) AttributeInts
        {
            int AttributeIntsData[];
        };

        // One vec4 per parameter slot (BuildEmitterParams): module input values, never in the program text.
        ReadBuffer(8) StackParams
        {
            vec4 StackParamsData[];
        };

        // The system's curve atlas (BuildCurveAtlas).
        ReadBuffer(9) StackCurves
        {
            float StackCurvesData[];
        };

        // VFX-15: the placement's painted point sets this emitter spawns from, back to back, its own buffer (UE
        // ArrayPosition's buffer, no cap on the points): xyz = the point relative to the placement turned by its
        // transform (cm), w = the set's cumulative weight through this point, normalised to 1 at its last point.
        ReadBuffer(10) PointSets
        {
            vec4 PointSetsData[];
        };

        PushConstant PushConstants
        {
            // The entity's full transform (cm; UE Engine.Owner.SystemLocalToWorld): a World emitter's newborns are
            // placed through it; a Local emitter's particles live in its frame (the renderer draws them through it).
            mat4  LocalToWorld;
            vec4  StepLength; // x = the fixed step length (seconds), yzw = 0
            uvec4 Counts;     // x = the range's particle count, y = step, z = the range's pool base, w = local
            uvec4 Stack;      // x = the emitter's serial of this tick's step 0 (the random streams' step),
                              // y = the system's event step serial of step 0 (Common/VFXEvents.glslh), zw = 0
        };

        // PORT-VFX-MODULES (UE emitter Event Handlers): the emitter's event table and its system's event buffer
        // (Common/VFXEvents.glslh). Every stack binds them: the host raises the table's events and spawns its
        // handlers' newborns whatever the modules are.
        ReadBuffer(14) EventTable
        {
            uvec4 EventTableData[];
        };

        Buffer(15) Events
        {
            uvec4 EventsData[];
        };

        #include <Common/VFXEvents.glslh>

        uvec4 VFXEventTableRow( uint row )
        {
            return EventTableData[row];
        }
        uvec4 VFXEventBufferRow( uint index )
        {
            return EventsData[index];
        }

        // The InheritVelocity of the handler whose event this invocation's newborn was spawned from (set by main
        // before the stack runs; one newborn per invocation).
        float s_EventInheritVelocity = 0.0;

        #include <Generated/VFXEmitterStack.glslh>

        vec4 VFX_Param( uint slot )
        {
            return StackParamsData[slot];
        }
        float VFX_ReadFloat( uint particle, uint component )
        {
            return AttributeFloatsData[component * Counts.x + particle];
        }
        int VFX_ReadInt( uint particle, uint component )
        {
            return AttributeIntsData[component * Counts.x + particle];
        }
        void VFX_WriteFloat( uint particle, uint component, float value )
        {
            AttributeFloatsData[component * Counts.x + particle] = value;
        }
        void VFX_WriteInt( uint particle, uint component, int value )
        {
            AttributeIntsData[component * Counts.x + particle] = value;
        }
        float VFX_CurveLUT( uint index )
        {
            return StackCurvesData[index];
        }
        vec3 VFX_PointSet( vec4 row, float u )
        {
            const uint first = uint( row.x );
            const uint count = uint( row.y );
            if ( count == 0u )
                return vec3( 0.0 );
            // The first point whose cumulative weight passes u (the weighted pick, O(log n) per particle).
            uint lo = 0u;
            uint hi = count - 1u;
            while ( lo < hi )
            {
                const uint mid = ( lo + hi ) / 2u;
                if ( PointSetsData[first + mid].w <= u )
                    lo = mid + 1u;
                else
                    hi = mid;
            }
            return PointSetsData[first + lo].xyz;
        }

        // PORT-VFX-MODULES: the PostOpaque scene inputs and the scene-reading host functions exist only in a
        // stack that reads the scene (VFX::kVFXSceneDepthDefine, defined by the generated stack above), so a
        // PreOpaque program's layout never names them.
        #ifdef VFX_READS_SCENE_DEPTH
        // The simulating view's camera, the one its scene depth was drawn with (ParticleRenderer CollisionView):
        // jittered view-projection and its inverse, camera position (cm), w = 1 when SceneDepth is the frame's scene
        // depth (0: the frame has none, nothing collides).
        ReadBuffer(11) CollisionView
        {
            mat4 CollisionViewProjection;
            mat4 CollisionInvViewProjection;
            vec4 CollisionCamera;
        };

        // The single-sample scene depth of this frame (SceneRenderer::AddFrameSceneDepthResolve), reversed Z. Read
        // with texelFetch, never filtered: a filtered depth across a silhouette is a distance where nothing is.
        Uniform(12) texture2D SceneDepth;
        // Its point sampler (dev RDG binds a texture and a sampler, no combined image sampler); the same-name
        // macro keeps the sampled form (Fog/HeightFog.shader).
        Uniform(13) sampler CollisionPointSampler;
        #define SceneDepth sampler2D(SceneDepth, CollisionPointSampler)

        // The world point the scene depth holds at pixel @p px (reversed Z: 0 is the far plane / sky).
        vec3 SceneDepthPoint( ivec2 px, ivec2 size, out float deviceDepth )
        {
            px             = clamp( px, ivec2( 0 ), size - 1 );
            deviceDepth    = texelFetch( SceneDepth, px, 0 ).r;
            const vec2 uv  = ( vec2( px ) + 0.5 ) / vec2( size );
            const vec2 ndc = vec2( uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0 );
            const vec4 h   = CollisionInvViewProjection * vec4( ndc, deviceDepth, 1.0 );
            return h.xyz / h.w;
        }

        // The surface's tangent along one screen axis at @p centre: the neighbour on the side whose point is nearer
        // the centre's, so a silhouette edge does not bend the normal toward the background.
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
            normal  = vec3( 0.0, 1.0, 0.0 );
            if ( CollisionCamera.w < 0.5 )
                return false;
            // The stack's frame (VFX_SceneDepthCollide in the contract): a Local particle and every newborn are in
            // the placement frame, the host moves a World newborn to the world after the stack (Run).
            const bool  placed = Counts.w == 1u || sim.Spawned;
            const vec3  world  = placed ? ( LocalToWorld * vec4( position, 1.0 ) ).xyz : position;
            const vec4  clip   = CollisionViewProjection * vec4( world, 1.0 );
            if ( clip.w <= 0.0 )
                return false; // behind the camera
            const vec2 ndc = clip.xy / clip.w;
            if ( abs( ndc.x ) > 1.0 || abs( ndc.y ) > 1.0 )
                return false; // off screen: the depth holds nothing there
            const ivec2 size = textureSize( SceneDepth, 0 );
            const vec2  uv   = vec2( ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5 );
            const ivec2 px   = clamp( ivec2( uv * vec2( size ) ), ivec2( 0 ), size - 1 );
            float       deviceDepth;
            const vec3  surface = SceneDepthPoint( px, size, deviceDepth );
            if ( deviceDepth <= 0.0 )
                return false; // the sky

            const vec3  toParticle       = world - CollisionCamera.xyz;
            const float particleDistance = length( toParticle );
            const float surfaceDistance  = distance( surface, CollisionCamera.xyz );
            const float behind           = particleDistance - surfaceDistance;
            if ( behind <= -radius || behind > thickness || particleDistance <= 1e-4 )
                return false;

            const vec3  dx  = SceneDepthTangent( px, ivec2( 1, 0 ), size, surface );
            const vec3  dy  = SceneDepthTangent( px, ivec2( 0, 1 ), size, surface );
            const vec3  n   = cross( dx, dy );
            const vec3  ray = toParticle / particleDistance;
            const float nl  = length( n );
            vec3 worldNormal = nl > 1e-6 ? n / nl : -ray;
            if ( dot( worldNormal, ray ) > 0.0 )
                worldNormal = -worldNormal; // facing the camera
            // The surface point on the particle's own view ray, back in the stack's frame (a normal through the
            // inverse transpose, so a scaled placement keeps it perpendicular to the surface).
            const vec3 worldContact = CollisionCamera.xyz + ray * surfaceDistance;
            if ( placed )
            {
                const mat4 worldToLocal = inverse( LocalToWorld );
                contact                 = ( worldToLocal * vec4( worldContact, 1.0 ) ).xyz;
                normal = normalize( transpose( mat3( LocalToWorld ) ) * worldNormal );
            }
            else
            {
                contact = worldContact;
                normal  = worldNormal;
            }
            return true;
        }
        #endif // VFX_READS_SCENE_DEPTH

        // The event row a handler's newborn reads, in the newborn's frames (VFXEventSpawn): the stored world
        // position back into the placement frame, the world velocity and normal into a Local emitter's frame.
        VFXEventSpawn VFX_EventSpawn( uint row )
        {
            const bool local = Counts.w == 1u;
            const mat3 turn  = inverse( mat3( LocalToWorld ) );
            const vec3 v     = uintBitsToFloat( EventsData[row + 1u].xyz ) * s_EventInheritVelocity;
            const vec3 nrm   = uintBitsToFloat( EventsData[row + 2u].xyz );
            const vec3 world = uintBitsToFloat( EventsData[row].xyz );
            VFXEventSpawn e;
            e.Position = vec4( ( inverse( LocalToWorld ) * vec4( world, 1.0 ) ).xyz, 1.0 );
            e.Velocity = vec4( local ? turn * v : v, 0.0 );
            e.Normal   = vec4( local && dot( nrm, nrm ) > 0.0 ? normalize( turn * nrm ) : nrm, 0.0 );
            return e;
        }

        // Appends one event of @p kind (world position, velocity, normal) to this step's half of the emitter's
        // channel (Common/VFXEvents.glslh); its Dispatch Args opened the half.
        void RaiseEvent( uint kind, vec3 world, vec3 velocity, vec3 normal )
        {
            const VFXEventChannel c      = VFXEventChannelOf( EventTableData[1u + kind] );
            const uint            serial = Stack.y + Counts.y;
            const uint            e      = atomicAdd( EventsData[VFXEventHeaderIndex( c, serial )].y, 1u );
            if ( e >= c.Capacity )
                return; // a half holds one event per particle of the source's range a step: never reached
            const uint row       = VFXEventRowIndex( c, serial, e );
            EventsData[row]      = uvec4( floatBitsToUint( world ), 0u );
            EventsData[row + 1u] = uvec4( floatBitsToUint( velocity ), 0u );
            EventsData[row + 2u] = uvec4( floatBitsToUint( normal ), 0u );
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
        void Run( uint i, bool spawned, uint id, uint step, VFXChannelSpawn channel, uint event )
        {
            const uint n     = i - Counts.z; // range-local: the attribute columns are the emitter's
            const bool local = Counts.w == 1u;

            VFXSim sim;
            sim.DeltaTime  = StepLength.x;
            sim.EmitterAge = 0.0;
            sim.Seed       = Steps[step].Seed;
            sim.ParticleId = id;
            sim.Step       = Stack.x + step;
            sim.Spawned    = spawned;
            sim.Kill       = false;
            sim.ChannelDirection = channel.Direction;
            sim.ChannelColor     = channel.Color;
            sim.ChannelScalars   = channel.Scalars;
            sim.Collided         = false;
            sim.CollisionNormal  = vec3( 0.0 );
            sim.Event            = event;
            VFX_SimulateParticle( n, sim );

            vec3 position = vec3( 0.0 );
#ifdef VFX_ATTR_POSITION
            position = ReadVec3( n, VFX_ATTR_POSITION );
            if ( spawned )
            {
                // A newborn's Position is in the placement frame (the shape modules place it around zero): a World
                // emitter's goes to the world through the entity's full transform, a Local emitter's stays in the
                // frame. A Spawn-from-Channel position is a world one: a Local emitter takes it into its frame.
                if ( channel.Position.w > 0.5 )
                    position = local ? ( inverse( LocalToWorld ) * vec4( channel.Position.xyz, 1.0 ) ).xyz
                                     : channel.Position.xyz;
                else if ( !local )
                    position = ( LocalToWorld * vec4( position, 1.0 ) ).xyz;
                WriteVec3( n, VFX_ATTR_POSITION, position );
            }
#endif
            // The events the emitter raises (its event table), at the particle's world position after the step:
            // Death from the stack's Kill, Collision from the Collision module's impact, Location every step lived.
            const uvec4 raises = EventTableData[0];
            if ( raises.x != 0u || raises.y != 0u || raises.z != 0u )
            {
                // The stack's frames: a Local particle lives in the placement frame; a World newborn's Position was
                // just taken to the world, its velocity and a collision normal are world.
                const mat3 turn     = mat3( LocalToWorld );
                const vec3 world    = local ? ( LocalToWorld * vec4( position, 1.0 ) ).xyz : position;
                vec3       velocity = vec3( 0.0 );
#ifdef VFX_ATTR_VELOCITY
                velocity = ReadVec3( n, VFX_ATTR_VELOCITY );
#endif
                velocity = local ? turn * velocity : velocity;
                const vec3 normal =
                     local && sim.Collided ? normalize( turn * sim.CollisionNormal ) : sim.CollisionNormal;
                if ( raises.x != 0u && sim.Kill )
                    RaiseEvent( VFX_EVENT_DEATH, world, velocity, vec3( 0.0 ) );
                if ( raises.y != 0u && sim.Collided )
                    RaiseEvent( VFX_EVENT_COLLISION, world, velocity, normal );
                if ( raises.z != 0u && !sim.Kill )
                    RaiseEvent( VFX_EVENT_LOCATION, world, velocity, vec3( 0.0 ) );
            }
            // Death is the stack's (a module's Kill, UpdateAge at Lifetime); a stack without one never kills.
            PoolSlotsData[i] = ParticleSlot( id, sim.Kill ? 0u : 1u );
        }

        void main()
        {
            uint t = gl_GlobalInvocationID.x;
            if ( t >= Counts.x )
                return;

            uint base      = Counts.z;
            uint step      = Counts.y;
            uint slot      = step & 1u;
            uint alive     = Slots[slot].VertexCount / 6u;
            uint freeCount = Slots[slot].FreeCount;

            VFXChannelSpawn none;
            none.Position  = vec4( 0.0 );
            none.Direction = vec4( 0.0 );
            none.Color     = vec4( 1.0 );
            none.Scalars   = vec4( 0.0, 1.0, 0.0, 0.0 );

            if ( t < alive )
            {
                uint i = AliveData[2u * base + slot * Counts.x + t];
                Run( i, false, PoolSlotsData[i].Id, step, none, 0u );
            }

            // The step's newborns: the CPU budget (rate, bursts, channel) first, then the Event Handlers' spawns -
            // the same count Dispatch Args sized the dispatch by (both read the same, unchanging, halves).
            const uint serial = Stack.y + step;
            const uint budget = Steps[step].Budget;
            if ( t < min( budget + VFXEventSpawnTotal( serial ), freeCount ) )
            {
                uint i = Free[base + freeCount - 1u - t];
                // Appended after the alive entries: compact step+1 scans alive + spawned (Dispatch Args).
                AliveData[2u * base + slot * Counts.x + alive + t] = i;
                VFXChannelSpawn channel = none;
                if ( t < Steps[step].ChannelCount )
                    channel = ChannelSpawnsData[Steps[step].ChannelFirst + t];
                uint id    = Steps[step].IdBase + t;
                uint event = 0u;
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
                    // The CPU hands out the ids from IdBase up; an event newborn takes the top half of the id
                    // space, keyed by the step and its place in it (distinct for 32768 spawns a step over 65536
                    // steps).
                    id = 0x80000000u | ( ( serial & 0xFFFFu ) << 15u ) | ( ( t - budget ) & 0x7FFFu );
                }
                Run( i, true, id, step, channel, event );
            }
        }
    }
}
