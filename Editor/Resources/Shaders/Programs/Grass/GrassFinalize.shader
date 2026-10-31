// DesertAsset {"Kind":"Shader","Guid":"9af5b17398544149b3546d72bc857b7b","Versions":{"SHDR":1},"Dependencies":[]}
// After GrassGenerate: each bucket's appended count, clamped to its capacity, becomes the instanceCount of the
// records that draw it and its first instance their firstInstance (GpuInstances.hpp: bucket L for the submeshes'
// records of a mesh's level L - one bucket without a LOD chain - and bucket s for record s of every set of a blade
// strip's records, one set per shadow LOD, Mode.z), the UNCLAMPED counts (the generator's atomic counts past the
// capacity) go to Feedback for the renderer to size the next capacities from (GrassRenderer, GPU-count feedback), and
// the counters are reset for the next frame's generation. One thread.
// The camera's list (GRASS-VIS, Mode.y): CameraArgs' records get its phase-1 counts the same way, and each bucket's
// phase words (GrassGenerate.shader CameraArgs) freeze this frame's candidates and phase 1's drawn count for phase 2
// and restart the counters. The post mode (Mode.x = 1, after phase 2) writes PostArgs' records: phase 2's appended
// instances, right after phase 1's drawn ones in the camera's list.
// Drawn counts (GRASS-VIS-7, GrassRenderer::GetDrawnCounts): after the written mark, Feedback holds the full list's
// drawn count, the camera's phase-1 drawn count (the full list's when the camera has no list of its own) and phase
// 2's appended count (0 until the post mode writes it), read back with the bucket counts FramesInFlight frames later.
// The cascades' lists (GPU-COST-21, Mode.w = the cascade views that have one, list k the k-th set bit): each list's
// copy of Args' layout in CascadeArgs gets its records and counts the same way, and Feedback, after those three
// counts, one drawn count per cascade VIEW (0 for a view without a list), so the read-back names the cascade
// whatever lists a later frame has.
Shader "GrassFinalize"
{
    Compute
    {
        LocalSize(1, 1, 1);

        Buffer(0) Args
        {
            uint Arguments[]; // bucket count, capacity, record count, drawn, 5 words per record, 4 per bucket
        };

        Buffer(1) Feedback
        {
            uint FeedbackData[]; // per bucket the count the generator wanted, then 1 (this copy was written), then
                                 // the drawn counts: full list, camera phase 1, phase 2
        };

        Buffer(2) CameraArgs
        {
            uint CameraArguments[]; // Args' layout, then four phase words per bucket (GrassGenerate.shader)
        };

        Buffer(3) PostArgs
        {
            uint PostArguments[]; // Args' layout: the phase-2 draws
        };

        Buffer(4) CascadeArgs
        {
            uint CascadeArguments[]; // per cascade list one copy of Args' layout (GrassGenerate.shader CascadeArgs)
        };

        // The cascade views Feedback counts (GrassRenderer kGrassCascadeLists, SceneResources::kMaxCascadeViews).
        const uint kCascadeViews = 8u;

        // Mirrors Graphic::System::GrassFinalizePush (GrassRenderer.cpp).
        PushConstant PushConstants
        {
            uvec4 Mode; // x = 0 after the generation, 1 after phase 2; y = 1 when the camera's list was generated;
                        // z = 1: a blade's records, sets of one record per bucket (record s draws bucket s % count);
                        // w = the cascade views with a list of their own (bit v: view v; GPU-COST-21)
        };

        // The bucket record @p s of @p records draws: a mesh's records come in equal groups, one per bucket (its
        // levels); a blade's in sets of one record per bucket (Mode.z), each set a shadow LOD of the same buckets.
        uint BucketOfRecord( uint s, uint records, uint buckets )
        {
            if ( Mode.z != 0u )
                return s % buckets;
            return min( s / max( 1u, records / buckets ), buckets - 1u );
        }

        // Phase 1's camera records, and the phase words frozen for phase 2.
        void FinalizeCamera()
        {
            uint buckets = CameraArguments[0];
            uint records = CameraArguments[2];
            uint table   = 4u + records * 5u;
            uint phases  = table + buckets * 4u;
            for ( uint s = 0u; s < records; ++s )
            {
                uint b  = BucketOfRecord( s, records, buckets );
                uint at = table + b * 4u;
                CameraArguments[4u + s * 5u + 1u] = min( CameraArguments[at], CameraArguments[at + 2u] );
                CameraArguments[4u + s * 5u + 4u] = CameraArguments[at + 1u];
            }
            uint drawn = 0u;
            for ( uint b = 0u; b < buckets; ++b )
            {
                uint at    = table + b * 4u;
                uint phase = phases + b * 4u;
                uint seen  = min( CameraArguments[at], CameraArguments[at + 2u] );
                drawn += seen;
                CameraArguments[phase + 1u] = min( CameraArguments[phase], CameraArguments[at + 2u] );
                CameraArguments[phase]      = 0u;
                CameraArguments[phase + 2u] = seen;
                CameraArguments[phase + 3u] = 0u;
                CameraArguments[at]         = 0u;
            }
            CameraArguments[3] = drawn;
        }

        // Phase 2's records: each bucket's appended count, within what phase 1 left of its capacity, after phase
        // 1's drawn instances.
        uint PostCount( uint at, uint phase )
        {
            uint room = CameraArguments[at + 2u] - min( CameraArguments[phase + 2u], CameraArguments[at + 2u] );
            return min( CameraArguments[phase + 3u], room );
        }
        void FinalizePost()
        {
            uint buckets = CameraArguments[0];
            uint records = CameraArguments[2];
            uint table   = 4u + records * 5u;
            uint phases  = table + buckets * 4u;
            for ( uint s = 0u; s < records; ++s )
            {
                uint b = BucketOfRecord( s, records, buckets );
                PostArguments[4u + s * 5u + 1u] = PostCount( table + b * 4u, phases + b * 4u );
                PostArguments[4u + s * 5u + 4u] = CameraArguments[table + b * 4u + 1u] + CameraArguments[phases + b * 4u + 2u];
            }
            uint drawn = 0u;
            for ( uint b = 0u; b < buckets; ++b )
                drawn += PostCount( table + b * 4u, phases + b * 4u );
            PostArguments[3]            = drawn;
            FeedbackData[buckets + 3u] = drawn;
        }

        // Each cascade list's records and counts as Args', its drawn count into Feedback by the view it is for.
        void FinalizeCascades( uint buckets, uint records )
        {
            for ( uint v = 0u; v < kCascadeViews; ++v )
                FeedbackData[buckets + 4u + v] = 0u;
            uint words = 4u + records * 5u + buckets * 4u;
            uint mask  = Mode.w;
            for ( uint k = 0u; mask != 0u; ++k )
            {
                uint view = uint( findLSB( mask ) );
                mask &= mask - 1u;
                uint base  = k * words;
                uint table = base + 4u + records * 5u;
                for ( uint s = 0u; s < records; ++s )
                {
                    uint at = table + BucketOfRecord( s, records, buckets ) * 4u;
                    CascadeArguments[base + 4u + s * 5u + 1u] = min( CascadeArguments[at], CascadeArguments[at + 2u] );
                    CascadeArguments[base + 4u + s * 5u + 4u] = CascadeArguments[at + 1u];
                }
                uint drawn = 0u;
                for ( uint b = 0u; b < buckets; ++b )
                {
                    uint at = table + b * 4u;
                    drawn += min( CascadeArguments[at], CascadeArguments[at + 2u] );
                    CascadeArguments[at] = 0u;
                }
                CascadeArguments[base + 3u]          = drawn;
                FeedbackData[buckets + 4u + view] = drawn;
            }
        }

        void main()
        {
            if ( Mode.x == 1u )
            {
                FinalizePost();
                return;
            }
            if ( Mode.y != 0u )
                FinalizeCamera();
            uint buckets = Arguments[0];
            uint records = Arguments[2];
            uint table   = 4u + records * 5u;
            for ( uint s = 0u; s < records; ++s )
            {
                uint b = BucketOfRecord( s, records, buckets );
                uint at = table + b * 4u;
                Arguments[4u + s * 5u + 1u] = min( Arguments[at], Arguments[at + 2u] );
                Arguments[4u + s * 5u + 4u] = Arguments[at + 1u];
            }
            uint drawn = 0u;
            for ( uint b = 0u; b < buckets; ++b )
            {
                drawn += min( Arguments[table + b * 4u], Arguments[table + b * 4u + 2u] );
                FeedbackData[b]          = Arguments[table + b * 4u];
                Arguments[table + b * 4u] = 0u;
            }
            FeedbackData[buckets]      = 1u;
            FeedbackData[buckets + 1u] = drawn;
            FeedbackData[buckets + 2u] = Mode.y != 0u ? CameraArguments[3] : drawn;
            FeedbackData[buckets + 3u] = 0u;
            Arguments[3] = drawn; // this frame's drawn count (the stats read Feedback's copy of it)
            FinalizeCascades( buckets, records );
        }
    }
}
