// DesertAsset {"Kind":"Shader","Guid":"5e0c7a3f2b9d4c61a8f4e2b7d9c1a306","Versions":{"SHDR":1},"Dependencies":[]}
// THE GRASS CAMERA CULL (GRASS-VIS-24, GRASS-CULL-1, GRASS-CULL-2; UE InstanceCulling: the instances of every
// primitive culled by ONE dispatch over one table, sized indirect by the GPU's own counts, in a program of its own
// apart from the builder's). Every pass covers the first Rows.x rows of the variety table
// (Common/GrassVarietyRows.glslh: the frame's camera-culled varieties) with one dispatch. A workgroup of 64 threads
// works inside ONE row - its variety, found by a binary search of the rows' first groups (CullDispatch) - so every
// address it reads is uniform across it and a subgroup's appends name one variety's counters. Modes (Hzb.w,
// GrassRenderer.cpp kCullMode*):
//   0 THE DISPATCH (one group): per row the camera cull's groups (its kept count, Args word 3, which GrassFinalize
//     writes after the generation and which stays until the next one), laid end to end in row order as the rows'
//     first groups, and their sum as the camera cull's indirect dispatch (CullDispatchData[0]).
//   1 THE CAMERA CULL: one thread per kept instance, bucket after bucket. Its bounding sphere - from its transform:
//     the bounds centre along its up axis, the bounds radius at its largest axis scale plus a mesh's sway - inside
//     the camera's side planes enters the camera's list in the same bucket, or, hidden by the previous frame's
//     furthest HZB (Hzb.z != 0; Common/GpuCullTests.glslh GpuCull_IsOccludedByHzb, the instance cull's test,
//     through that frame's ViewProjection), becomes phase 2's candidate (its sphere and its row of Instances).
//     Hzb.z = 0: the camera's planes alone.
//   2 THE CAMERA CULL'S FINALIZE (one group): per row the camera's records get phase 1's counts (each bucket's
//     min(count, capacity)) and first instances, each bucket's phase words freeze this frame's candidates and
//     phase 1's drawn count and restart the counters, Feedback gets phase 1's drawn count; the frozen candidates'
//     groups are laid out as phase 2's first groups and dispatch (CullDispatchData[1]).
//   3 PHASE 2 (UE two-phase occlusion): one thread per frozen candidate, after this frame's furthest HZB of the
//     phase-1 depth; a candidate that HZB no longer hides is appended to the camera's list after phase 1's drawn
//     ones. Hzb.z = 0 (no HZB this frame): every candidate is appended, so a missing HZB never drops grass.
//   4 THE POST FINALIZE (one group): per row PostArgs' records - phase 2's appended instances, within what phase 1
//     left of each bucket's capacity, right after phase 1's - and Feedback's phase-2 count.
// Both lists are appended to with one atomic per subgroup and counter (WaveAppend).
Shader "GrassCull"
{
    Compute
    {
        // WaveAppend: one atomic per subgroup and counter (subgroupBallot / subgroupElect / subgroupBroadcastFirst).
        #extension GL_KHR_shader_subgroup_basic : require
        #extension GL_KHR_shader_subgroup_ballot : require

        LocalSize(64, 1, 1);

        #include <Common/GpuCullTests.glslh>
        #include <Common/GrassVarietyRows.glslh>
        #include <Common/ShadowWind.glslh>

        // GrassRenderer.cpp GrassVarietyTableGpu: the camera's side planes, the cascades' caster boxes and the shadow
        // wind rule's origin, then the rows (this pass's first).
        ReadBuffer(0) Varieties
        {
            vec4            VarietyCameraPlanes[4]; // left / right / bottom / top, xyz.n + w >= 0 inside
            // GRASS-CULL-UNIFY-1: list k's caster box at 6k..6k+5 (the six planes of the matrix the k-th live cascade
            // view draws with, MeshRenderer::CascadeCasterBoxSet; GrassRenderer kGrassCascadeLists = 8 lists).
            vec4            VarietyCascadePlanes[48];
            vec4            VarietyCasterWindOrigin; // xyz = MeshRenderer::CasterWindOrigin (the main camera)
            GrassVarietyRow VarietyRows[];
        };

        // This frame's (a graph buffer): [0] the camera cull's VkDispatchIndirectCommand (w: its groups), [1] phase
        // 2's, then per row x = its first group of the camera cull, y = its first group of phase 2
        // (GrassRenderer.cpp kCullDispatchHeader / kPostDispatchOffset).
        Buffer(1) CullDispatch
        {
            uvec4 CullDispatchData[];
        };

        Uniform sampler PointClampSampler;
        // The camera cull: the previous frame's furthest HZB of the view; phase 2: this frame's (neutral: none).
        Uniform(2) texture2D GrassHzbTexture;

        // Mirrors Graphic::System::GrassCullPush (GrassRenderer.cpp).
        PushConstant PushConstants
        {
            mat4  HzbViewProjection; // unjittered, what GrassHzbTexture was built through
            uvec4 Hzb;               // xy = GrassHzbTexture's mip-0 size, z = its mip count (0: nothing tested),
                                     // w = the mode
            uvec4 Rows;              // x = the rows this pass covers (the table's first)
        };

        const uint kModeDispatch       = 0u;
        const uint kModeCameraCull     = 1u;
        const uint kModeCameraFinalize = 2u;
        const uint kModePostOcclusion  = 3u;
        const uint kModePostFinalize   = 4u;

        const uint kCullGroupThreads = 64u;    // LocalSize
        const uint kCullGroupsX      = 65535u; // Vulkan's guaranteed maxComputeWorkGroupCount[0]
        const uint kDispatchHeader   = 2u;     // CullDispatchData's two commands before the rows

        shared uint s_Groups[64];

        // The command that runs @p groups groups (x, y, z; w = @p groups, which the passes exit past): one row up to
        // kCullGroupsX groups, past it as few rows as fit, so fewer groups than rows run idle. None for none.
        uvec4 CullGroups( uint groups )
        {
            uint rows = max( 1u, ( groups + kCullGroupsX - 1u ) / kCullGroupsX );
            return uvec4( ( groups + rows - 1u ) / rows, rows, 1u, groups );
        }

        // Row @p row's first group of the camera cull (@p command 0) or of phase 2 (1).
        uint FirstGroup( uint row, uint command )
        {
            uvec4 entry = CullDispatchData[kDispatchHeader + row];
            if ( command == 0u )
                return entry.x;
            return entry.y;
        }

        // The row the workgroup @p group works in: the last row whose first group is at most @p group (a row with no
        // groups shares its successor's first group and is passed over). Uniform across the workgroup.
        uint RowOfGroup( uint group, uint command )
        {
            uint lo = 0u;
            uint hi = Rows.x;
            while ( hi - lo > 1u )
            {
                uint mid = ( lo + hi ) / 2u;
                if ( FirstGroup( mid, command ) <= group )
                    lo = mid;
                else
                    hi = mid;
            }
            return lo;
        }

        // THE WAVE'S APPEND (GRASS-CULL-1; UE InstanceCulling: one atomic per wave and list): every invocation that
        // calls it takes one entry of the counter at word @p at of @p counters. The subgroup's invocations that name
        // the same counter add their number with ONE atomicAdd and each takes its rank among them. One round per
        // distinct counter among the callers - a subgroup spans a bucket boundary at most once, and its instances
        // split into the visible and the hidden; a subgroup lies inside one workgroup, so inside one variety.
        // Returns the entry's index: the counter before the subgroup's add, plus the rank.
        uint WaveAppend( GrassWords counters, uint at )
        {
            uint index = 0u;
            for ( ;; )
            {
                if ( subgroupBroadcastFirst( at ) == at )
                {
                    uvec4 ballot = subgroupBallot( true );
                    uint  base   = 0u;
                    if ( subgroupElect() )
                        base = atomicAdd( counters.Words[at], subgroupBallotBitCount( ballot ) );
                    index = subgroupBroadcastFirst( base ) + subgroupBallotExclusiveBitCount( ballot );
                    break;
                }
            }
            return index;
        }

        // Mode 1: one thread per kept instance of every row.
        // GRASS-CULL-UNIFY-1, THE CASCADES' LISTS (UE: each shadow view culls the same instances by its own volume;
        // moved here from GrassGenerate's tile cull, which no longer sees any view): instance @p slot of row @p r,
        // in bucket @p b (its table word @p table), goes into each cascade list whose caster box its reach meets - a
        // sphere about its root (after the snow sank it) holding its bounds at the instance's scale @p scale, plus a
        // mesh's sway (Bounds.z). MeshRenderer::BuildGpuInstancedShadowDraws' rule on the root: a mover inside the
        // list's ring [inner, wind distance), else its rest pose (Common/ShadowWind.glslh, the one rule). A mover is
        // appended from the bucket's first slot, a rest caster from its last; a list's bucket never passes the room
        // the full list's bucket has, so the lists share its capacity.
        void CascadeCull( uint r, uint b, uint table, uint slot, vec3 root, float scale )
        {
            uint mask = VarietyRows[r].Info.w;
            if ( mask == 0u )
                return;
            vec4       bounds  = VarietyRows[r].Bounds;
            float      reach   = ( abs( bounds.y ) + bounds.x ) * scale + bounds.z;
            uint       words   = VarietyRows[r].CascadeWords.x;
            uint       rests   = VarietyRows[r].CascadeWords.y;
            GrassWords cascade = GrassWords( VarietyRows[r].CascadeArgs );
            GrassWords ids     = GrassWords( VarietyRows[r].CascadeIds );
            uint       lists   = uint( bitCount( mask ) );
            for ( uint k = 0u; k < lists; ++k )
            {
                bool inside = true;
                for ( uint i = 0u; i < 6u && inside; ++i )
                {
                    vec4 plane = VarietyCascadePlanes[k * 6u + i];
                    inside     = dot( plane.xyz, root ) + plane.w >= -reach;
                }
                if ( !inside )
                    continue;
                vec4 rings = VarietyRows[r].CascadeRings[k / 2u];
                vec2 ring  = ( k & 1u ) == 0u ? rings.xy : rings.zw;
                bool mover = ring.y > 0.0 &&
                             !ShadowWindCastsRestPose( root, VarietyCasterWindOrigin.xyz, ring.y ) &&
                             !ShadowWindInsideRing( root, VarietyCasterWindOrigin.xyz, ring.x );
                uint at      = k * words + table;
                uint room    = cascade.Words[at + 2u];
                uint counter = mover ? at : k * words + rests + b;
                uint index   = WaveAppend( cascade, counter );
                if ( index < room )
                {
                    uint offset = mover ? index : room - 1u - index;
                    ids.Words[cascade.Words[at + 1u] + offset] = slot;
                }
            }
        }

        // Row @p r's cascade lists after CascadeCull (moved from GrassFinalize.shader with the lists): each list's
        // draw records and its rest twin's from the counters, the counters reset for the next frame's cull, and
        // each view's drawn count into Feedback (GrassRenderer kFeedbackTailWords: buckets + 4 + view).
        void FinalizeCascades( uint r, uint buckets, uint records, bool blade )
        {
            GrassWords feedback = GrassWords( VarietyRows[r].Feedback );
            GrassWords cascade  = GrassWords( VarietyRows[r].CascadeArgs );
            for ( uint v = 0u; v < 8u; ++v )
                feedback.Words[buckets + 4u + v] = 0u;
            uint args  = 4u + records * 5u + buckets * 4u;
            uint words = VarietyRows[r].CascadeWords.x;
            uint mask  = VarietyRows[r].Info.w;
            for ( uint k = 0u; mask != 0u; ++k )
            {
                uint view = uint( findLSB( mask ) );
                mask &= mask - 1u;
                uint base  = k * words;
                uint table = base + 4u + records * 5u;
                uint rests = base + VarietyRows[r].CascadeWords.y;
                for ( uint s = 0u; s < records; ++s )
                {
                    uint bucket = GrassBucketOfRecord( s, records, buckets, blade );
                    uint at     = table + bucket * 4u;
                    uint room   = cascade.Words[at + 2u];
                    uint rest   = min( cascade.Words[rests + bucket], room );
                    cascade.Words[base + 4u + s * 5u + 1u]   = min( cascade.Words[at], room );
                    cascade.Words[base + 4u + s * 5u + 4u]   = cascade.Words[at + 1u];
                    cascade.Words[base + args + s * 5u + 1u] = rest;
                    cascade.Words[base + args + s * 5u + 4u] = cascade.Words[at + 1u] + room - rest;
                }
                uint drawn = 0u;
                for ( uint b = 0u; b < buckets; ++b )
                {
                    uint at = table + b * 4u;
                    drawn += min( cascade.Words[at], cascade.Words[at + 2u] ) +
                             min( cascade.Words[rests + b], cascade.Words[at + 2u] );
                    cascade.Words[at]        = 0u;
                    cascade.Words[rests + b] = 0u;
                }
                cascade.Words[base + 3u]            = drawn;
                feedback.Words[buckets + 4u + view] = drawn;
            }
        }

        void CameraCull()
        {
            uint group = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
            if ( group >= CullDispatchData[0].w )
                return;
            uint       r       = RowOfGroup( group, 0u );
            uint       t       = ( group - FirstGroup( r, 0u ) ) * kCullGroupThreads + gl_LocalInvocationIndex;
            GrassWords args    = GrassWords( VarietyRows[r].Args );
            uint       buckets = VarietyRows[r].Info.x;
            uint       tables  = VarietyRows[r].Info.y;
            uint       b       = 0u;
            for ( ; b < buckets; ++b )
            {
                uint kept = min( args.Words[tables + b * 4u], args.Words[tables + b * 4u + 2u] );
                if ( t < kept )
                    break;
                t -= kept;
            }
            if ( b == buckets )
                return;
            uint  table    = tables + b * 4u;
            uint  first    = args.Words[table + 1u];
            uint  capacity = args.Words[table + 2u];
            uint  slot     = first + t;
            mat4  instance = GrassTransforms( VarietyRows[r].Instances ).Transforms[slot];
            vec4  bounds   = VarietyRows[r].Bounds;
            float scale    = max( length( instance[0].xyz ), max( length( instance[1].xyz ), length( instance[2].xyz ) ) );
            float radius   = bounds.x * scale + bounds.z;
            vec3  centre   = instance[3].xyz + bounds.y * instance[1].xyz;
            // The shadow views first: an instance outside the camera's volume still casts into a cascade.
            CascadeCull( r, b, table, slot, instance[3].xyz, scale );
            for ( int i = 0; i < 4; ++i )
            {
                vec4 plane = VarietyCameraPlanes[i];
                if ( dot( plane.xyz, centre ) + plane.w < -radius )
                    return;
            }
            bool hidden = false;
            if ( Hzb.z != 0u )
            {
                GpuCullView view;
                view.HzbExtent = uvec4( Hzb.xyz, 0u );
                hidden = GpuCull_IsOccludedByHzb( GrassHzbTexture, PointClampSampler, view, HzbViewProjection,
                                                  centre - vec3( radius ), centre + vec3( radius ) );
            }
            // The bucket's count in the camera's list, or its candidate count (its first phase word).
            uint at    = hidden ? tables + buckets * 4u + b * 4u : table;
            uint index = WaveAppend( GrassWords( VarietyRows[r].CameraArgs ), at );
            if ( index >= capacity )
                return;
            if ( hidden )
            {
                GrassQuads candidates = GrassQuads( VarietyRows[r].Candidates );
                candidates.Quads[2u * ( first + index )]      = floatBitsToUint( vec4( centre, radius ) );
                candidates.Quads[2u * ( first + index ) + 1u] = uvec4( slot, 0u, 0u, 0u );
            }
            else
                GrassWords( VarietyRows[r].CameraIds ).Words[first + index] = slot;
        }

        // Mode 3: one thread per frozen candidate of every row.
        void PostOcclusion()
        {
            uint group = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
            if ( group >= CullDispatchData[1].w )
                return;
            uint       r       = RowOfGroup( group, 1u );
            uint       t       = ( group - FirstGroup( r, 1u ) ) * kCullGroupThreads + gl_LocalInvocationIndex;
            GrassWords args    = GrassWords( VarietyRows[r].Args );
            GrassWords camera  = GrassWords( VarietyRows[r].CameraArgs );
            uint       buckets = VarietyRows[r].Info.x;
            uint       tables  = VarietyRows[r].Info.y;
            uint       phases  = tables + buckets * 4u;
            uint       b       = 0u;
            for ( ; b < buckets; ++b )
            {
                uint frozen = camera.Words[phases + b * 4u + 1u];
                if ( t < frozen )
                    break;
                t -= frozen;
            }
            if ( b == buckets )
                return;
            uint       table      = tables + b * 4u;
            uint       phase      = phases + b * 4u;
            uint       first      = args.Words[table + 1u];
            uint       slot       = first + t;
            GrassQuads candidates = GrassQuads( VarietyRows[r].Candidates );
            vec4       sphere     = uintBitsToFloat( candidates.Quads[2u * slot] );
            GpuCullView view;
            view.HzbExtent = uvec4( Hzb.xyz, 0u );
            if ( Hzb.z != 0u &&
                 GpuCull_IsOccludedByHzb( GrassHzbTexture, PointClampSampler, view, HzbViewProjection,
                                          sphere.xyz - vec3( sphere.w ), sphere.xyz + vec3( sphere.w ) ) )
                return;
            uint index = camera.Words[phase + 2u] + WaveAppend( camera, phase + 3u );
            if ( index < args.Words[table + 2u] )
                GrassWords( VarietyRows[r].CameraIds ).Words[first + index] = candidates.Quads[2u * slot + 1u].x;
        }

        // Mode 2, row @p r: phase 1's camera records and Feedback count, and the phase words frozen for phase 2;
        // returns the candidates frozen (phase 2's threads).
        uint FinalizeCamera( uint r )
        {
            GrassWords camera  = GrassWords( VarietyRows[r].CameraArgs );
            bool       blade   = ( VarietyRows[r].Info.z & kGrassRowBlade ) != 0u;
            uint       buckets = camera.Words[0];
            uint       records = camera.Words[2];
            uint       table   = 4u + records * 5u;
            uint       phases  = table + buckets * 4u;
            for ( uint s = 0u; s < records; ++s )
            {
                uint at = table + GrassBucketOfRecord( s, records, buckets, blade ) * 4u;
                camera.Words[4u + s * 5u + 1u] = min( camera.Words[at], camera.Words[at + 2u] );
                camera.Words[4u + s * 5u + 4u] = camera.Words[at + 1u];
            }
            FinalizeCascades( r, buckets, records, blade );
            uint drawn      = 0u;
            uint candidates = 0u;
            for ( uint b = 0u; b < buckets; ++b )
            {
                uint at    = table + b * 4u;
                uint phase = phases + b * 4u;
                uint seen  = min( camera.Words[at], camera.Words[at + 2u] );
                drawn += seen;
                camera.Words[phase + 1u] = min( camera.Words[phase], camera.Words[at + 2u] );
                candidates += camera.Words[phase + 1u];
                camera.Words[phase]      = 0u;
                camera.Words[phase + 2u] = seen;
                camera.Words[phase + 3u] = 0u;
                camera.Words[at]         = 0u;
            }
            camera.Words[3] = drawn;
            GrassWords feedback = GrassWords( VarietyRows[r].Feedback );
            feedback.Words[buckets + 2u] = drawn;
            feedback.Words[buckets + 3u] = 0u;
            return candidates;
        }

        // Mode 4, row @p r: phase 2's records - each bucket's appended count, within what phase 1 left of its
        // capacity, after phase 1's drawn instances - and Feedback's phase-2 count.
        uint PostCount( GrassWords camera, uint at, uint phase )
        {
            uint room = camera.Words[at + 2u] - min( camera.Words[phase + 2u], camera.Words[at + 2u] );
            return min( camera.Words[phase + 3u], room );
        }
        void FinalizePost( uint r )
        {
            GrassWords camera  = GrassWords( VarietyRows[r].CameraArgs );
            GrassWords post    = GrassWords( VarietyRows[r].PostArgs );
            bool       blade   = ( VarietyRows[r].Info.z & kGrassRowBlade ) != 0u;
            uint       buckets = camera.Words[0];
            uint       records = camera.Words[2];
            uint       table   = 4u + records * 5u;
            uint       phases  = table + buckets * 4u;
            for ( uint s = 0u; s < records; ++s )
            {
                uint b = GrassBucketOfRecord( s, records, buckets, blade );
                post.Words[4u + s * 5u + 1u] = PostCount( camera, table + b * 4u, phases + b * 4u );
                post.Words[4u + s * 5u + 4u] = camera.Words[table + b * 4u + 1u] + camera.Words[phases + b * 4u + 2u];
            }
            uint drawn = 0u;
            for ( uint b = 0u; b < buckets; ++b )
                drawn += PostCount( camera, table + b * 4u, phases + b * 4u );
            post.Words[3] = drawn;
            GrassWords( VarietyRows[r].Feedback ).Words[buckets + 3u] = drawn;
        }

        // The threads row @p r gives the next pass: its kept instances (mode 0) or, finalizing its camera list, its
        // frozen candidates (mode 2).
        uint RowThreads( uint r )
        {
            if ( Hzb.w == kModeDispatch )
                return GrassWords( VarietyRows[r].Args ).Words[3];
            return FinalizeCamera( r );
        }

        // Modes 0 and 2, one group: row i's work by thread i % 64, 64 rows at a time, and the rows' groups of the
        // next pass laid end to end in row order - row i's first group into its CullDispatchData entry (x for
        // @p command 0, y for 1), their sum into command @p command.
        void LayOutRows( uint command )
        {
            uint base = 0u;
            for ( uint chunk = 0u; chunk < Rows.x; chunk += kCullGroupThreads )
            {
                uint row    = chunk + gl_LocalInvocationIndex;
                uint groups = 0u;
                if ( row < Rows.x )
                    groups = ( RowThreads( row ) + kCullGroupThreads - 1u ) / kCullGroupThreads;
                s_Groups[gl_LocalInvocationIndex] = groups;
                barrier();
                uint first = base;
                for ( uint k = 0u; k < gl_LocalInvocationIndex; ++k )
                    first += s_Groups[k];
                uint total = 0u;
                for ( uint k = 0u; k < kCullGroupThreads; ++k )
                    total += s_Groups[k];
                if ( row < Rows.x )
                {
                    if ( command == 0u )
                        CullDispatchData[kDispatchHeader + row].x = first;
                    else
                        CullDispatchData[kDispatchHeader + row].y = first;
                }
                base += total;
                barrier();
            }
            if ( gl_LocalInvocationIndex == 0u )
                CullDispatchData[command] = CullGroups( base );
        }

        void main()
        {
            if ( Hzb.w == kModeCameraCull )
            {
                CameraCull();
                return;
            }
            if ( Hzb.w == kModePostOcclusion )
            {
                PostOcclusion();
                return;
            }
            if ( Hzb.w == kModePostFinalize )
            {
                for ( uint r = gl_LocalInvocationIndex; r < Rows.x; r += kCullGroupThreads )
                    FinalizePost( r );
                return;
            }
            if ( Hzb.w == kModeDispatch )
                LayOutRows( 0u );
            else
                LayOutRows( 1u );
        }
    }
}
