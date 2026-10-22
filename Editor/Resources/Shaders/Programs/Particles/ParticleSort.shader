// DesertAsset {"Kind":"Shader","Guid":"0b1f478c18804f7c87abc428fa5b43af","Versions":{"SHDR":1},"Dependencies":[]}
Shader "ParticleSort"
{
    Compute
    {
        // "Particles: Sort {view} {emitter} {stage}" (VFX-08): the back-to-front order of ONE translucent sprite
        // emitter for ONE view, on the GPU because the alive count lives on the GPU (the draw is indirect; a CPU
        // sort would need a readback, a frame late). A bitonic network over N = the power of two covering the
        // emitter's range (ParticleSortLength), staged by u_Step.x:
        //   0 KEYS   thread t < N: (0, ParticleSortKey(view depth), alive index) for t < alive, the pad key past it;
        //   1 LOCAL  each group sorts its block of 1024 keys (or all N) in shared memory, every k <= block;
        //   2 GLOBAL one compare-exchange (k = u_Step.y, j = u_Step.z) across blocks, j >= 1024;
        //   3 MERGE  each group finishes k = u_Step.y for every j < 1024 in shared memory;
        //   4 WRITE  thread t < alive: the sorted index into SortedAlive at the emitter's alive half, so the draw
        //            binds SortedAlive in place of AliveList and its slot (FirstVertex) is unchanged.
        // A ribbon emitter (u_Step.w = 1) replaces WRITE by a parallel segmented scan over the sorted keys (UE: the
        // GPU ribbon's per-RibbonId segments, NiagaraRibbonGpuInitComputeShader), one thread per entry each stage:
        //   5 RIBBON INIT  thread t < alive: (pool index, distance to the previous entry of the same ribbon, t + 1 at
        //                  a ribbon's head else 0, t at a ribbon's tail else ~0) into RibbonOrder;
        //   6 RIBBON SCAN  one Hillis-Steele step at distance d = u_Step.z: the length and the head summed / carried
        //                  from t - d within the ribbon, the tail carried back from t + d; ping-pong between
        //                  RibbonOrder and SortKeys (u_Step.y = 0 reads RibbonOrder, 1 reads SortKeys);
        //   7 RIBBON FINAL thread t < alive: RibbonOrder = (pool index, length from its ribbon's head in cm,
        //                  place / (ribbon count - 1), flags 1 | 2 head | 4 tail) (u_Step.y = the scan's last write)
        //                  - Mesh/Surface/Vertex_ParticleRibbon.glslh links e to e +- 1 inside one ribbon only.
        // u_Step.w picks the key: 0 = the view depth (a translucent sprite emitter, back to front), 1 = the ribbon
        // id, then the link order (UE: the ribbon's sort by (RibbonId, RibbonLinkOrder), ParticleRibbonId /
        // ParticleRibbonLinkOrder), ascending = head first: each ribbon is one contiguous run of the sorted keys.
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

        // The emitter's int columns: the Ribbon row's RibbonIdBinding (u_DrawLayout.Columns2.x).
        ReadBuffer(7) AttributeInts
        {
            int u_DrawInts[];
        };

        // UE Particles.RibbonID: which ribbon of the emitter the particle links into; unbound = 0, one ribbon.
        int ParticleRibbonId( uint n )
        {
            const uint c = u_DrawLayout.Columns2.x;
            return c == PARTICLE_ATTR_NONE ? 0 : u_DrawInts[c * u_DrawLayout.Range.x + n];
        }

        Buffer(1) AliveList
        {
            uint u_Alive[];
        };

        Buffer(2) Counters
        {
            ParticleDrawSlot u_Slots[];
        };

        // x = major key (the ribbon id, 0 for a depth sort), y = key, z = alive index; reused by the ribbon scan.
        Buffer(3) SortKeys
        {
            uvec4 u_Keys[];
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
            uvec4 u_Step;        // x = stage, y = k (ribbon: the scan's source), z = j (ribbon: d), w = key kind
            vec4  u_ViewOrigin;  // xyz = the view's position (cm)
            vec4  u_ViewForward; // xyz = the view's forward axis (unit)
        };

        shared uvec4 s_Keys[1024];

        bool KeyBefore( uvec4 a, uvec4 b )
        {
            return a.x < b.x || ( a.x == b.x && ( a.y < b.y || ( a.y == b.y && a.z < b.z ) ) );
        }

        // Int -> uint with the same order.
        uint OrderedIntKey( int i )
        {
            return uint( i ) ^ 0x80000000u;
        }

        #define RIBBON_NO_TAIL 0xFFFFFFFFu

        // One ribbon-scan entry: x = pool index, y = length bits, z = head + 1 (0 none yet), w = tail (~0 none yet).
        uvec4 RibbonScanRead( uint source, uint t )
        {
            return source == 0u ? u_Ribbon[u_Range.x + t] : u_Keys[u_Range.z + t];
        }

        void RibbonScanWrite( uint source, uint t, uvec4 v )
        {
            if ( source == 0u )
                u_Keys[u_Range.z + t] = v;
            else
                u_Ribbon[u_Range.x + t] = v;
        }

        // Orders the pair (i, l) of the shared block; global index gi decides the direction of its k-run.
        void ExchangeShared( uint i, uint l, uint gi, uint k )
        {
            uvec4 a         = s_Keys[i];
            uvec4 b         = s_Keys[l];
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
                uvec4 key = uvec4( PARTICLE_SORT_PAD_KEY, PARTICLE_SORT_PAD_KEY, 0u, 0u );
                if ( t < alive )
                {
                    uint index = u_Alive[u_Range.x + t];
                    uint n     = ParticleLocalIndex( index );
                    if ( u_Step.w == 1u )
                    {
                        // The ribbon id clamped below the pad key, so every live particle sorts before the padding.
                        key = uvec4( min( OrderedIntKey( ParticleRibbonId( n ) ), PARTICLE_SORT_PAD_KEY - 1u ),
                                     OrderedFloatKey( ParticleRibbonLinkOrder( n ) ), index, 0u );
                    }
                    else
                    {
                        float depth = dot( ParticleWorldPosition( n ) - u_ViewOrigin.xyz, u_ViewForward.xyz );
                        key         = uvec4( 0u, ParticleSortKey( depth ), index, 0u );
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
                uvec4 a         = u_Keys[keyBase + i];
                uvec4 b         = u_Keys[keyBase + l];
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
                if ( t >= alive )
                    return;
                const uvec4 key   = u_Keys[keyBase + t];
                const bool  head  = t == 0u || u_Keys[keyBase + t - 1u].x != key.x;
                const bool  tail  = t + 1u == alive || u_Keys[keyBase + t + 1u].x != key.x;
                float       step  = 0.0;
                if ( !head )
                    step = distance( ParticleWorldPosition( ParticleLocalIndex( u_Keys[keyBase + t - 1u].z ) ),
                                     ParticleWorldPosition( ParticleLocalIndex( key.z ) ) );
                u_Ribbon[u_Range.x + t] =
                     uvec4( key.z, floatBitsToUint( step ), head ? t + 1u : 0u, tail ? t : RIBBON_NO_TAIL );
                return;
            }

            if ( stage == 6u )
            {
                if ( t >= alive )
                    return;
                const uint source = u_Step.y;
                const uint d      = u_Step.z;
                uvec4      v      = RibbonScanRead( source, t );
                if ( v.z == 0u && t >= d )
                {
                    // No head yet within (t - d, t]: the ribbon goes on before, so its sum and head carry over.
                    const uvec4 before = RibbonScanRead( source, t - d );
                    v.y                = floatBitsToUint( uintBitsToFloat( v.y ) + uintBitsToFloat( before.y ) );
                    v.z                = before.z;
                }
                if ( t + d < alive )
                    v.w = min( v.w, RibbonScanRead( source, t + d ).w );
                RibbonScanWrite( source, t, v );
                return;
            }

            if ( stage == 7u )
            {
                if ( t >= alive )
                    return;
                const uvec4 v       = RibbonScanRead( u_Step.y, t );
                const uint  first   = v.z - 1u;
                const uint  last    = v.w;
                const float stretch = last > first ? float( t - first ) / float( last - first ) : 0.0;
                const uint  flags   = 1u | ( t == first ? 2u : 0u ) | ( t == last ? 4u : 0u );
                u_Ribbon[u_Range.x + t] = uvec4( v.x, v.y, floatBitsToUint( stretch ), flags );
                return;
            }

            if ( stage == 4u )
            {
                if ( t < alive )
                    u_Sorted[u_Range.x + t] = u_Keys[keyBase + t].z; // z = the alive index (x major, y key)
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
