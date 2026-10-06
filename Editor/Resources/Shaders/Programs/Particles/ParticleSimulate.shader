// DesertAsset {"Kind":"Shader","Guid":"5194037468f4cb1e73120f7432ee6cf2","Versions":{"SHDR":1},"Dependencies":[]}
Shader "ParticleSimulate"
{
    Compute
    {
        // GPU particle simulation. One thread per particle: integrate alive particles (gravity + velocity),
        // age them, respawn dead ones from the emitter within a per-frame spawn budget (atomic counter), and
        // bake the CURRENT interpolated size + colour into the particle so the billboard shader needs no
        // per-emitter uniforms. State lives in a PERSISTENT storage buffer (survives across frames). Dispatched
        // via DispatchComputeCull so the writes are visible to the billboard VERTEX shader that reads the same
        // buffer.

        // 64 is Graphic::System::kParticleLocalSize, which is what the dispatch divides the particle
        // count by. Tests/Engine/ShaderCacheKey reads this number back out of the compiled module and
        // asserts it against that constant; a workgroup edited here alone would leave the tail of every
        // emitter unsimulated, with nothing to say so.
        LocalSize(64, 1, 1);

        // The element layout, shared with ParticleBillboard.shader rather than restated here.
        #include <Common/ParticleState.glslh>

        Buffer(0) Particles
        {
            Particle u_Particles[];
        };

        #include <Common/VFXRandom.glslh>

        // One fixed step of the emitter, written by the CPU from the scene's VFXWorld once per frame
        // (Graphic::System::kParticleStepStride bytes each). The frame runs u_Counts.y = 0..N-1 in order.
        struct VFXStep
        {
            uint SpawnCount; // atomically consumed this step (CPU-zeroed)
            uint IdBase;     // id of the first particle this step may spawn
            uint Seed;       // the emitter instance's seed (system ⊕ entity ⊕ emitter)
            uint Budget;     // how many particles this step spawns
        };

        Buffer(1) StepTable
        {
            VFXStep u_Steps[];
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
            uvec4 u_Counts;     // x = maxParticles, y = this dispatch's step (index into u_Steps), z = unused,
                                // w = local-space(0/1): particles ride the emitter instead of trailing it
        };

        void main()
        {
            uint i = gl_GlobalInvocationID.x;
            if ( i >= u_Counts.x )
                return;

            Particle p  = u_Particles[i];
            float    dt = u_EmitterPos.w;

            // Integrate alive particles. In LOCAL mode (u_Counts.w) the integrated state is the offset
            // FROM the emitter (kept in the spare Age.yzw lanes) and the world position is rebuilt from
            // the CURRENT emitter position every frame — that is the whole difference between "the smoke
            // trails behind the torch" and "the flame rides it".
            if ( p.VelLife.w > 0.0 )
            {
                p.VelLife.xyz += u_Gravity.xyz * dt;
                if ( u_Counts.w == 1u )
                {
                    p.Age.yzw += p.VelLife.xyz * dt;
                    p.PosSize.xyz = u_EmitterPos.xyz + p.Age.yzw;
                }
                else
                {
                    p.PosSize.xyz += p.VelLife.xyz * dt;
                }
                p.Age.x += dt;
                if ( p.Age.x >= p.VelLife.w )
                    p.VelLife.w = 0.0; // died this frame
            }

            // Respawn dead particles from the emitter, within this frame's spawn budget.
            if ( p.VelLife.w <= 0.0 )
            {
                uint step = u_Counts.y;
                uint slot = atomicAdd( u_Steps[step].SpawnCount, 1u );
                if ( slot < u_Steps[step].Budget )
                {
                    // Which buffer slot takes the particle depends on thread order; WHAT is spawned does
                    // not: the particle's id is the step's id base plus its spawn ordinal, and every random
                    // number below is a hash of (seed, id, call) — never of time.
                    uint  id = u_Steps[step].IdBase + slot;
                    vec4  r  = VFXRandomFloat4( uvec4( u_Steps[step].Seed, id, 0u, 0u ) );
                    float u1 = r.x;
                    float u2 = r.y;
                    float u3 = r.z;

                    float cosT  = mix( 1.0, cos( u_Direction.w ), u1 );
                    float sinT  = sqrt( max( 0.0, 1.0 - cosT * cosT ) );
                    float phi   = 6.2831853 * u2;
                    vec3  local = vec3( sinT * cos( phi ), sinT * sin( phi ), cosT );

                    vec3 axis = normalize( u_Direction.xyz + vec3( 1e-5, 0.0, 0.0 ) );
                    vec3 up   = abs( axis.y ) < 0.99 ? vec3( 0.0, 1.0, 0.0 ) : vec3( 1.0, 0.0, 0.0 );
                    vec3 tang = normalize( cross( up, axis ) );
                    vec3 bitn = cross( axis, tang );
                    vec3 dir  = normalize( tang * local.x + bitn * local.y + axis * local.z );

                    float speed = u_Params.x * ( 1.0 - u_Params.y * u3 );
                    float life  = u_Params.z * ( 1.0 - u_Params.w * r.w );

                    p.PosSize = vec4( u_EmitterPos.xyz, u_Sizes.x );
                    p.VelLife = vec4( dir * speed, max( life, 0.01 ) );
                    p.Age     = vec4( 0.0 );
                }
                else
                {
                    p.VelLife.w = 0.0; // stay dead
                }
            }

            // Bake the current size + colour from the particle's normalized age (0..1 over its life).
            float t     = ( p.VelLife.w > 0.0 ) ? clamp( p.Age.x / p.VelLife.w, 0.0, 1.0 ) : 0.0;
            float st    = pow( t, u_Sizes.z > 0.0 ? u_Sizes.z : 1.0 ); // size-over-life ease curve
            p.PosSize.w = mix( u_Sizes.x, u_Sizes.y, st );
            p.Color     = mix( u_StartColor, u_EndColor, t );
            if ( p.VelLife.w <= 0.0 )
                p.Color.a = 0.0; // dead => invisible

            u_Particles[i] = p;
        }
    }
}
