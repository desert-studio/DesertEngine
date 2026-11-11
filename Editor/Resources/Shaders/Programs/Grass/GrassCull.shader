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
//     (CullDispatchData[0]). A row of a new law generation restarts its law's slots (Chunks' header y, w). The
//     met chunks' list is emptied (MetCount, its dispatch (0, 0, 1)).
//   1 THE CAMERA CULL (GRASS-CULL-HIER-1, GRASS-CULL-COST-3; UE GPU Scene / InstanceCulling / Nanite's cluster
//     cull and UE foliage culling: an instance's data is written once when it changes, the views only cull it - a
//     node per lane, then one group per node that survives over its elements): its chunk pass, kChunksPerGroup
//     chunks a group, a lane per chunk - a tile cluster's run of records (Common/GrassVarietyRows.glslh, the chunk
//     contract). Each chunk a view met is appended to the met chunks' list with the views it met, and its kept
//     records to the work list, whose length grows mode 5's indirect dispatch (GRASS-CULL-COST-4,
//     FPS-GRASS-CULL-GPU: mode 5 runs a lane per kept record of a met chunk, so no lane spends time on a rejected
//     chunk or a record its law dropped).
//     (a) THE LAW, once per law generation
//     (LawGeneration: the params with the field eye, the field, the pool - a still or turning camera keeps it): a
//     chunk a view may meet whose law word carries another generation is lawed by its subgroup
//     (LawChunk) - each kept record's row (GrassLawInstance, the law's one statement of it) into its slot of
//     Instances (FPS-ARCH-GRASSROWS: the 64-byte rows are written once per law generation), its bucket + 1 into
//     the chunk's code planes,
//     the box of the kept instances' reach into its law words. (b) The kept box meets the views: each cascade
//     list's caster box (none while the row's lists hold, kGrassRowCascadesHeld), the camera's side planes and the
//     previous frame's HZB.
//   5 THE CAMERA CULL'S INSTANCES (after mode 1, indirect by the work list): a lane per work item - a met
//     chunk's kept record (FPS-GRASS-CULL-GPU: compacted across chunks by mode 1, so every group runs
//     kCullGroupThreads live lanes whatever its chunks keep). (c) Each reads its law
//     row and is tested only by the views its chunk met; a list that takes it gets its 4-byte id (row +
//     InstancesBase), never a copy of the row: into each cascade list whose box met the chunk (CascadeCull), and -
//     the camera's planes met - by its own bounding sphere. Its bounding sphere - from its transform: the bounds centre along its up axis, the bounds radius at
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
        // the camera cull's lane per chunk and subgroup per lawed chunk (gl_SubgroupID / gl_SubgroupInvocationID /
        // gl_SubgroupSize), its kept boxes (subgroupMin / subgroupMax), the lawed chunk's words
        // (subgroupShuffle); the met chunks' appends (subgroupBallotExclusiveBitCount).
        #extension GL_KHR_shader_subgroup_basic : require
        #extension GL_KHR_shader_subgroup_ballot : require
        #extension GL_KHR_shader_subgroup_arithmetic : require
        #extension GL_KHR_shader_subgroup_shuffle : require

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

        // This frame's (a graph buffer, GrassRenderer.cpp kMetHeaderBytes): the met chunks' list and its records'
        // work list. MetCommand[0..2] is mode 5's VkDispatchIndirectCommand (a group per kCullGroupThreads work
        // items), MetCommand[3] the chunks appended, MetCommand[4] the work items appended (all grown by mode 1's
        // atomics from mode 0's (0, 0, 1, 0, 0)); then MetWords: Rows.y met chunks, two words each (x = its chunk of
        // the row, y = its row << kMetViewBits | the views it met: kCascadeViews | kCameraView | kChunkHidden), then
        // Rows.z work items, a word each (FPS-GRASS-CULL-GPU: its met chunk << kWorkRecordBits | its record of the
        // chunk). Sized to every culled row's chunk room and law capacity.
        Buffer(3) MetChunks
        {
            uint MetCommand[8];
            uint MetWords[];
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
            uvec4 Rows;              // x = the rows this pass covers (the table's first), y = MetChunks' room for met
                                     // chunks, z = its room for work items
        };

        const uint kModeDispatch        = 0u;
        const uint kModeCameraCull      = 1u;
        const uint kModeCameraFinalize  = 2u;
        const uint kModePostOcclusion   = 3u;
        const uint kModePostFinalize    = 4u;
        const uint kModeCameraInstances = 5u;

        const uint kCullGroupThreads = 64u;    // LocalSize
        // The camera cull's chunk pass: a lane per chunk (GRASS-CULL-COST-4: its records are mode 5's, a group per
        // met chunk - a chunk holds at most 64 records, two code words, a lane each).
        const uint kChunksPerGroup = kCullGroupThreads;
        // A met chunk's views in its second word below its row (kChunkHidden is bit 9).
        const uint kMetViewBits = 10u;
        // The views a chunk met (BoxMeetsViews): bits 0..7 the cascade lists (GrassRenderer kGrassCascadeLists), then
        // the camera's side planes, then - those met - the previous frame's HZB hid its whole box.
        const uint kCascadeViews = 0xFFu;
        const uint kCameraView   = 1u << 8u;
        const uint kChunkHidden  = 1u << 9u;
        // A work item's record of its chunk in its low bits (a chunk holds at most 64 records), its met chunk above.
        const uint kWorkRecordBits = 6u;
        // Feedback's word past the bucket counts that takes the law's slots handed out (GrassRenderer
        // kFeedbackLawWord: after the written mark, three drawn counts and eight cascade views').
        const uint kFeedbackLawWord = 12u;
        const uint kCullGroupsX    = 65535u; // Vulkan's guaranteed maxComputeWorkGroupCount[0]
        const uint kDispatchHeader = 2u;     // CullDispatchData's two commands before the rows
        // A chunk's reach is grown by this share: the tile cull and this pass compute the same largest axis scale,
        // up to their rounding (a few ulps), and the chunk must hold every instance's sphere.
        const float kChunkReachSlack = 1.0e-4;

        shared uint s_Groups[64];

        // MetChunks' two lists (Rows.y met chunks, then Rows.z work items).
        uint MetRoom()
        {
            return Rows.y;
        }
        uint WorkRoom()
        {
            return Rows.z;
        }
        void MetWrite( uint index, uvec2 entry )
        {
            MetWords[2u * index]      = entry.x;
            MetWords[2u * index + 1u] = entry.y;
        }
        uvec2 MetRead( uint index )
        {
            return uvec2( MetWords[2u * index], MetWords[2u * index + 1u] );
        }
        void WorkWrite( uint item, uint work )
        {
            MetWords[2u * MetRoom() + item] = work;
        }
        uint WorkRead( uint item )
        {
            return MetWords[2u * MetRoom() + item];
        }

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
        // calls it takes one entry of the counter at word @p at of @p counters, row @p r's. The subgroup's
        // invocations that name the same counter of the same row add their number with ONE atomicAdd and each takes
        // its rank among them. One round per distinct (row, counter) among the callers - mode 5's work list keeps a
        // chunk's records together and its chunks in the order mode 1 met them, so a subgroup spans few rows and
        // bucket boundaries, and its instances split into the visible and the hidden.
        // Returns the entry's index: the counter before the subgroup's add, plus the rank.
        uint WaveAppend( GrassWords counters, uint at, uint r )
        {
            uint  index = 0u;
            uvec2 key   = uvec2( at, r );
            for ( ;; )
            {
                if ( all( equal( subgroupBroadcastFirst( key ), key ) ) )
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
        // a variety with params @p p and bucket table in @p args - false when the law drops it, else its scale
        // @p outScale (GrassLawInstance makes its row of it) and its bucket @p outBucket. Everything view-dependent
        // is here.
        bool GrassViewLaw( GrassParams p, GrassWords args, TileRecord r, out vec3 outScale, out uint outBucket )
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
            // A blade's strip levels: every bucket but the last, its small one (GrassRenderer::BladeStrip's
            // BladeSmall, the coarsest level's submesh again).
            const uint bladeLevels = max( p.Buckets.x, 2u ) - 1u;
            if ( blade )
            {
                uint  ring = p.Blade.x > 0.0 ? min( uint( distance / p.Blade.x ), bladeLevels - 1u ) : 0u;
                float size = p.Lod.z > 0.0 ? p.Lod.z * scale.y / max( distance, 1.0 ) : 0.0;
                lod        = GrassBladeLodAt( size, ring, bladeLevels );
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

            uint bucket = 0u;
            if ( blade )
            {
                // THE SMALL BUCKET (FPS-GRASS-RASTER; UE r.MinScreenRadiusForDepthPrepass): the coarsest level's
                // instances from p.Lod.w out, where every blade stands below kMinScreenRadiusForDepthPrepass of the
                // screen, draw the same strip level as a bucket of their own, which the masked depth prepass leaves
                // to the G-buffer (GrassRenderer VarietyGpu::PrepassBuckets). The level and the share are the LOD's.
                bucket = lod.Level;
                if ( p.Lod.w > 0.0 && lod.Level + 1u == bladeLevels && distance >= p.Lod.w )
                    bucket = bladeLevels;
            }
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
            outScale  = scale;
            outBucket = bucket;
            return true;
        }

        // THE LAWED ROW (GRASS-CULL-COST-3; the one statement of it - LawChunk's kept box and the camera cull's rows):
        // record @p r's instance at the scale the view law gave it (@p scale, GrassViewLaw): LawChunk's row.
        // The world's snow (Common/WeatherSnow.glslh) lies @p snow deep over the ground: the root sinks by it, so the
        // instance shows only what stands above the snow and one shorter than it is buried whole. A blade's forward
        // axis carries the height multiplier (its lean is a share of its height) and the columns' w its three draws
        // (Vertex_GrassBlade.glslh); a mesh's are its scaled axes.
        mat4 GrassLawInstance( TileRecord r, vec3 scale, bool blade, float snow )
        {
            mat3 basis    = BasisFromQuat( r.Rotation );
            vec3 position = r.Position;
            position.y -= snow;
            vec3 draws = r.Draws;
            if ( blade )
                return mat4( vec4( basis[0] * scale.x, draws.x ), vec4( basis[1] * scale.y, draws.y ),
                             vec4( basis[2] * scale.y, draws.z ), vec4( position, 1.0 ) );
            return mat4( vec4( basis[0] * scale.x, 0.0 ), vec4( basis[1] * scale.y, 0.0 ),
                         vec4( basis[2] * scale.z, 0.0 ), vec4( position, 1.0 ) );
        }

        // An instance's largest axis scale: its transform's longest axis column.
        float InstanceScale( mat4 instance )
        {
            return max( length( instance[0].xyz ), max( length( instance[1].xyz ), length( instance[2].xyz ) ) );
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
        // (Bounds.z) - of the lists whose box met its chunk (bit k of @p met, BoxMeetsViews; none while the lists hold).
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

        // Mode 5, a lane's instance of a met chunk.
        // GRASS-CULL-UNIFY-1, THE CASCADES' LISTS (moved here from GrassGenerate's tile cull, which no longer sees any
        // view): the instance of id @p id (its law row of Instances + InstancesBase, FPS-ARCH-GRASSROWS) of row
        // @p r, in bucket @p b (its table word @p table), goes into each cascade list of @p holds (CascadeHolds).
        // MeshRenderer::BuildGpuInstancedShadowDraws' rule on the root @p root: a mover inside the
        // list's ring [inner, wind distance), else its rest pose (Common/ShadowWind.glslh, the one rule). A mover is
        // appended from the bucket's first slot, a rest caster from its last; a list's bucket never passes the room
        // the full list's bucket has, so the lists share its capacity.
        void CascadeCull( uint r, uint b, uint table, uint id, vec3 root, uint holds )
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
                uint index   = WaveAppend( cascade, counter, r );
                if ( index < room )
                {
                    uint offset = mover ? index : room - 1u - index;
                    ids.Words[cascade.Words[at + 1u] + offset] = id;
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
            uint idsBase = VarietyRows[r].CascadeIdsBase; // CPU-GRASS-POOL-5: absolute slots of the ids pool
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
                    cascade.Words[base + 4u + s * 5u + 4u]   = cascade.Words[at + 1u] + idsBase;
                    cascade.Words[base + args + s * 5u + 1u] = rest;
                    cascade.Words[base + args + s * 5u + 4u] = cascade.Words[at + 1u] + room - rest + idsBase;
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

        // Mode 5, one lane: row @p r's instance @p instance, the law's row @p row of Instances
        // (FPS-ARCH-GRASSROWS; UE GPU Scene / InstanceCulling: an instance's data is written once when it
        // changes, the views' lists hold ids), in bucket @p b, of a chunk that met the cascade lists @p met (bit k:
        // list k), the camera's side planes (@p seen) and - inside them - was hidden whole by the previous frame's
        // HZB (@p chunkHidden); @p held: the row's cascades and its full list hold from an earlier frame. A view
        // that keeps it appends the row's id (row + InstancesBase) to its list - the cull writes 4 bytes per list,
        // never the 64-byte row, which LawChunk wrote once this law generation. The cascades and the full list (the
        // union of the cascades
        // and the camera's planes - RT and the list-less cascades draw it, so grass the camera cannot see still
        // casts and reflects) take a slot of the full list's bucket; the camera's list and its candidates their
        // own. A held full list keeps the ids of the frame that culled it, as its cascades keep their depth: the
        // law generation is in their key (GrassRenderer CascadeKey), so the rows they name stand.
        void CullInstance( uint r, uint b, uint row, mat4 instance, uint met, bool seen, bool chunkHidden,
                           bool held )
        {
            uint       tables   = VarietyRows[r].Info.y;
            uint       buckets  = VarietyRows[r].Info.x;
            uint       table    = tables + b * 4u;
            GrassWords args     = GrassWords( VarietyRows[r].Args );
            uint       first    = args.Words[table + 1u];
            uint       capacity = args.Words[table + 2u];
            uint       id       = row + VarietyRows[r].InstancesBase;
            vec4       bounds   = VarietyRows[r].Bounds;
            float scale  = InstanceScale( instance );
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
            if ( !held )
            {
                uint full = WaveAppend( args, table, r );
                if ( full < capacity )
                {
                    GrassWords( VarietyRows[r].FullIds ).Words[first + full] = id;
                    CascadeCull( r, b, table, id, instance[3].xyz, holds );
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
            uint index = WaveAppend( GrassWords( VarietyRows[r].CameraArgs ), at, r );
            if ( index >= capacity )
                return;
            if ( hidden )
            {
                GrassQuads candidates = GrassQuads( VarietyRows[r].Candidates );
                candidates.Quads[2u * ( first + index )]      = floatBitsToUint( vec4( centre, radius ) );
                candidates.Quads[2u * ( first + index ) + 1u] = uvec4( row, 0u, 0u, 0u );
            }
            else
                GrassWords( VarietyRows[r].CameraIds ).Words[first + index] = id;
        }

        // A box (@p lo..@p hi) against the views (bit k < kGrassCascadeLists: cascade list k's caster box, none while
        // @p held; kCameraView: the camera's side planes).
        uint BoxMeetsViews( uint r, vec3 lo, vec3 hi, bool held )
        {
            vec3 centre = 0.5 * ( lo + hi );
            vec3 extent = 0.5 * ( hi - lo );
            uint views  = 0u;
            if ( !held )
            {
                uint lists = uint( bitCount( VarietyRows[r].Info.w ) );
                for ( uint k = 0u; k < lists; ++k )
                {
                    bool inside = true;
                    for ( uint i = 0u; i < 6u && inside; ++i )
                        inside = BoxMeetsPlane( VarietyCascadePlanes[k * 6u + i], centre, extent );
                    if ( inside )
                        views |= 1u << k;
                }
            }
            bool seen = true;
            for ( uint i = 0u; i < 4u && seen; ++i )
                seen = BoxMeetsPlane( VarietyCameraPlanes[i], centre, extent );
            if ( seen )
                views |= kCameraView;
            return views;
        }

        // Whether a view may take an instance of the chunk at word @p at of row @p r before the law ran on it: the
        // build's box of its roots, sunk by the snow and grown by the reach of the law's largest axis scale - ( |centre
        // height| + radius ) x GrassLawScaleBound + pad, which holds every instance's camera and cascade sphere -
        // against the views; none past the cull radius of the field eye.
        bool ChunkMayMeet( uint r, uint at, GrassParams p, bool held )
        {
            GrassWords chunks = GrassWords( VarietyRows[r].Chunks );
            vec3       lo     = uintBitsToFloat( uvec3( chunks.Words[at], chunks.Words[at + 1u], chunks.Words[at + 2u] ) );
            vec3       hi     = uintBitsToFloat( uvec3( chunks.Words[at + 3u], chunks.Words[at + 4u], chunks.Words[at + 5u] ) );
            const vec3 eye    = p.Camera.xyz;
            if ( length( clamp( eye, lo, hi ) - eye ) > p.Cull.y )
                return false;
            float farthest = length( max( abs( lo - eye ), abs( hi - eye ) ) );
            float scale    = GrassLawScaleBound( p, farthest, uintBitsToFloat( chunks.Words[at + kGrassChunkScaleWord] ) );
            vec4  bounds   = VarietyRows[r].Bounds;
            float reach    = ( ( abs( bounds.y ) + bounds.x ) * scale + bounds.z ) * ( 1.0 + kChunkReachSlack );
            lo.y -= p.Lean.z;
            hi.y -= p.Lean.z;
            return BoxMeetsViews( r, lo - vec3( reach ), hi + vec3( reach ), held ) != 0u;
        }

        // THE LAW, ONCE PER LAW GENERATION (GRASS-CULL-COST-3; UE GPU Scene: an instance's data is written once when
        // it changes, the views only cull it): the view law sees nothing of the view but the field eye
        // (GrassParams.Camera, the camera snapped to the tile grid), so what it makes of a record holds for its row's
        // law generation (LawGeneration). The chunk at word @p at of row @p r, @p count records, lawed by ONE whole
        // subgroup, a lane per record: the run past the cut at the box's nearest point is never kept (the run ascends
        // by fade draw and the share only falls with distance), the whole chunk past the cull radius. The run before
        // the cut gets its slots of Instances, one per record (Chunks' header y; none when they would pass
        // LawCapacity - the law then keeps nothing of the chunk this generation, and the renderer, fed the uncapped
        // count, grows the law's rows, which is a new generation). Every kept record's row (GrassLawInstance;
        // 64 B, FPS-ARCH-GRASSROWS: written here once per law generation, the views' lists only name it) goes
        // into its slot, its bucket + 1 into the code planes (@p outLow: records 0..31, @p outHigh: 32..63, plane
        // b at [b]) and the reach of its cascade sphere about its root - which holds its camera sphere - into the kept box (@p outLo .. @p outHi;
        // inverted when nothing is kept). They become the chunk's law words, stamped with the generation; @p outSlot:
        // the chunk's first slot.
        void LawChunk( uint r, uint at, uint count, GrassParams p, out vec3 outLo, out vec3 outHi, out uvec4 outLow,
                       out uvec4 outHigh, out uint outSlot )
        {
            GrassWords chunks = GrassWords( VarietyRows[r].Chunks );
            vec3       lo     = uintBitsToFloat( uvec3( chunks.Words[at], chunks.Words[at + 1u], chunks.Words[at + 2u] ) );
            vec3       hi     = uintBitsToFloat( uvec3( chunks.Words[at + 3u], chunks.Words[at + 4u], chunks.Words[at + 5u] ) );
            const vec3            eye     = p.Camera.xyz;
            const float           nearest = length( clamp( eye, lo, hi ) - eye );
            const GrassFieldShare head    = GrassFieldShareAt( nearest, p.Cull.x, p.Cull.y, p.Spread.w );
            // Every fade draw is >= 0: a cut of 0 drops the chunk past the cull radius whole.
            float cut = 0.0;
            if ( nearest <= p.Cull.y )
                cut = min( head.Kept, head.Floor );
            GrassSegments pool = GrassSegments( VarietyRows[r].TilePool );
            uint       first = chunks.Words[at + kGrassChunkFirstWord];
            uint       page  = chunks.Words[at + kGrassChunkPageWord];
            // The run before the cut: up to its last record whose fade draw passes the cut.
            uint run = 0u;
            for ( uint base = 0u; base < count; base += gl_SubgroupSize )
            {
                uint i      = base + gl_SubgroupInvocationID;
                bool passes = false;
                if ( i < count )
                    {
                        uint record = GrassChunkRecord( first, page, i );
                        passes      = unpackUnorm2x16( GrassPoolSegment( pool, record ).Quads[GrassSegmentRecord( record ) * kGrassTilePoolWords].w ).x < cut;
                    }
                uvec4 lanes = subgroupBallot( passes );
                if ( any( notEqual( lanes, uvec4( 0u ) ) ) )
                    run = base + subgroupBallotFindMSB( lanes ) + 1u;
            }
            uint slot = 0u;
            if ( run != 0u && subgroupElect() )
                slot = atomicAdd( chunks.Words[kGrassChunkSlotsUsedWord], run );
            slot = subgroupBroadcastFirst( slot );
            if ( run == 0u || slot + run > VarietyRows[r].LawCapacity )
                slot = kGrassLawNoSlot;

            g_Field               = GrassWords( VarietyRows[r].Field );
            GrassWords        args   = GrassWords( VarietyRows[r].Args );
            GrassInstanceRows rows   = GrassInstanceRows( VarietyRows[r].Instances );
            vec4              bounds = VarietyRows[r].Bounds;
            bool           blade  = ( floatBitsToUint( p.ScaleX.w ) & kFlagBlade ) != 0u;
            vec3           keptLo = vec3( 1.0e30 );
            vec3           keptHi = vec3( -1.0e30 );
            outLow                = uvec4( 0u );
            outHigh               = uvec4( 0u );
            uint lawed            = slot == kGrassLawNoSlot ? 0u : run;
            for ( uint base = 0u; base < lawed; base += gl_SubgroupSize )
            {
                uint i    = base + gl_SubgroupInvocationID;
                uint code = 0u;
                if ( i < lawed )
                {
                    uint  record = GrassChunkRecord( first, page, i );
                    GrassQuads quads  = GrassPoolSegment( pool, record );
                    uint       quad   = GrassSegmentRecord( record ) * kGrassTilePoolWords;
                    uvec4      head0  = quads.Quads[quad];
                    if ( unpackUnorm2x16( head0.w ).x < cut )
                    {
                        TileRecord tile = GrassUnpackRecord( head0, quads.Quads[quad + 1u], quads.Quads[quad + 2u] );
                        vec3 scale;
                        uint b;
                        if ( GrassViewLaw( p, args, tile, scale, b ) )
                        {
                            mat4  instance = GrassLawInstance( tile, scale, blade, p.Lean.z );
                            rows.Rows[slot + i] = instance;
                            float reach    = ( ( abs( bounds.y ) + bounds.x ) * InstanceScale( instance ) + bounds.z ) *
                                          ( 1.0 + kChunkReachSlack );
                            keptLo = min( keptLo, instance[3].xyz - vec3( reach ) );
                            keptHi = max( keptHi, instance[3].xyz + vec3( reach ) );
                            code   = b + 1u;
                        }
                    }
                }
                // Plane b's bits of records base..: the subgroup's ballot of its lanes' code bit b, placed at base.
                for ( uint plane = 0u; plane < 4u; ++plane )
                {
                    uvec4 lanes = subgroupBallot( ( ( code >> plane ) & 1u ) != 0u );
                    for ( uint j = 0u; j < 2u; ++j )
                    {
                        uint bit = base + 32u * j;
                        if ( bit < 32u )
                            outLow[plane] |= lanes[j] << bit;
                        else if ( bit < 64u )
                            outHigh[plane] |= lanes[j] << ( bit - 32u );
                    }
                }
            }
            outLo   = subgroupMin( keptLo );
            outHi   = subgroupMax( keptHi );
            outSlot = slot;
            if ( subgroupElect() )
            {
                for ( uint k = 0u; k < 3u; ++k )
                {
                    chunks.Words[at + kGrassChunkReachWord + k]      = floatBitsToUint( outLo[k] );
                    chunks.Words[at + kGrassChunkReachWord + 3u + k] = floatBitsToUint( outHi[k] );
                }
                for ( uint plane = 0u; plane < 4u; ++plane )
                {
                    chunks.Words[at + kGrassChunkCodeWord + 2u * plane]      = outLow[plane];
                    chunks.Words[at + kGrassChunkCodeWord + 2u * plane + 1u] = outHigh[plane];
                }
                chunks.Words[at + kGrassChunkSlotWord] = slot;
                chunks.Words[at + kGrassChunkLawWord]  = VarietyRows[r].LawGeneration;
            }
        }

        // The chunk at word @p at's law words as LawChunk wrote them.
        void ChunkLawWords( GrassWords chunks, uint at, out vec3 outLo, out vec3 outHi, out uvec4 outLow,
                            out uvec4 outHigh, out uint outSlot )
        {
            uint box   = at + kGrassChunkReachWord;
            uint codes = at + kGrassChunkCodeWord;
            outLo   = uintBitsToFloat( uvec3( chunks.Words[box], chunks.Words[box + 1u], chunks.Words[box + 2u] ) );
            outHi   = uintBitsToFloat( uvec3( chunks.Words[box + 3u], chunks.Words[box + 4u], chunks.Words[box + 5u] ) );
            outLow  = uvec4( chunks.Words[codes], chunks.Words[codes + 2u], chunks.Words[codes + 4u],
                             chunks.Words[codes + 6u] );
            outHigh = uvec4( chunks.Words[codes + 1u], chunks.Words[codes + 3u], chunks.Words[codes + 5u],
                             chunks.Words[codes + 7u] );
            outSlot = chunks.Words[at + kGrassChunkSlotWord];
        }

        // The bit index of the @p n-th set bit (from 0, lowest first) of @p word; @p n < bitCount( word ). A
        // binary search by the set bits of each window's lower half: five counts, no loop over the bits.
        uint NthSetBit( uint word, uint n )
        {
            uint pos = 0u;
            for ( uint width = 16u; width != 0u; width >>= 1u )
            {
                uint below = uint( bitCount( ( word >> pos ) & ( ( 1u << width ) - 1u ) ) );
                if ( n >= below )
                {
                    n -= below;
                    pos += width;
                }
            }
            return pos;
        }

        // The records of a chunk below record @p n (bit i of the two code words: record i), all 64 from n = 64 on.
        uvec2 RecordsBelow( uint n )
        {
            uvec2 mask = uvec2( 0xFFFFFFFFu );
            if ( n < 32u )
                mask = uvec2( ( 1u << n ) - 1u, 0u );
            else if ( n < 64u )
                mask.y = ( 1u << ( n - 32u ) ) - 1u;
            return mask;
        }

        // FPS-GRASS-CULL-GPU (UE InstanceCulling's instance work: the instances of the surviving nodes compacted into
        // one dense list before the instance pass, so its groups carry 64 live lanes whatever a node keeps): the
        // subgroup's met chunks' records (@p records, @p k of them, of met chunk @p index) are appended to the work
        // list with ONE atomic (MetCommand[4]); each chunk's run is then written by the whole subgroup, a lane per
        // record (ascending, so a chunk's records keep their order), one chunk after the other. The elected lane
        // grows mode 5's dispatch to the list's new end in groups of kCullGroupThreads (atomicMax: the last
        // append's end is the largest, whatever the order).
        void AppendChunkWork( uint index, uvec2 records, uint k )
        {
            uint total = subgroupAdd( k );
            if ( total == 0u )
                return;
            uint first = subgroupExclusiveAdd( k );
            uint base  = 0u;
            if ( subgroupElect() )
            {
                base        = atomicAdd( MetCommand[4], total );
                uint end    = min( base + total, WorkRoom() );
                uint groups = ( end + kCullGroupThreads - 1u ) / kCullGroupThreads;
                atomicMax( MetCommand[0], min( groups, kCullGroupsX ) );
                atomicMax( MetCommand[1], ( groups + kCullGroupsX - 1u ) / kCullGroupsX );
            }
            base         = subgroupBroadcastFirst( base );
            uvec4 todo   = subgroupBallot( k != 0u );
            while ( any( notEqual( todo, uvec4( 0u ) ) ) )
            {
                uint j = subgroupBallotFindLSB( todo );
                todo[j / 32u] &= ~( 1u << ( j % 32u ) );
                uint  chunk = subgroupShuffle( index, j );
                uint  count = subgroupShuffle( k, j );
                uint  start = base + subgroupShuffle( first, j );
                uvec2 mask  = subgroupShuffle( records, j );
                uint  low   = uint( bitCount( mask.x ) );
                for ( uint e = gl_SubgroupInvocationID; e < count; e += gl_SubgroupSize )
                {
                    if ( start + e >= WorkRoom() )
                        break;
                    uint i = e < low ? NthSetBit( mask.x, e ) : 32u + NthSetBit( mask.y, e - low );
                    WorkWrite( start + e, ( chunk << kWorkRecordBits ) | i );
                }
            }
        }

        // Mode 1 (GRASS-CULL-COST-3, GRASS-CULL-COST-4; UE foliage culling / InstanceCulling / Nanite's cluster
        // cull: the nodes' pass writes the list of the nodes that survive, then one group per surviving node culls
        // its elements): kChunksPerGroup chunks of one row per workgroup (CullDispatchData[0]), a lane each. (1)
        // The law, once per law generation: the chunks a view may meet that carry another generation are lawed by
        // their lanes' subgroup, one after the other (LawChunk); (2) each chunk's kept box meets the views and,
        // the camera's planes met, the previous frame's HZB; (3) a chunk a view met is appended to the met chunks'
        // list (MetChunks, one atomic per subgroup) with the views it met, and the list's dispatch grows to cover
        // it. A still or turning camera laws nothing (the law generation holds), and mode 5 runs no group for a
        // chunk no view met.
        void CameraCull()
        {
            uint group = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
            if ( group >= CullDispatchData[0].w )
                return;
            uint       r      = RowOfGroup( group, 0u );
            uint       c      = ( group - FirstGroup( r, 0u ) ) * kChunksPerGroup + gl_LocalInvocationIndex;
            GrassWords chunks = GrassWords( VarietyRows[r].Chunks );
            uint       at     = GrassChunkAt( c );
            bool       held   = ( VarietyRows[r].Info.z & kGrassRowCascadesHeld ) != 0u;
            uint       count  = 0u;
            bool       lawed  = false;
            if ( c < ChunkCount( r ) )
            {
                count = chunks.Words[at + kGrassChunkCountWord];
                lawed = count != 0u && chunks.Words[at + kGrassChunkLawWord] == VarietyRows[r].LawGeneration;
            }
            vec3  lo   = vec3( 0.0 );
            vec3  hi   = vec3( 0.0 );
            uvec4 low  = uvec4( 0u );
            uvec4 high = uvec4( 0u );
            uint  slot = kGrassLawNoSlot; // ChunkLawWords' out; mode 5 reads the slot back from the law words
            if ( lawed )
                ChunkLawWords( chunks, at, lo, hi, low, high, slot );

            // (1) Every lane of the subgroup takes part: the law's ballots and shuffles, and the append below.
            bool stale = count != 0u && !lawed;
            if ( any( notEqual( subgroupBallot( stale ), uvec4( 0u ) ) ) )
            {
                GrassParams p    = GrassParamsRef( VarietyRows[r].Params ).Data[0];
                uvec4       todo = subgroupBallot( stale && ChunkMayMeet( r, at, p, held ) );
                while ( any( notEqual( todo, uvec4( 0u ) ) ) )
                {
                    uint j = subgroupBallotFindLSB( todo );
                    todo[j / 32u] &= ~( 1u << ( j % 32u ) );
                    vec3  lawLo;
                    vec3  lawHi;
                    uvec4 lawLow;
                    uvec4 lawHigh;
                    uint  lawSlot;
                    LawChunk( r, subgroupShuffle( at, j ), subgroupShuffle( count, j ), p, lawLo, lawHi, lawLow,
                              lawHigh, lawSlot );
                    if ( gl_SubgroupInvocationID == j )
                    {
                        lo    = lawLo;
                        hi    = lawHi;
                        low   = lawLow;
                        high  = lawHigh;
                        slot  = lawSlot;
                        lawed = true;
                    }
                }
            }

            // (2) The chunk's kept box against the views.
            uvec2 kept  = uvec2( low.x | low.y | low.z | low.w, high.x | high.y | high.z | high.w );
            uint  views = 0u;
            if ( lawed && ( kept.x | kept.y ) != 0u )
            {
                views = BoxMeetsViews( r, lo, hi, held );
                if ( ( views & kCameraView ) != 0u && Hzb.z != 0u )
                {
                    GpuCullView view;
                    view.HzbExtent = uvec4( Hzb.xyz, 0u );
                    if ( GpuCull_IsOccludedByHzb( GrassHzbTexture, PointClampSampler, view, HzbViewProjection, lo, hi ) )
                        views |= kChunkHidden;
                }
            }

            // (3) The chunks a view met, appended with one atomic per subgroup (MetCommand[3]); a met chunk's kept
            // records that hold a law row (FPS-DEVLOST: slot + i < LawCapacity, mode 5's own guard - a record past
            // it is never read, so it takes no work item) are counted for the work list.
            bool  met     = views != 0u;
            uvec2 records = uvec2( 0u );
            if ( met && slot != kGrassLawNoSlot && slot < VarietyRows[r].LawCapacity )
                records = kept & RecordsBelow( VarietyRows[r].LawCapacity - slot );
            uint  k      = uint( bitCount( records.x ) + bitCount( records.y ) );
            uvec4 ballot = subgroupBallot( met );
            uint  n      = subgroupBallotBitCount( ballot );
            if ( n == 0u )
                return;
            uint base = 0u;
            if ( subgroupElect() )
                base = atomicAdd( MetCommand[3], n );
            uint index = subgroupBroadcastFirst( base ) + subgroupBallotExclusiveBitCount( ballot );
            if ( met && index < MetRoom() )
                MetWrite( index, uvec2( c, ( r << kMetViewBits ) | views ) );
            else
                k = 0u; // a chunk the list has no room for gives no work (mode 5 never reads past the list)
            AppendChunkWork( index, records, k );
        }

        // Mode 5 (GRASS-CULL-COST-4, FPS-GRASS-CULL, FPS-GRASS-CULL-GPU; UE InstanceCulling's instance pass,
        // indirect by the node pass's work list): a lane per work item - a kept record of a met chunk that holds a
        // law row, compacted by mode 1 (AppendChunkWork), so every group but the last runs kCullGroupThreads live
        // lanes whatever its chunks keep, and a group spans as many chunks (and rows) as its items name. A record
        // reads its row from its slot of Instances (FPS-ARCH-GRASSROWS: LawChunk wrote it once this law
        // generation; the pool records and params are not read here) and goes to CullInstance, tested only by the
        // views its chunk met, which appends its id to each list that takes it.
        void CameraInstances()
        {
            uint group = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
            uint item  = group * kCullGroupThreads + gl_LocalInvocationIndex;
            if ( item >= min( MetCommand[4], WorkRoom() ) )
                return;
            uint       work   = WorkRead( item );
            uvec2      entry  = MetRead( work >> kWorkRecordBits );
            uint       i      = work & ( ( 1u << kWorkRecordBits ) - 1u );
            uint       r      = entry.y >> kMetViewBits;
            uint       met    = entry.y & ( ( 1u << kMetViewBits ) - 1u );
            GrassWords chunks = GrassWords( VarietyRows[r].Chunks );
            uint       at     = GrassChunkAt( entry.x );
            vec3       lo;
            vec3       hi;
            uvec4      low;
            uvec4      high;
            uint       slot;
            ChunkLawWords( chunks, at, lo, hi, low, high, slot );
            uint  bit    = i % 32u;
            uvec4 planes = i < 32u ? low : high;
            uint  code   = ( ( planes.x >> bit ) & 1u ) | ( ( ( planes.y >> bit ) & 1u ) << 1u ) |
                           ( ( ( planes.z >> bit ) & 1u ) << 2u ) | ( ( ( planes.w >> bit ) & 1u ) << 3u );
            bool held = ( VarietyRows[r].Info.z & kGrassRowCascadesHeld ) != 0u;
            // The record's row as LawChunk wrote it this law generation (its slot: the chunk's first + i).
            uint row      = slot + i;
            // FPS-DEVLOST: a row the law did not hand out this generation (kGrassLawNoSlot, or past LawCapacity -
            // the rows were re-sized since the chunk's words were written) is never read: Instances holds
            // LawCapacity rows and the pool's next range lies past them.
            if ( slot == kGrassLawNoSlot || row >= VarietyRows[r].LawCapacity )
                return;
            mat4 instance = GrassLawRows( VarietyRows[r].Instances ).Rows[row];
            CullInstance( r, code - 1u, row, instance, met & kCascadeViews, ( met & kCameraView ) != 0u,
                          ( met & kChunkHidden ) != 0u, held );
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
            uint index = camera.Words[phase + 2u] + WaveAppend( camera, phase + 3u, r );
            if ( index < args.Words[table + 2u] )
                GrassWords( VarietyRows[r].CameraIds ).Words[first + index] =
                     candidates.Quads[2u * slot + 1u].x + VarietyRows[r].InstancesBase;
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
                args.Words[4u + s * 5u + 4u] = args.Words[at + 1u] + VarietyRows[r].FullIdsBase;
            }
            uint drawn = 0u;
            for ( uint b = 0u; b < buckets; ++b )
            {
                drawn += min( args.Words[table + b * 4u], args.Words[table + b * 4u + 2u] );
                feedback.Words[b] = args.Words[table + b * 4u];
            }
            feedback.Words[buckets]      = VarietyRows[r].FeedbackMark;
            feedback.Words[buckets + 1u] = drawn;
            args.Words[3]                = drawn;
        }

        // Mode 2, row @p r: phase 1's camera records and Feedback count, and the phase words frozen for phase 2;
        // returns the candidates frozen (phase 2's threads).
        // Mode 2, row @p r whose camera holds (kGrassRowCameraHeld; its cascades hold too): the camera's records,
        // drawn count and phase words stay as the frame that culled it froze them - its counters were restarted
        // then and no camera cull appended since - and phase 2 restarts its appends, so the held candidates are
        // retested against this frame's HZB. This frame in flight's Feedback copy gets the held counts; returns the
        // candidates frozen.
        uint HeldCamera( uint r )
        {
            GrassWords camera  = GrassWords( VarietyRows[r].CameraArgs );
            uint       buckets = camera.Words[0];
            uint       records = camera.Words[2];
            uint       phases  = 4u + records * 5u + buckets * 4u;
            HeldCascadeFeedback( r, buckets );
            uint candidates = 0u;
            for ( uint b = 0u; b < buckets; ++b )
            {
                candidates += camera.Words[phases + b * 4u + 1u];
                camera.Words[phases + b * 4u + 3u] = 0u;
            }
            GrassWords feedback = GrassWords( VarietyRows[r].Feedback );
            feedback.Words[buckets + 2u] = camera.Words[3];
            feedback.Words[buckets + 3u] = 0u;
            feedback.Words[buckets + kFeedbackLawWord] =
                 GrassWords( VarietyRows[r].Chunks ).Words[kGrassChunkSlotsUsedWord];
            return candidates;
        }

        uint FinalizeCamera( uint r )
        {
            FinalizeFull( r );
            if ( ( VarietyRows[r].Info.z & kGrassRowCameraHeld ) != 0u )
                return HeldCamera( r );
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
                camera.Words[4u + s * 5u + 4u] = camera.Words[at + 1u] + VarietyRows[r].CameraIdsBase;
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
            // The law's slots handed out this law generation, uncapped: the renderer sizes the rows by them.
            feedback.Words[buckets + kFeedbackLawWord] =
                 GrassWords( VarietyRows[r].Chunks ).Words[kGrassChunkSlotsUsedWord];
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
                post.Words[4u + s * 5u + 4u] = camera.Words[table + b * 4u + 1u] + camera.Words[phases + b * 4u + 2u] +
                                               VarietyRows[r].CameraIdsBase;
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
                // A new law generation hands the law's slots out anew (LawChunk; the chunk contract's header y, w).
                GrassWords chunks = GrassWords( VarietyRows[r].Chunks );
                if ( chunks.Words[kGrassChunkSlotsGenWord] != VarietyRows[r].LawGeneration )
                {
                    chunks.Words[kGrassChunkSlotsUsedWord] = 0u;
                    chunks.Words[kGrassChunkSlotsGenWord]  = VarietyRows[r].LawGeneration;
                }
                // A row whose camera holds (and so its cascades and full list) culls nothing this frame.
                if ( ( VarietyRows[r].Info.z & kGrassRowCameraHeld ) != 0u )
                    return 0u;
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
            if ( Hzb.w == kModeCameraInstances )
            {
                CameraInstances();
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
            {
                // The met chunks' list and the work list, empty: mode 1 appends to them and grows mode 5's dispatch.
                if ( gl_LocalInvocationIndex == 0u )
                {
                    MetCommand[0] = 0u;
                    MetCommand[1] = 0u;
                    MetCommand[2] = 1u;
                    MetCommand[3] = 0u;
                    MetCommand[4] = 0u;
                }
                LayOutRows( 0u );
            }
            else
                LayOutRows( 1u );
        }
    }
}
