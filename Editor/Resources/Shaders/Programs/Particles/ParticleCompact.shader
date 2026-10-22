// DesertAsset {"Kind":"Shader","Guid":"ca0832fc3a1848b5957efe2e308fb5c4","Versions":{"SHDR":1},"Dependencies":[]}
Shader "ParticleCompact"
{
    Compute
    {
        // "Particles: Compact {c}" (VFX-07): rebuilds ONE emitter's free and alive lists from its range of the
        // world's particle pool. Thread t classifies the slot at Base + t: alive (its Alive flag) -> its index is
        // appended to the alive list and VertexCount grows by six (the slot IS the billboard's indirect draw);
        // otherwise to the free list. Every index of the range lands in exactly one list. Compact c fills slot
        // (c & 1), which the compact before it (or the CPU upload, for compact 0) left at zero, and thread 0
        // resets slot ((c + 1) & 1) for compact c + 1 - that slot was last read by the Spawn+Update before this
        // compact. Reset (SortRange.w = 1, compact 0 of a fresh or restarted emitter) first kills the whole range:
        // the pool is never zeroed on the CPU.
        LocalSize(64, 1, 1);

        #include <Common/ParticleState.glslh>
        #include <Common/ParticlePool.glslh>

        // The pool's bookkeeping (Common/ParticleState.glslh): id + alive flag, none of the particle's data.
        Buffer(0) PoolSlots
        {
            ParticleSlot PoolSlotsData[];
        };

        Buffer(1) FreeList
        {
            uint Free[];
        };

        Buffer(2) AliveList
        {
            uint AliveData[];
        };

        Buffer(3) Counters
        {
            ParticleDrawSlot Slots[];
        };

        PushConstant PushConstants
        {
            uvec4 SortRange; // x = the range's pool base, y = its particle count, z = the slot filled, w = 1 reset | 2 full scan
        };

        void main()
        {
            uint t     = gl_GlobalInvocationID.x;
            uint base  = SortRange.x;
            uint count = SortRange.y;
            uint slot  = SortRange.z;
            uint i;
            if ( ( SortRange.w & 2u ) != 0u )
            {
                // Compact 0: the whole range (slot 0 uploaded empty).
                if ( t >= count )
                    return;
                i = base + t;
                if ( ( SortRange.w & 1u ) != 0u )
                    PoolSlotsData[i] = ParticleSlot( 0u, 0u );
            }
            else
            {
                // A later compact: the particles the step touched (the other half: alive + spawned), its slot
                // opened by Dispatch Args with the untouched free entries kept.
                if ( t >= Slots[slot].Touched )
                    return;
                i = AliveData[2u * base + ( 1u - slot ) * count + t];
            }

            if ( PoolSlotsData[i].Alive != 0u )
                AliveData[2u * base + slot * count + atomicAdd( Slots[slot].VertexCount, 6u ) / 6u] = i;
            else
                Free[base + atomicAdd( Slots[slot].FreeCount, 1u )] = i;
        }
    }
}
