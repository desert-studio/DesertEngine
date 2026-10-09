// DesertAsset {"Kind":"Shader","Guid":"5194037468f4cb1e73120f7432ee6cf2","Versions":{"SHDR":1},"Dependencies":[]}
Shader "ParticleSimulate"
{
    Compute
    {
        // "Particles: Spawn+Update {s}" (VFX-07): one fixed VFX step of ONE emitter over its range of the world's
        // particle pool. It reads the lists the compact before it built (ParticleCompact, compact s) and never
        // writes them, so the free and alive sets it walks are disjoint and fixed for the dispatch:
        //   * update: thread t < alive count integrates the particle at AliveList[Base + t] (gravity + velocity),
        //     ages it, and marks it dead (lifetime 0, alpha 0) when its age reaches its lifetime; the NEXT compact
        //     moves it to the free list;
        //   * spawn: thread t < min(budget, free count) initialises the particle at FreeList[Base + FreeCount - 1 -
        //     t] with id IdBase + t (VFXWorld's spawn plan, VFXRandom keyed by seed and id: deterministic).
        // Both bake the current size and colour into the particle, so the billboard needs no per-emitter uniforms.
        // One thread per particle of the range (Counts.x); positions in centimetres.
        LocalSize(64, 1, 1);

        #include <Common/ParticleState.glslh>
        #include <Common/ParticlePool.glslh>

        Buffer(0) Particles
        {
            Particle u_Particles[];
        };

        #include <Common/VFXRandom.glslh>

        struct VFXStep
        {
            uint IdBase; // id of the first particle this step may spawn
            uint Seed;   // the emitter instance's seed (system xor entity xor emitter)
            uint Budget; // how many particles this step spawns
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

        PushConstant PushConstants
        {
            vec4  u_EmitterPos; // xyz = emitter world pos, w = the fixed step length (seconds)
            vec4  u_Gravity;    // xyz = gravity, w = unused
            vec4  u_Direction;  // xyz = normalized emit dir, w = cone half-angle (radians)
            vec4  u_Params;     // x = startSpeed, y = speedVar, z = lifetime, w = lifetimeVar
            vec4  u_StartColor; // rgb + start alpha (w)
            vec4  u_EndColor;   // rgb + end alpha (w)
            vec4  u_Sizes;      // x = start size, y = end size, z = size-curve power, w = unused
            uvec4 u_Counts;     // x = the range's particle count, y = step, z = the range's pool base, w = local
        };

        Particle Shade( Particle p )
        {
            float t     = ( p.VelLife.w > 0.0 ) ? clamp( p.Age.x / p.VelLife.w, 0.0, 1.0 ) : 0.0;
            float st    = pow( t, u_Sizes.z > 0.0 ? u_Sizes.z : 1.0 ); // size-over-life ease curve
            p.PosSize.w = mix( u_Sizes.x, u_Sizes.y, st );
            p.Color     = mix( u_StartColor, u_EndColor, t );
            if ( p.VelLife.w <= 0.0 )
                p.Color.a = 0.0; // dead => invisible
            return p;
        }

        void main()
        {
            uint t = gl_GlobalInvocationID.x;
            if ( t >= u_Counts.x )
                return;

            uint  base      = u_Counts.z;
            uint  step      = u_Counts.y;
            uint  slot      = step & 1u;
            uint  alive     = u_Slots[slot].VertexCount / 6u;
            uint  freeCount = u_Slots[slot].FreeCount;
            float dt        = u_EmitterPos.w;

            if ( t < alive )
            {
                uint     i = u_Alive[2u * base + slot * u_Counts.x + t];
                Particle p = u_Particles[i];
                p.VelLife.xyz += u_Gravity.xyz * dt;
                if ( u_Counts.w == 1u )
                {
                    // Local space: Age.yzw is the offset from the emitter, which moves with it.
                    p.Age.yzw += p.VelLife.xyz * dt;
                    p.PosSize.xyz = u_EmitterPos.xyz + p.Age.yzw;
                }
                else
                {
                    p.PosSize.xyz += p.VelLife.xyz * dt;
                }
                p.Age.x += dt;
                if ( p.Age.x >= p.VelLife.w )
                    p.VelLife.w = 0.0; // died this step: the next compact frees it
                u_Particles[i] = Shade( p );
            }

            if ( t < min( u_Steps[step].Budget, freeCount ) )
            {
                uint i  = u_Free[base + freeCount - 1u - t];
                // Appended after the alive entries: compact step+1 scans alive + spawned (Dispatch Args).
                u_Alive[2u * base + slot * u_Counts.x + alive + t] = i;
                uint id = u_Steps[step].IdBase + t;
                vec4 r  = VFXRandomFloat4( uvec4( u_Steps[step].Seed, id, 0u, 0u ) );

                // Uniform direction inside the cone around u_Direction.
                float cosT  = mix( 1.0, cos( u_Direction.w ), r.x );
                float sinT  = sqrt( max( 0.0, 1.0 - cosT * cosT ) );
                float phi   = 6.2831853 * r.y;
                vec3  local = vec3( sinT * cos( phi ), sinT * sin( phi ), cosT );

                vec3 axis = normalize( u_Direction.xyz + vec3( 1e-5, 0.0, 0.0 ) );
                vec3 up   = abs( axis.y ) < 0.99 ? vec3( 0.0, 1.0, 0.0 ) : vec3( 1.0, 0.0, 0.0 );
                vec3 tang = normalize( cross( up, axis ) );
                vec3 bitn = cross( axis, tang );
                vec3 dir  = normalize( tang * local.x + bitn * local.y + axis * local.z );

                float speed = u_Params.x * ( 1.0 - u_Params.y * r.z );
                float life  = u_Params.z * ( 1.0 - u_Params.w * r.w );

                Particle p;
                p.PosSize      = vec4( u_EmitterPos.xyz, u_Sizes.x );
                p.VelLife      = vec4( dir * speed, max( life, 0.01 ) );
                p.Age          = vec4( 0.0 );
                p.Color        = vec4( 0.0 );
                u_Particles[i] = Shade( p );
            }
        }
    }
}
