// DesertAsset {"Kind":"Shader","Guid":"a50da2878ce74d4383171194eb17d44a","Versions":{"SHDR":1},"Dependencies":[]}
// Dispatch Args of one emitter's fixed step (VFX-07b, UE: indirect dispatch arguments from the GPU counts). One
// thread, after compact s and before Spawn+Update s: from compact s's alive / free counts (slot s & 1) and the
// step's spawn budget it writes the group counts of Spawn+Update s (max(alive, spawned) threads, args 0) and of
// compact s + 1 (alive + spawned threads - the particles the step touched, args 1), and opens compact s + 1's
// slot: no alive entry yet, the free list's untouched bottom (free - spawned) kept, Touched = alive + spawned.
// VFX-PARAMS: spawned = the step's CPU budget plus what the emitter's Event Handlers spawn from the events their
// sources raised the step before (Common/VFXEvents.glslh), capped by the free count; and the emitter opens this
// step's half of every event channel it raises into, so Spawn+Update appends to an empty half.
Shader "ParticleDispatchArgs"
{
    Compute
    {
        LocalSize(1, 1, 1);

        #include <Common/ParticlePool.glslh>

        struct VFXStep
        {
            uint IdBase;
            uint Seed;
            uint Budget;
            uint ChannelFirst; // ParticleSimulate's step layout (kParticleStepStride 20); Budget includes them
            uint ChannelCount;
        };

        ReadBuffer(0) StepTable
        {
            VFXStep u_Steps[];
        };

        Buffer(1) Counters
        {
            ParticleDrawSlot u_Slots[];
        };

        Buffer(2) DispatchArgs
        {
            uvec4 u_Args[]; // xyz = VkDispatchIndirectCommand, w unused; [0] Spawn+Update, [1] the next compact
        };

        // VFX-PARAMS: the emitter's event table and its system's event buffer (Common/VFXEvents.glslh).
        ReadBuffer(3) EventTable
        {
            uvec4 u_EventTable[];
        };

        Buffer(4) Events
        {
            uvec4 u_Events[];
        };

        PushConstant PushConstants
        {
            uvec4 u_Step; // x = the step, y = the system's event step serial of step 0
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

        void main()
        {
            uint step      = u_Step.x;
            uint slot      = step & 1u;
            uint alive     = u_Slots[slot].VertexCount / 6u;
            uint freeCount = u_Slots[slot].FreeCount;
            uint serial    = u_Step.y + step;
            uint spawned   = min( u_Steps[step].Budget + VFXEventSpawnTotal( serial ), freeCount );
            uint touched   = alive + spawned;

            u_Args[0] = uvec4( ( max( alive, spawned ) + PARTICLE_GROUP_SIZE - 1u ) / PARTICLE_GROUP_SIZE, 1u, 1u, 0u );
            u_Args[1] = uvec4( ( touched + PARTICLE_GROUP_SIZE - 1u ) / PARTICLE_GROUP_SIZE, 1u, 1u, 0u );

            // Open this step's half of each channel the emitter raises into (its readers take it next step).
            for ( uint kind = 0u; kind < 3u; ++kind )
                if ( u_EventTable[0][kind] != 0u )
                    u_Events[VFXEventHeaderIndex( VFXEventChannelOf( u_EventTable[1u + kind] ), serial )] =
                         uvec4( serial, 0u, 0u, 0u );

            uint next = 1u - slot;
            u_Slots[next] =
                 ParticleDrawSlot( 0u, 1u, u_Slots[next].FirstVertex, 0u, freeCount - spawned, touched, 0u, 0u );
        }
    }
}
