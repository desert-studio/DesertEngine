// DesertAsset {"Kind":"Shader","Guid":"5e0c7a3f2b9d4c61a8f4e2b7d9c1a306","Versions":{"SHDR":1},"Dependencies":[]}
// THE GRASS CAMERA CULL (GRASS-VIS-24, GRASS-CULL-1, GRASS-CULL-2; UE InstanceCulling: the instances of every
// primitive culled by ONE dispatch over one table, sized indirect by the GPU's own counts, in a program of its own
// apart from the builder's). Every pass covers the first Rows.x rows of the variety table
// (Common/GrassVarietyRows.glslh: the frame's camera-culled varieties) with one dispatch. A workgroup of 64 threads
// works inside ONE row - its variety, found by a binary search of the rows' first groups (CullDispatch) - so every
// address it reads is uniform across it and a subgroup's appends name one variety's counters. Modes (Hzb.w,
// GrassRenderer.cpp kCullMode*):
//   0 THE DISPATCH (one group): per row the camera cull's groups - kChunksPerGroup of its chunks a group (Chunks
//     word 0, which the tile cull writes with the instances and which stays until the next one) - laid end to end
//     in row order as the rows' first groups, and their sum as the camera cull's indirect dispatch
//     (CullDispatchData[0]).
//   1 THE CAMERA CULL (GRASS-CULL-HIER-1; UE InstanceCulling / Nanite: a node's bound is culled by its group of
//     lanes, and the lanes of a node that survives cull its own elements in the same pass, with no list of them in
//     between): one subgroup per chunk - a tile cluster's run of one bucket's slots (Common/GrassVarietyRows.glslh
//     kGrassChunkWords), at most a subgroup of them - and one lane per instance of it (CullChunk). The chunk's box
//     meets the views first: each cascade list's caster box (none while the row's lists hold,
//     kGrassRowCascadesHeld), the camera's side planes and the previous frame's HZB; a chunk no view meets costs its
//     subgroup its eight words. Each lane's instance is then tested only by the views its chunk met: into each
//     cascade list whose box met the chunk (CascadeCull), and - the camera's planes met - by its own bounding
//     sphere. Its bounding sphere - from its transform: the bounds centre along its up axis, the bounds radius at
//     its largest axis scale plus a mesh's sway - inside the camera's side planes enters the camera's list in the
//     same bucket, or, hidden by the previous frame's furthest HZB (Hzb.z != 0; Common/GpuCullTests.glslh
//     GpuCull_IsOccludedByHzb, the instance cull's test, through that frame's ViewProjection), becomes phase 2's
//     candidate (its sphere and its row of Instances). Hzb.z = 0: the camera's planes alone. A chunk the previous
//     HZB hides whole makes its instances candidates without their own test (an instance's box lies inside its
//     chunk's).
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
        // WaveAppend: one atomic per subgroup and counter (subgroupBallot / subgroupElect / subgroupBroadcastFirst);
        // the camera cull's subgroup per chunk (gl_SubgroupID / gl_NumSubgroups / gl_SubgroupInvocationID /
        // gl_SubgroupSize).
        #extension GL_KHR_shader_subgroup_basic : require
        #extension GL_KHR_shader_subgroup_ballot : require

        LocalSize(64, 1, 1);

        #include <Common/GpuCullTests.glslh>
        #include <Common/GrassVarietyRows.glslh>
        #include <Common/GrassFieldShare.glslh>
        // The view law's field (Common/GrassTileRecord.glslh GRASS_FIELD_WORD): the row's Field, set per chunk.
        GrassWords g_Field;
        #define GRASS_FIELD_WORD( i ) g_Field.Words[i]
        #include <Common/GrassTileRecord.glslh>
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
        // 2's, then per row x = its first group of the camera cull, y = of phase 2 (GrassRenderer.cpp
        // kCullDispatchHeader / kPostDispatchOffset).
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
        // The camera cull's chunks per group: one per subgroup of LocalSize on a 32-lane device. Any other subgroup
        // size stays correct - a 64-lane subgroup culls both in turn, a 16-lane device's spare subgroups idle.
        const uint kChunksPerGroup = 2u;
        const uint kCullGroupsX    = 65535u; // Vulkan's guaranteed maxComputeWorkGroupCount[0]
        const uint kDispatchHeader = 2u;     // CullDispatchData's two commands before the rows
        // A chunk's reach is grown by this share: the tile cull and this pass compute the same largest axis scale,
        // up to their rounding (a few ulps), and the chunk must hold every instance's sphere.
        const float kChunkReachSlack = 1.0e-4;

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
            return CullDispatchData[kDispatchHeader + row][command];
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

        // THE VIEW LAW (GRASS-VIEW-LAW-1, moved unchanged from GrassGenerate's tile cull, which is gone; UE landscape
        // grass: the instance cull's distance cull, LOD and PerInstanceFadeAmount, per view and frame): record @p r of
        // a variety with params @p p and bucket table in @p args - false when the law drops it, else its instance
        // @p outInstance (the row the draws read) and its bucket @p outBucket. Everything view-dependent is here.
        bool GrassViewLaw( GrassParams p, GrassWords args, TileRecord r, out mat4 outInstance, out uint outBucket )
        {
            vec2  world  = r.Position.xz;
            float height = r.Position.y;
            if ( r.Thin >= p.ScaleZ.w && r.Thin >= LensShare( world, height, p ) )
                return false;
            uint flags = floatBitsToUint( p.ScaleX.w );
            bool blade = ( flags & kFlagBlade ) != 0u;
            vec3  position = r.Position;
            float distance = length( position - p.Camera.xyz );
            if ( distance > p.Cull.y )
                return false;
            // The share of the field kept at this distance (GRASS-DENSITY, Common/GrassFieldShare.glslh - the one
            // law the ground's far cover reads too): beyond StartCullDistance S it falls with the square of the
            // distance, (S / d)^2, so the instances a band of ground spends stay those it spent at S (UE's grass
            // HISM keeps the full density out to its cull distance and spends it on sub-pixel instances) - and on
            // top of that UE's fade, down to nothing at EndCullDistance. Nested: an instance kept at d is kept
            // nearer, so walking does not reshuffle the field, only thins it; before S every instance is kept.
            float                 fade  = r.Fade;
            const GrassFieldShare field = GrassFieldShareAt( distance, p.Cull.x, p.Cull.y, p.Spread.w );
            float                 kept  = field.Kept;
            // The screen-width floor (GRASS-WIDTH; GoT / UE landscape grass LOD): past D the mean blade would stand
            // narrower than MinScreenWidth pixels, so the field keeps (D / d)^2 of its blades - the law the thinning
            // above widens by (a survivor widened 1/sqrt(share): d / D here) - and each kept blade's own card is
            // lifted to the floor width (below), so no blade strip goes sub-pixel. The same nested draw: a blade
            // kept at d is kept nearer.
            if ( fade >= min( kept, field.Floor ) )
                return false;
            vec3 scale = r.Scale;
            // Coverage kept through the thinning (GoT / UE-style distance thinning; GrassFieldShare): a BLADE
            // survivor stands for the 1/share blades dropped around it, so its width grows by 1/sqrt(share) - at its
            // own height the field keeps sqrt(share) of its covered area, and the ground's far cover takes on the
            // rest (LandscapeGrassCover, GrassFieldCoverage); the height drops only over the last 20 % of the range.
            // A MESH survivor's cards cost their pixels, so it keeps its size and shrinks uniformly to nothing over
            // the last stretch before its own draw drops it - at EndCullDistance too (GPU-COST-29; UE grass HISM:
            // no survivor growth, PerInstanceFadeAmount / scale to zero; GrassMeshShrink, GrassMeshCoverage): its
            // card overdraw falls with distance as the field thins, and the cover takes on what it leaves.
            // THE BLADE'S LOD (GPU-FRAME-4, GrassFieldShare.glslh GrassBladeLodAt - the one choice): its strip level
            // and the far LOD's share from its bounding radius on screen - its own height scale, before the
            // thinning's height and its clump's, over the far LOD's screen size (p.Lod.z: where a blade at scale 1
            // stands at it). The far LOD drops the blades of the same nested draw the thinning above drops by (an
            // instance kept at a share is kept at every larger one), so the field thins one blade at a time and
            // never reshuffles; its survivors widen by 1 / Share below, past the floor's lift.
            GrassBladeLod lod = GrassBladeLod( 0u, 1.0, 1.0 );
            if ( blade )
            {
                uint  ring = p.Blade.x > 0.0 ? min( uint( distance / p.Blade.x ), p.Buckets.x - 1u ) : 0u;
                float size = p.Lod.z > 0.0 ? p.Lod.z * scale.y / max( distance, 1.0 ) : 0.0;
                lod        = GrassBladeLodAt( size, ring, p.Buckets.x );
                float share = min( kept, field.Floor ) * lod.Share;
                if ( fade >= share )
                    return false;
                scale.x *= field.Widen;
                scale.z *= field.Widen;
                // The blade the thinning is about to drop lowers into the ground first (GrassBladeFade: stable per
                // instance, no pop), and the survivors widen by what that lowering takes from the field's area.
                scale.y *= field.Height * GrassBladeFade( share, fade );
                lod.Widen *= GrassBladeFadeWiden( share );
            }
            else
            {
                // THE MESH'S FAR LOD (GPU-FRAME-6, GrassFieldShare.glslh GrassMeshFarLod): the blade's far-LOD
                // share by its bounding radius on screen - its largest axis scale over the far LOD's screen size
                // (p.Lod.z: where the mesh at scale 1 stands at it) - on top of the distance thinning, and the
                // instance it drops shrinks to nothing first (GrassMeshShrink over both shares).
                float           size    = p.Lod.z > 0.0
                                              ? p.Lod.z * max( scale.x, max( scale.y, scale.z ) ) / max( distance, 1.0 )
                                              : 0.0;
                GrassFieldShare reached = GrassMeshFarLod( field, size );
                if ( fade >= reached.Kept )
                    return false;
                scale *= GrassMeshShrink( reached, fade );
            }
            // The bounding sphere's centre (a mesh's level below); no view volume here (BoxCulled).
            vec3 centre = position + vec3( 0.0, p.ScaleY.z * scale.y, 0.0 );
            vec3 draws = r.Draws;
            if ( blade )
            {
                scale.y *= r.Clump;
                // The screen-width floor (GRASS-WIDTH): the card this instance draws (Mesh/Surface/GrassBlade.glslh
                // BladeCard, from draws.x) stands at least MinScreenWidth pixels wide - its width scale lifted to
                // Lean.w x d / its width - so no strip of the blade goes sub-pixel.
                if ( p.Lean.w > 0.0 && p.Buckets.z > 0u )
                {
                    uint  count = min( p.Buckets.z, 8u );
                    uint  card  = min( uint( draws.x * float( count ) ), count - 1u );
                    float width = p.CardWidths[card / 4u][card % 4u];
                    scale.x     = max( scale.x, p.Lean.w * distance / max( width, 1.0e-3 ) );
                }
                // The far LOD's survivor stands for the 1 / Share blades it keeps the place of: its card - lifted to
                // the floor first - is that much wider, so the field covers what it did.
                scale.x *= lod.Widen;
            }
            mat3 basis = BasisFromQuat( r.Rotation );

            uint bucket = 0u;
            if ( blade )
                bucket = lod.Level;
            else if ( p.Buckets.x > 1u )
            {
                // A mesh's level (UE ComputeStaticMeshLOD over the grass HISM's instances, Geometry::
                // SelectLODFromBounds): the coarsest whose screen size the instance's bounding sphere is still below.
                float r        = p.Lod.x * max( scale.x, max( scale.y, scale.z ) );
                vec3  d        = centre - p.Camera.xyz;
                float screenSq = r * r / max( 1.0, dot( d, d ) * p.Lod.y );
                for ( uint level = p.Buckets.x - 1u; level > 0u; --level )
                {
                    float halfSize = uintBitsToFloat( args.Words[p.Buckets.y + level * 4u + 3u] );
                    if ( halfSize * halfSize > screenSq )
                    {
                        bucket = level;
                        break;
                    }
                }
            }
            // The world's snow (Common/WeatherSnow.glslh) lies p.Lean.z deep over the ground: the root sinks by it,
            // so the instance shows only what stands above the snow and one shorter than it is buried whole.
            position.y -= p.Lean.z;
            // A blade's forward axis carries the height multiplier (its lean is a share of its height) and the
            // columns' w its three draws (Vertex_GrassBlade.glslh); a mesh's are its scaled axes.
            mat4 instance;
            if ( blade )
                instance = mat4( vec4( basis[0] * scale.x, draws.x ), vec4( basis[1] * scale.y, draws.y ),
                                 vec4( basis[2] * scale.y, draws.z ), vec4( position, 1.0 ) );
            else
                instance = mat4( vec4( basis[0] * scale.x, 0.0 ), vec4( basis[1] * scale.y, 0.0 ),
                                 vec4( basis[2] * scale.z, 0.0 ), vec4( position, 1.0 ) );
            outInstance = instance;
            outBucket   = bucket;
            return true;
        }

        // An upper bound of the law's largest axis scale for a record whose scale before it is @p scale (the
        // chunk's, GrassGenerate RecordScale), at a distance up to @p farthest: a blade's width widens by the
        // thinning (GrassFieldShare Widen, which grows with distance), is lifted to the screen-width floor and
        // widens by its far LOD and its fade (GrassBladeLod Widen = 1 / Share <= 2, GrassBladeFadeWiden <= 2); its
        // height and a mesh only shrink.
        float GrassLawScaleBound( GrassParams p, float farthest, float scale )
        {
            if ( ( floatBitsToUint( p.ScaleX.w ) & kFlagBlade ) == 0u )
                return scale;
            const GrassFieldShare rim   = GrassFieldShareAt( farthest, p.Cull.x, p.Cull.y, p.Spread.w );
            float                 width = scale * rim.Widen;
            if ( p.Lean.w > 0.0 && p.Buckets.z > 0u )
            {
                float narrowest = 1.0e30;
                for ( uint card = 0u; card < min( p.Buckets.z, 8u ); ++card )
                    narrowest = min( narrowest, p.CardWidths[card / 4u][card % 4u] );
                width = max( width, p.Lean.w * farthest / max( narrowest, 1.0e-3 ) );
            }
            return max( scale, 4.0 * width );
        }

        // THE CASCADES' PLANE RULE (GRASS-CULL-UNIFY-1; UE: each shadow view culls the same instances by its own
        // volume): the cascade lists (bit k: list k) whose caster box an instance's reach meets - a sphere about its
        // root @p root (after the snow sank it) holding its bounds at the instance's scale @p scale, plus a mesh's sway
        // (Bounds.z) - of the lists whose box met its chunk (bit k of @p met, CullChunk; none while the lists hold).
        // The one statement of it: the full list keeps an instance some list holds (CullInstance), CascadeCull
        // appends it to exactly those lists.
        uint CascadeHolds( uint r, vec3 root, float scale, uint met )
        {
            uint holds = 0u;
            if ( VarietyRows[r].Info.w == 0u || met == 0u )
                return holds;
            vec4  bounds = VarietyRows[r].Bounds;
            float reach  = ( abs( bounds.y ) + bounds.x ) * scale + bounds.z;
            uint  lists  = uint( bitCount( VarietyRows[r].Info.w ) );
            for ( uint k = 0u; k < lists; ++k )
            {
                bool inside = ( met & ( 1u << k ) ) != 0u;
                for ( uint i = 0u; i < 6u && inside; ++i )
                {
                    vec4 plane = VarietyCascadePlanes[k * 6u + i];
                    inside     = dot( plane.xyz, root ) + plane.w >= -reach;
                }
                if ( inside )
                    holds |= 1u << k;
            }
            return holds;
        }

        // Mode 1, a lane's instance of a chunk.
        // GRASS-CULL-UNIFY-1, THE CASCADES' LISTS (moved here from GrassGenerate's tile cull, which no longer sees any
        // view): the instance at row @p slot of Instances (REGRESS-CACHE-1: its full list's slot) of row @p r, in
        // bucket @p b (its table word @p table), goes into each cascade list of @p holds (CascadeHolds).
        // MeshRenderer::BuildGpuInstancedShadowDraws' rule on the root @p root: a mover inside the
        // list's ring [inner, wind distance), else its rest pose (Common/ShadowWind.glslh, the one rule). A mover is
        // appended from the bucket's first slot, a rest caster from its last; a list's bucket never passes the room
        // the full list's bucket has, so the lists share its capacity.
        void CascadeCull( uint r, uint b, uint table, uint slot, vec3 root, uint holds )
        {
            uint       words   = VarietyRows[r].CascadeWords.x;
            uint       rests   = VarietyRows[r].CascadeWords.y;
            GrassWords cascade = GrassWords( VarietyRows[r].CascadeArgs );
            GrassWords ids     = GrassWords( VarietyRows[r].CascadeIds );
            uint       lists   = uint( bitCount( VarietyRows[r].Info.w ) );
            for ( uint k = 0u; k < lists; ++k )
            {
                if ( ( holds & ( 1u << k ) ) == 0u )
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

        // Row @p r's chunks, never past their room (Chunks words 0 and 2: one per tile slot and cluster).
        uint ChunkCount( uint r )
        {
            GrassWords chunks = GrassWords( VarietyRows[r].Chunks );
            return min( chunks.Words[0], chunks.Words[2] );
        }

        // A box (@p centre, @p extent) reaches the inner side of @p plane (xyz.n + w >= 0 inside).
        bool BoxMeetsPlane( vec4 plane, vec3 centre, vec3 extent )
        {
            return dot( plane.xyz, centre ) + plane.w >= -dot( abs( plane.xyz ), extent );
        }

        const uint kNoGrassRow = 0xFFFFFFFFu;

        // Mode 1, one lane: row @p r's instance @p instance (a pool record after the law) in bucket @p b, of a
        // chunk that met the cascade lists @p met (bit k: list k), the camera's side planes (@p seen) and - inside
        // them - was hidden whole by the previous frame's HZB (@p chunkHidden); @p held: the row's cascades and its
        // full list hold from an earlier frame. A view that keeps it gets its ROW (REGRESS-CACHE-1; UE: the culled
        // instances' compacted buffer, sized by what the views keep): Instances is three regions of Args' capacity
        // (word 1; GrassRenderer kGrassRowRegions) laid out as the lists - the full list's slot, else the camera's
        // own or its candidates' slot - and every list holds that row, not the pool record. The cascades and the
        // full list (the union of the cascades and the camera's planes - RT and the list-less cascades draw it, so
        // grass the camera cannot see still casts and reflects) take the full slot; the camera takes it too when the
        // full list took the instance this frame. A held full list keeps the rows of the frame that culled it, as
        // its cascades keep their depth.
        void CullInstance( uint r, uint b, mat4 instance, uint met, bool seen, bool chunkHidden, bool held )
        {
            uint       tables   = VarietyRows[r].Info.y;
            uint       buckets  = VarietyRows[r].Info.x;
            uint       table    = tables + b * 4u;
            GrassWords args     = GrassWords( VarietyRows[r].Args );
            uint       first    = args.Words[table + 1u];
            uint       capacity = args.Words[table + 2u];
            uint       room     = args.Words[1u];
            vec4       bounds   = VarietyRows[r].Bounds;
            float scale  = max( length( instance[0].xyz ), max( length( instance[1].xyz ), length( instance[2].xyz ) ) );
            float radius = bounds.x * scale + bounds.z;
            vec3  centre = instance[3].xyz + bounds.y * instance[1].xyz;
            // The shadow views first: an instance outside the camera's volume still casts into a cascade.
            uint holds  = CascadeHolds( r, instance[3].xyz, scale, met );
            bool inside = seen;
            for ( int i = 0; i < 4 && inside; ++i )
            {
                vec4 plane = VarietyCameraPlanes[i];
                inside     = dot( plane.xyz, centre ) + plane.w >= -radius;
            }
            if ( holds == 0u && !inside )
                return;
            uint row = kNoGrassRow;
            if ( !held )
            {
                uint full = WaveAppend( args, table );
                if ( full < capacity )
                {
                    row = first + full;
                    GrassInstanceRows( VarietyRows[r].Instances ).Rows[row] = instance;
                    GrassWords( VarietyRows[r].FullIds ).Words[row] = row;
                    CascadeCull( r, b, table, row, instance[3].xyz, holds );
                }
            }
            if ( !inside )
                return;
            // Hidden by the previous frame's HZB: its chunk's whole box was, or its own sphere's box is.
            bool hidden = chunkHidden;
            if ( !hidden && Hzb.z != 0u )
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
            if ( row == kNoGrassRow )
            {
                uint region = 1u;
                if ( hidden )
                    region = 2u;
                row = region * room + first + index;
                GrassInstanceRows( VarietyRows[r].Instances ).Rows[row] = instance;
            }
            if ( hidden )
            {
                GrassQuads candidates = GrassQuads( VarietyRows[r].Candidates );
                candidates.Quads[2u * ( first + index )]      = floatBitsToUint( vec4( centre, radius ) );
                candidates.Quads[2u * ( first + index ) + 1u] = uvec4( row, 0u, 0u, 0u );
            }
            else
                GrassWords( VarietyRows[r].CameraIds ).Words[first + index] = row;
        }

        // Mode 1, the build's chunk @p c of row @p r (Common/GrassVarietyRows.glslh, the chunk contract), by ONE whole
        // subgroup (its every lane calls it with the same chunk, so what it reads and decides of the chunk is uniform
        // across the subgroup). The chunk past the cull radius of the field eye is dropped whole; so is the tail of
        // its run the share at the box's nearest point drops (the run ascends by fade draw and the share only falls
        // with distance). Its box - the roots' box sunk by the snow and grown by the reach of the law's largest axis
        // scale, ( |centre height| + radius ) x scale + pad (GrassLawScaleBound) - holds every instance's camera
        // sphere and cascade sphere, so a view the box misses misses each of them. The lanes then take the run's
        // records, a subgroup's width at a time: the law, then CullInstance.
        void CullChunk( uint r, uint c )
        {
            GrassWords chunks = GrassWords( VarietyRows[r].Chunks );
            uint       at     = GrassChunkAt( c );
            uint       count  = chunks.Words[at + kGrassChunkCountWord];
            if ( count == 0u )
                return;
            GrassParams p  = GrassParamsRef( VarietyRows[r].Params ).Data[0];
            vec3        lo = uintBitsToFloat( uvec3( chunks.Words[at], chunks.Words[at + 1u], chunks.Words[at + 2u] ) );
            vec3        hi = uintBitsToFloat( uvec3( chunks.Words[at + 3u], chunks.Words[at + 4u], chunks.Words[at + 5u] ) );
            const vec3  eye     = p.Camera.xyz;
            float       nearest = length( clamp( eye, lo, hi ) - eye );
            if ( nearest > p.Cull.y )
                return;
            const GrassFieldShare head = GrassFieldShareAt( nearest, p.Cull.x, p.Cull.y, p.Spread.w );
            const float           cut  = min( head.Kept, head.Floor );
            float farthest = length( max( abs( lo - eye ), abs( hi - eye ) ) );
            float scale    = GrassLawScaleBound( p, farthest, uintBitsToFloat( chunks.Words[at + kGrassChunkScaleWord] ) );
            vec4  bounds   = VarietyRows[r].Bounds;
            float reach    = ( ( abs( bounds.y ) + bounds.x ) * scale + bounds.z ) * ( 1.0 + kChunkReachSlack );
            lo.y -= p.Lean.z;
            hi.y -= p.Lean.z;
            lo -= vec3( reach );
            hi += vec3( reach );
            vec3 centre = 0.5 * ( lo + hi );
            vec3 extent = 0.5 * ( hi - lo );
            uint met    = 0u;
            bool held   = ( VarietyRows[r].Info.z & kGrassRowCascadesHeld ) != 0u;
            if ( !held )
            {
                uint lists = uint( bitCount( VarietyRows[r].Info.w ) );
                for ( uint k = 0u; k < lists; ++k )
                {
                    bool inside = true;
                    for ( uint i = 0u; i < 6u && inside; ++i )
                        inside = BoxMeetsPlane( VarietyCascadePlanes[k * 6u + i], centre, extent );
                    if ( inside )
                        met |= 1u << k;
                }
            }
            bool seen = true;
            for ( uint i = 0u; i < 4u && seen; ++i )
                seen = BoxMeetsPlane( VarietyCameraPlanes[i], centre, extent );
            bool hidden = false;
            if ( seen && Hzb.z != 0u )
            {
                GpuCullView view;
                view.HzbExtent = uvec4( Hzb.xyz, 0u );
                hidden = GpuCull_IsOccludedByHzb( GrassHzbTexture, PointClampSampler, view, HzbViewProjection, lo, hi );
            }
            if ( met == 0u && !seen )
                return;
            g_Field          = GrassWords( VarietyRows[r].Field );
            GrassWords args  = GrassWords( VarietyRows[r].Args );
            GrassSegments pool = GrassSegments( VarietyRows[r].TilePool );
            uint       first = chunks.Words[at + kGrassChunkFirstWord];
            uint       page  = chunks.Words[at + kGrassChunkPageWord];
            for ( uint base = 0u; base < count; base += gl_SubgroupSize )
            {
                uint i = base + gl_SubgroupInvocationID;
                if ( i >= count )
                    continue;
                uint       record = GrassChunkRecord( first, page, i );
                GrassQuads quads  = GrassPoolSegment( pool, record );
                uint       quad   = GrassSegmentRecord( record ) * kGrassTilePoolWords;
                uvec4      head0  = quads.Quads[quad];
                if ( unpackUnorm2x16( head0.w ).x >= cut )
                    continue;
                TileRecord tile = GrassUnpackRecord( head0, quads.Quads[quad + 1u], quads.Quads[quad + 2u] );
                mat4 instance;
                uint b;
                if ( GrassViewLaw( p, args, tile, instance, b ) )
                    CullInstance( r, b, instance, met, seen, hidden, held );
            }
        }

        // Mode 1: kChunksPerGroup chunks of one row per workgroup (CullDispatchData[0]), one per subgroup.
        void CameraCull()
        {
            uint group = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
            if ( group >= CullDispatchData[0].w )
                return;
            uint r      = RowOfGroup( group, 0u );
            uint chunks = ChunkCount( r );
            uint first  = ( group - FirstGroup( r, 0u ) ) * kChunksPerGroup;
            for ( uint j = gl_SubgroupID; j < kChunksPerGroup; j += gl_NumSubgroups )
            {
                if ( first + j < chunks )
                    CullChunk( r, first + j );
            }
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

        // Row @p r's cascade lists held (kGrassRowCascadesHeld): this frame in flight's Feedback copy gets each view's
        // drawn count from its list's own drawn word (FinalizeCascades wrote it when it culled the list), so a list
        // holds from the frame after the one that culled it and every copy still tells what each view draws.
        void HeldCascadeFeedback( uint r, uint buckets )
        {
            GrassWords feedback = GrassWords( VarietyRows[r].Feedback );
            GrassWords cascade  = GrassWords( VarietyRows[r].CascadeArgs );
            for ( uint v = 0u; v < 8u; ++v )
                feedback.Words[buckets + 4u + v] = 0u;
            uint words = VarietyRows[r].CascadeWords.x;
            uint mask  = VarietyRows[r].Info.w;
            for ( uint k = 0u; mask != 0u; ++k )
            {
                uint view = uint( findLSB( mask ) );
                mask &= mask - 1u;
                feedback.Words[buckets + 4u + view] = cascade.Words[k * words + 3u];
            }
        }

        // Modes 0 / 2, row @p r's FULL LIST (GRASS-VIEW-LAW-1; moved from GrassFinalize.shader): Args' bucket counts
        // zeroed before a frame that culls its cascades appends anew (mode 0), then its records, its unclamped counts
        // into this frame in flight's Feedback (the renderer sizes the capacities from them), the written mark and
        // the drawn count (mode 2). A row whose cascades hold keeps the counts and the list of the frame that culled
        // them, so every frame in flight's copy gets them.
        void ResetFull( uint r )
        {
            GrassWords args    = GrassWords( VarietyRows[r].Args );
            uint       tableAt = 4u + args.Words[2] * 5u;
            for ( uint b = 0u; b < args.Words[0]; ++b )
                args.Words[tableAt + b * 4u] = 0u;
        }
        void FinalizeFull( uint r )
        {
            GrassWords args     = GrassWords( VarietyRows[r].Args );
            GrassWords feedback = GrassWords( VarietyRows[r].Feedback );
            bool       blade    = ( VarietyRows[r].Info.z & kGrassRowBlade ) != 0u;
            uint       buckets  = args.Words[0];
            uint       records  = args.Words[2];
            uint       table    = 4u + records * 5u;
            for ( uint s = 0u; s < records; ++s )
            {
                uint at = table + GrassBucketOfRecord( s, records, buckets, blade ) * 4u;
                args.Words[4u + s * 5u + 1u] = min( args.Words[at], args.Words[at + 2u] );
                args.Words[4u + s * 5u + 4u] = args.Words[at + 1u];
            }
            uint drawn = 0u;
            for ( uint b = 0u; b < buckets; ++b )
            {
                drawn += min( args.Words[table + b * 4u], args.Words[table + b * 4u + 2u] );
                feedback.Words[b] = args.Words[table + b * 4u];
            }
            feedback.Words[buckets]      = 1u;
            feedback.Words[buckets + 1u] = drawn;
            args.Words[3]                = drawn;
        }

        // Mode 2, row @p r: phase 1's camera records and Feedback count, and the phase words frozen for phase 2;
        // returns the candidates frozen (phase 2's threads).
        uint FinalizeCamera( uint r )
        {
            FinalizeFull( r );
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
            if ( ( VarietyRows[r].Info.z & kGrassRowCascadesHeld ) == 0u )
                FinalizeCascades( r, buckets, records, blade );
            else
                HeldCascadeFeedback( r, buckets );
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

        // What row @p r gives the next pass: its chunks (mode 0, kChunksPerGroup a group) or, finalizing its camera
        // list, its frozen candidates (mode 2, a thread each).
        uint RowGroups( uint r )
        {
            if ( Hzb.w == kModeDispatch )
            {
                // A row whose cascades are culled this frame appends its full list anew (CullInstance).
                if ( ( VarietyRows[r].Info.z & kGrassRowCascadesHeld ) == 0u )
                    ResetFull( r );
                return ( ChunkCount( r ) + kChunksPerGroup - 1u ) / kChunksPerGroup;
            }
            return ( FinalizeCamera( r ) + kCullGroupThreads - 1u ) / kCullGroupThreads;
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
                    groups = RowGroups( row );
                s_Groups[gl_LocalInvocationIndex] = groups;
                barrier();
                uint first = base;
                for ( uint k = 0u; k < gl_LocalInvocationIndex; ++k )
                    first += s_Groups[k];
                uint total = 0u;
                for ( uint k = 0u; k < kCullGroupThreads; ++k )
                    total += s_Groups[k];
                if ( row < Rows.x )
                    CullDispatchData[kDispatchHeader + row][command] = first;
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
