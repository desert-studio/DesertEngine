// DesertAsset {"Kind":"Shader","Guid":"a50da2878ce74d4383171194eb17d44a","Versions":{"SHDR":1},"Dependencies":[]}
// Dispatch Args of one emitter's fixed step (VFX-07b, UE: indirect dispatch arguments from the GPU counts). One
// thread, after compact s and before Spawn+Update s: from compact s's alive / free counts (slot s & 1) and the
// step's spawn budget it writes the group counts of Spawn+Update s (max(alive, spawned) threads, args 0) and of
// compact s + 1 (alive + spawned threads - the particles the step touched, args 1), and opens compact s + 1's
// slot: no alive entry yet, the free list's untouched bottom (free - spawned) kept, Touched = alive + spawned.
// OPEN MODE (Step.w = 1; "Particles <stage>: Open", before compact 0 of the emitter's chain): writes both draw slots
// empty with their FirstVertex (6 x the start of alive half h, 2 x base + h x count); compact 0 then fills slot 0.
// The Counters buffer is persistent and the host never writes it, so until this node the slots are the previous
// tick's - what a PostOpaque emitter's draws recorded before its simulation read (PORT-VFX-MODULES).
// EVENTS (PORT-VFX-MODULES, UE emitter Event Handlers): in step mode spawned = the step's CPU budget plus what the
// emitter's Event Handlers spawn from the events their sources raised the step before (Common/VFXEvents.glslh),
// capped by the free count; and the emitter opens this step's half of every event channel it raises into, so its
// Spawn+Update appends to an empty half - step 0 of a tick also zeroes the channel's tick count (VFXEvents.glslh).
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
            VFXStep Steps[];
        };

        Buffer(1) Counters
        {
            ParticleDrawSlot Slots[];
        };

        Buffer(2) DispatchArgs
        {
            uvec4 Arguments[]; // xyz = VkDispatchIndirectCommand, w unused; [0] Spawn+Update, [1] the next compact
        };

        // The emitter's event table and its system's event buffer (Common/VFXEvents.glslh).
        ReadBuffer(3) EventTable
        {
            uvec4 EventTableData[];
        };

        Buffer(4) Events
        {
            uvec4 EventsData[];
        };

        PushConstant PushConstants
        {
            uvec4 Step; // x = the step, y = the system's event step serial of step 0; open mode (w = 1): x = the
                        // range's pool base, y = its particle count
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

        void main()
        {
            if ( Step.w == 1u )
            {
                Slots[0] = ParticleDrawSlot( 0u, 1u, 2u * Step.x * 6u, 0u, 0u, 0u, 0u, 0u );
                Slots[1] = ParticleDrawSlot( 0u, 1u, ( 2u * Step.x + Step.y ) * 6u, 0u, 0u, 0u, 0u, 0u );
                return;
            }

            uint step      = Step.x;
            uint slot      = step & 1u;
            uint alive     = Slots[slot].VertexCount / 6u;
            uint freeCount = Slots[slot].FreeCount;
            uint serial    = Step.y + step;
            uint spawned   = min( Steps[step].Budget + VFXEventSpawnTotal( serial ), freeCount );
            uint touched   = alive + spawned;

            Arguments[0] = uvec4( ( max( alive, spawned ) + PARTICLE_GROUP_SIZE - 1u ) / PARTICLE_GROUP_SIZE, 1u, 1u, 0u );
            Arguments[1] = uvec4( ( touched + PARTICLE_GROUP_SIZE - 1u ) / PARTICLE_GROUP_SIZE, 1u, 1u, 0u );

            // Open this step's half of each channel the emitter raises into (its handlers take it next step): its
            // serial and count only, half 0's .z is the channel's tick count, zeroed by the tick's first step.
            for ( uint kind = 0u; kind < 3u; ++kind )
                if ( EventTableData[0][kind] != 0u )
                {
                    const VFXEventChannel c = VFXEventChannelOf( EventTableData[1u + kind] );
                    EventsData[VFXEventHeaderIndex( c, serial )].xy = uvec2( serial, 0u );
                    if ( step == 0u )
                        EventsData[VFXEventTickCountIndex( c )].z = 0u;
                }

            uint next = 1u - slot;
            Slots[next] =
                 ParticleDrawSlot( 0u, 1u, Slots[next].FirstVertex, 0u, freeCount - spawned, touched, 0u, 0u );
        }
    }
}
