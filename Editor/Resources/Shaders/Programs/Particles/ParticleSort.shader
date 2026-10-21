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
        //   5 RIBBON (u_Step.w = 1 only) one thread walks the emitter's link order and writes RibbonOrder at the
        //            same offsets: (pool index, length from the head in cm, t / (alive - 1), 1), then a (0, 0, 0, 0)
        //            terminator when alive < the range count (Mesh/Surface/Vertex_ParticleRibbon.glslh reads it).
        // u_Step.w picks the key: 0 = the view depth (a translucent sprite emitter, back to front), 1 = the ribbon
        // link order (UE: the ribbon's sort by RibbonLinkOrder, ParticleRibbonLinkOrder), ascending = head first.
        // Ascending keys = far first (Common/ParticleSortKey.glslh). The stages are separate graph nodes: the graph
        // places the barrier between two writes of the key buffer.
        LocalSize(512, 1, 1);

        #include <Common/ParticlePool.glslh>
        #include <Common/ParticleSortKey.glslh>
        #include <Common/ParticleDrawLayout.glslh>

        // The emitter's attribute columns and where its stack put Position (Common/ParticleDrawLayout.glslh): the
        // depth is the drawn position's, read from the one store the sprite draw reads it from.
        ReadBuffer(0) AttributeFloats
        {
            float u_DrawFloats[];
        };

        ReadBuffer(5) DrawLayout
        {
            ParticleDrawLayout u_DrawLayout;
        };

        #include <Common/ParticleDrawRead.glslh>

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

        // A ribbon emitter's linked order (stage 5), indexed as AliveList.
        Buffer(6) RibbonOrder
        {
            uvec4 u_Ribbon[];
        };

        // Float -> uint with the same order (negative values flipped whole, positive ones above them).
        uint OrderedFloatKey( float f )
        {
            const uint u = floatBitsToUint( f );
            return ( u & 0x80000000u ) != 0u ? ~u : ( u | 0x80000000u );
        }

        PushConstant PushConstants
        {
            uvec4 u_Range;       // x = the alive half's offset in AliveList, y = the draw slot, z = key base, w = N
            uvec4 u_Step;        // x = stage, y = k, z = j, w = key (0 view depth, 1 ribbon link order)
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
                    uint index = u_Alive[u_Range.x + t];
                    uint n     = ParticleLocalIndex( index );
                    if ( u_Step.w == 1u )
                    {
                        // Clamped below the pad key, so every live particle sorts before the padding.
                        key = uvec2( min( OrderedFloatKey( ParticleRibbonLinkOrder( n ) ), PARTICLE_SORT_PAD_KEY - 1u ),
                                     index );
                    }
                    else
                    {
                        float depth = dot( ParticleWorldPosition( n ) - u_ViewOrigin.xyz, u_ViewForward.xyz );
                        key         = uvec2( ParticleSortKey( depth ), index );
                    }
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

            if ( stage == 5u )
            {
                // Serial over the ribbon (one thread): the length from the head is a running sum.
                if ( t != 0u )
                    return;
                float length   = 0.0;
                vec3  previous = vec3( 0.0 );
                for ( uint s = 0u; s < alive; ++s )
                {
                    uint index    = u_Keys[keyBase + s].y;
                    vec3 position = ParticleWorldPosition( ParticleLocalIndex( index ) );
                    if ( s > 0u )
                        length += distance( previous, position );
                    previous = position;
                    float stretch = alive > 1u ? float( s ) / float( alive - 1u ) : 0.0;
                    u_Ribbon[u_Range.x + s] = uvec4( index, floatBitsToUint( length ), floatBitsToUint( stretch ), 1u );
                }
                if ( alive < u_DrawLayout.Range.x )
                    u_Ribbon[u_Range.x + alive] = uvec4( 0u );
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
