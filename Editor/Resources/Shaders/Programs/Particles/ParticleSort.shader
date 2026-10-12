// DesertAsset {"Kind":"Shader","Guid":"0b1f478c18804f7c87abc428fa5b43af","Versions":{"SHDR":1},"Dependencies":[]}
Shader "ParticleSort"
{
    Compute
    {
        // "Particles: Sort {view} {emitter} {stage}" (VFX-08): the back-to-front order of ONE translucent sprite
        // emitter for ONE view, on the GPU because the alive count lives on the GPU (the draw is indirect; a CPU
        // sort would need a readback, a frame late). A bitonic network over N = the power of two covering the
        // emitter's range (ParticleSortLength), staged by u_Step.x:
        //   0 KEYS   thread t < N: (ParticleSortKey(view depth), alive index) for t < alive, the pad key past it;
        //   1 LOCAL  each group sorts its block of 1024 keys (or all N) in shared memory, every k <= block;
        //   2 GLOBAL one compare-exchange (k = u_Step.y, j = u_Step.z) across blocks, j >= 1024;
        //   3 MERGE  each group finishes k = u_Step.y for every j < 1024 in shared memory;
        //   4 WRITE  thread t < alive: the sorted index into SortedAlive at the emitter's alive half, so the draw
        //            binds SortedAlive in place of AliveList and its slot (FirstVertex) is unchanged.
        // Ascending keys = far first (Common/ParticleSortKey.glslh). The stages are separate graph nodes: the graph
        // places the barrier between two writes of the key buffer.
        LocalSize(512, 1, 1);

        #include <Common/ParticleState.glslh>
        #include <Common/ParticlePool.glslh>
        #include <Common/ParticleSortKey.glslh>

        Buffer(0) Particles
        {
            Particle u_Particles[];
        };

        Buffer(1) AliveList
        {
            uint u_Alive[];
        };

        Buffer(2) Counters
        {
            ParticleDrawSlot u_Slots[];
        };

        Buffer(3) SortKeys
        {
            uvec2 u_Keys[];
        };

        Buffer(4) SortedAlive
        {
            uint u_Sorted[];
        };

        PushConstant PushConstants
        {
            uvec4 u_Range;       // x = the alive half's offset in AliveList, y = the draw slot, z = key base, w = N
            uvec4 u_Step;        // x = stage, y = k, z = j
            vec4  u_ViewOrigin;  // xyz = the view's position (cm)
            vec4  u_ViewForward; // xyz = the view's forward axis (unit)
        };

        shared uvec2 s_Keys[1024];

        bool KeyBefore( uvec2 a, uvec2 b )
        {
            return a.x < b.x || ( a.x == b.x && a.y < b.y );
        }

        // Orders the pair (i, l) of the shared block; global index gi decides the direction of its k-run.
        void ExchangeShared( uint i, uint l, uint gi, uint k )
        {
            uvec2 a         = s_Keys[i];
            uvec2 b         = s_Keys[l];
            bool  ascending = ( gi & k ) == 0u;
            if ( KeyBefore( b, a ) == ascending )
            {
                s_Keys[i] = b;
                s_Keys[l] = a;
            }
        }

        void main()
        {
            uint stage   = u_Step.x;
            uint n       = u_Range.w;
            uint keyBase = u_Range.z;
            uint alive   = u_Slots[u_Range.y].VertexCount / 6u;
            uint t       = gl_GlobalInvocationID.x;

            if ( stage == 0u )
            {
                if ( t >= n )
                    return;
                uvec2 key = uvec2( PARTICLE_SORT_PAD_KEY, 0u );
                if ( t < alive )
                {
                    uint  index = u_Alive[u_Range.x + t];
                    float depth = dot( u_Particles[index].PosSize.xyz - u_ViewOrigin.xyz, u_ViewForward.xyz );
                    key         = uvec2( ParticleSortKey( depth ), index );
                }
                u_Keys[keyBase + t] = key;
                return;
            }

            if ( stage == 2u )
            {
                uint k = u_Step.y;
                uint j = u_Step.z;
                if ( t >= n / 2u )
                    return;
                uint  i         = 2u * j * ( t / j ) + ( t % j );
                uint  l         = i + j;
                uvec2 a         = u_Keys[keyBase + i];
                uvec2 b         = u_Keys[keyBase + l];
                bool  ascending = ( i & k ) == 0u;
                if ( KeyBefore( b, a ) == ascending )
                {
                    u_Keys[keyBase + i] = b;
                    u_Keys[keyBase + l] = a;
                }
                return;
            }

            if ( stage == 4u )
            {
                if ( t < alive )
                    u_Sorted[u_Range.x + t] = u_Keys[keyBase + t].y;
                return;
            }

            // Stages 1 and 3: one block of min(N, 1024) keys per group, two keys per thread.
            uint block = min( n, 1024u );
            uint first = gl_WorkGroupID.x * block;
            uint p     = gl_LocalInvocationID.x;
            if ( 2u * p < block )
            {
                s_Keys[2u * p]      = u_Keys[keyBase + first + 2u * p];
                s_Keys[2u * p + 1u] = u_Keys[keyBase + first + 2u * p + 1u];
            }
            barrier();
            uint kFirst = stage == 1u ? 2u : u_Step.y;
            uint kLast  = stage == 1u ? block : u_Step.y;
            for ( uint k = kFirst; k <= kLast; k *= 2u )
            {
                for ( uint j = min( k, block ) / 2u; j > 0u; j /= 2u )
                {
                    if ( 2u * p < block )
                    {
                        uint i = 2u * j * ( p / j ) + ( p % j );
                        ExchangeShared( i, i + j, first + i, k );
                    }
                    barrier();
                }
            }
            if ( 2u * p < block )
            {
                u_Keys[keyBase + first + 2u * p]      = s_Keys[2u * p];
                u_Keys[keyBase + first + 2u * p + 1u] = s_Keys[2u * p + 1u];
            }
        }
    }
}
