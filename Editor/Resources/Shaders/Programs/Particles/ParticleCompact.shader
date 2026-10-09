// DesertAsset {"Kind":"Shader","Guid":"ca0832fc3a1848b5957efe2e308fb5c4","Versions":{"SHDR":1},"Dependencies":[]}
Shader "ParticleCompact"
{
    Compute
    {
        // "Particles: Compact {c}" (VFX-07): rebuilds ONE emitter's free and alive lists from its range of the
        // world's particle pool. Thread t classifies the particle at Base + t: alive (lifetime > 0) -> its index is
        // appended to the alive list and VertexCount grows by six (the slot IS the billboard's indirect draw);
        // otherwise to the free list. Every index of the range lands in exactly one list. Compact c fills slot
        // (c & 1), which the compact before it (or the CPU upload, for compact 0) left at zero, and thread 0
        // resets slot ((c + 1) & 1) for compact c + 1 - that slot was last read by the Spawn+Update before this
        // compact. Reset (u_Range.w = 1, compact 0 of a fresh or restarted emitter) first kills the whole range:
        // the pool is never zeroed on the CPU.
        LocalSize(64, 1, 1);

        #include <Common/ParticlePool.glslh>

        // Liveness of every pool slot, whichever program simulates its range (ParticleSimulate or a compiled
        // stack): the compact does not read the particles' attributes, so it serves every layout.
        Buffer(0) Slots
        {
            VFXSlotState u_SlotState[];
        };

        Buffer(1) FreeList
        {
            uint u_Free[];
        };

        Buffer(2) AliveList
        {
            uint u_Alive[];
        };

        Buffer(3) Counters
        {
            ParticleDrawSlot u_Slots[];
        };

        PushConstant PushConstants
        {
            uvec4 u_Range; // x = the range's pool base, y = its particle count, z = the slot filled, w = 1 reset | 2 full scan
        };

        void main()
        {
            uint t     = gl_GlobalInvocationID.x;
            uint base  = u_Range.x;
            uint count = u_Range.y;
            uint slot  = u_Range.z;
            uint i;
            if ( ( u_Range.w & 2u ) != 0u )
            {
                // Compact 0: the whole range (slot 0 uploaded empty).
                if ( t >= count )
                    return;
                i = base + t;
                if ( ( u_Range.w & 1u ) != 0u )
                    u_SlotState[i] = VFXSlotState( 0u, 0u );
            }
            else
            {
                // A later compact: the particles the step touched (the other half: alive + spawned), its slot
                // opened by Dispatch Args with the untouched free entries kept.
                if ( t >= u_Slots[slot].Touched )
                    return;
                i = u_Alive[2u * base + ( 1u - slot ) * count + t];
            }

            if ( u_SlotState[i].Alive != 0u )
                u_Alive[2u * base + slot * count + atomicAdd( u_Slots[slot].VertexCount, 6u ) / 6u] = i;
            else
                u_Free[base + atomicAdd( u_Slots[slot].FreeCount, 1u )] = i;
        }
    }
}
