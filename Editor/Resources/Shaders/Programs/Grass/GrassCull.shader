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
//     (CullDispatchData[0]). A row of a new law generation restarts its law's slots (Chunks' header y, w).
//   1 THE CAMERA CULL (GRASS-CULL-HIER-1, GRASS-CULL-COST-3; UE GPU Scene / InstanceCulling / Nanite's cluster
//     cull: an instance's data is written once when it changes, the views only cull it - a node per lane, then the
//     elements of the nodes that survive packed a lane each): kChunksPerGroup chunks a group, a lane per chunk - a
//     tile cluster's run of records (Common/GrassVarietyRows.glslh, the chunk contract), its kept records packed
//     over all the group's lanes (GRASS-CULL-COST-4). (a) THE LAW, once per law generation
//     (LawGeneration: the params with the field eye, the field, the pool - a still or turning camera keeps it): a
//     chunk a view may meet whose law word carries another generation is lawed by its subgroup
//     (LawChunk) - each kept record's scale into its slot of LawScales, its bucket + 1 into the chunk's code planes,
//     the box of the kept instances' reach into its law words. (b) The kept box meets the views: each cascade
//     list's caster box (none while the row's lists hold, kGrassRowCascadesHeld), the camera's side planes and the
//     previous frame's HZB. (c) The kept records of the chunks a view met are packed over the lanes; each builds its
//     row from its pool record and its slot's scale (GrassLawInstance, the law's one statement of a row) and is
//     tested only by the views its chunk met: into each cascade list whose box met the chunk (CascadeCull), and -
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
        // (subgroupShuffle); its packed records go through shared memory (s_Start..s_Run).
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
        // The camera cull's chunks per group (GRASS-CULL-COST-4): its first lanes own one each, and the kept records
        // of the ones a view met are packed over ALL its kCullGroupThreads lanes. A chunk holds at most 64 records
        // (two code words), so a group walks at most kChunksPerGroup * 64 / kCullGroupThreads = 16 rounds - the
        // near field's tail. At 64 chunks a group one 32-wide subgroup walked up to 2048 records, 64 dependent
        // rounds, and the pass waited on its nearest groups.
        const uint kChunksPerGroup = 16u;
        // The views a chunk met (BoxMeetsViews): bits 0..7 the cascade lists (GrassRenderer kGrassCascadeLists), then
        // the camera's side planes, then - those met - the previous frame's HZB hid its whole box.
        const uint kCascadeViews = 0xFFu;
        const uint kCameraView   = 1u << 8u;
        const uint kChunkHidden  = 1u << 9u;
        // Feedback's word past the bucket counts that takes the law's slots handed out (GrassRenderer
        // kFeedbackLawWord: after the written mark, three drawn counts and eight cascade views').
        const uint kFeedbackLawWord = 12u;
        const uint kCullGroupsX    = 65535u; // Vulkan's guaranteed maxComputeWorkGroupCount[0]
        const uint kDispatchHeader = 2u;     // CullDispatchData's two commands before the rows
        // A chunk's reach is grown by this share: the tile cull and this pass compute the same largest axis scale,
        // up to their rounding (a few ulps), and the chunk must hold every instance's sphere.
        const float kChunkReachSlack = 1.0e-4;

        shared uint s_Groups[64];
        // The camera cull's chunks of the group (CameraCull (2) -> (3)): each owner lane's chunk as the packing reads
        // it - the first packed record (s_Start[kChunksPerGroup]: the group's total), the kept mask, the code planes,
        // the pool run, the first law slot and the views met.
        shared uint  s_Start[kChunksPerGroup + 1u];
        shared uvec2 s_Kept[kChunksPerGroup];
        shared uvec4 s_Low[kChunksPerGroup];
        shared uvec4 s_High[kChunksPerGroup];
        shared uvec4 s_Run[kChunksPerGroup]; // x first, y page, z slot, w views

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
            outScale  = scale;
            outBucket = bucket;
            return true;
        }

        // THE LAWED ROW (GRASS-CULL-COST-3; the one statement of it - LawChunk's kept box and the camera cull's rows):
        // record @p r's instance at the scale the view law gave it (@p scale, GrassViewLaw / its slot of LawScales).
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
                    ids.Words[cascade.Words[at + 1u] + offset] = slot + VarietyRows[r].InstancesBase;
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

        const uint kNoGrassRow = 0xFFFFFFFFu;

        // Mode 1, one lane: row @p r's instance @p instance (its row, GrassLawInstance) in bucket @p b, of a
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
            uint row = kNoGrassRow;
            if ( !held )
            {
                uint full = WaveAppend( args, table );
                if ( full < capacity )
                {
                    row = first + full;
                    GrassInstanceRows( VarietyRows[r].Instances ).Rows[row] = instance;
                    GrassWords( VarietyRows[r].FullIds ).Words[row] = row + VarietyRows[r].InstancesBase;
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
                GrassWords( VarietyRows[r].CameraIds ).Words[first + index] = row + VarietyRows[r].InstancesBase;
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
        // the cut gets its slots of LawScales, one per record (Chunks' header y; none when they would pass
        // LawCapacity - the law then keeps nothing of the chunk this generation, and the renderer, fed the uncapped
        // count, grows LawScales, which is a new generation). Every kept record's scale goes into its slot, its bucket
        // + 1 into the code planes (@p outLow: records 0..31, @p outHigh: 32..63, plane b at [b]) and the reach of its
        // cascade sphere about its root - which holds its camera sphere - into the kept box (@p outLo .. @p outHi;
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
            GrassWords     args   = GrassWords( VarietyRows[r].Args );
            GrassLawScales scales = GrassLawScales( VarietyRows[r].LawScales );
            vec4           bounds = VarietyRows[r].Bounds;
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
                            uint word                = 3u * ( slot + i );
                            scales.Scales[word]      = scale.x;
                            scales.Scales[word + 1u] = scale.y;
                            scales.Scales[word + 2u] = scale.z;
                            mat4  instance = GrassLawInstance( tile, scale, blade, p.Lean.z );
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

        // The index of the @p k-th set bit of @p word (k < bitCount( word )), by halves.
        uint NthSetBit( uint word, uint k )
        {
            uint at = 0u;
            for ( uint width = 16u; width > 0u; width /= 2u )
            {
                uint below = uint( bitCount( ( word >> at ) & ( ( 1u << width ) - 1u ) ) );
                if ( k >= below )
                {
                    k -= below;
                    at += width;
                }
            }
            return at;
        }

        // Mode 1 (GRASS-CULL-COST-3; UE GPU Scene / InstanceCulling / Nanite's cluster cull: a node per lane, then the
        // elements of the nodes that survive packed a lane each): kChunksPerGroup chunks of one row per workgroup
        // (CullDispatchData[0]), owned by its first lanes, one each. (1) The law, once per law generation: the chunks
        // a view may meet that carry another generation are lawed by their owners' subgroup, one after the other
        // (LawChunk); (2) each owner's chunk's kept box meets the views and, the camera's planes met, the previous
        // frame's HZB; (3) the kept records of the chunks a view met are packed over ALL the group's lanes by a
        // prefix sum of their counts in shared memory (GRASS-CULL-COST-4: a group walks at most 16 rounds, not one
        // subgroup up to 64 - the near field's tail the pass waited on) - a lane per record, its row built from its
        // pool record and its slot's scale (GrassLawInstance), then
        // CullInstance, which writes it to the row of the list that takes it. A still or turning camera laws nothing
        // (the law generation holds), and no lane works on a record its chunk's views cannot take.
        void CameraCull()
        {
            uint group = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
            if ( group >= CullDispatchData[0].w )
                return;
            uint       r      = RowOfGroup( group, 0u );
            uint       lane   = gl_LocalInvocationIndex;
            bool       owner  = lane < kChunksPerGroup;
            uint       c      = ( group - FirstGroup( r, 0u ) ) * kChunksPerGroup + lane;
            GrassWords chunks = GrassWords( VarietyRows[r].Chunks );
            uint       at     = GrassChunkAt( c );
            bool       held   = ( VarietyRows[r].Info.z & kGrassRowCascadesHeld ) != 0u;
            uint       count  = 0u;
            bool       lawed  = false;
            if ( owner && c < ChunkCount( r ) )
            {
                count = chunks.Words[at + kGrassChunkCountWord];
                lawed = count != 0u && chunks.Words[at + kGrassChunkLawWord] == VarietyRows[r].LawGeneration;
            }
            vec3  lo   = vec3( 0.0 );
            vec3  hi   = vec3( 0.0 );
            uvec4 low  = uvec4( 0u );
            uvec4 high = uvec4( 0u );
            uint  slot = kGrassLawNoSlot;
            if ( lawed )
                ChunkLawWords( chunks, at, lo, hi, low, high, slot );

            // (1) Every lane of the subgroup takes part: the law's ballots, the prefix sums and the shuffles below.
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
                // The slots LawChunk's lanes wrote, before other lanes read them back (3).
                subgroupMemoryBarrierBuffer();
                subgroupBarrier();
            }

            // (2) The chunk's kept box against the views.
            uvec2 kept  = uvec2( low.x | low.y | low.z | low.w, high.x | high.y | high.z | high.w );
            uint  views = 0u;
            uint  first = 0u;
            uint  page  = 0u;
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
                first = chunks.Words[at + kGrassChunkFirstWord];
                page  = chunks.Words[at + kGrassChunkPageWord];
            }
            uint taken = 0u;
            if ( views != 0u )
                taken = uint( bitCount( kept.x ) + bitCount( kept.y ) );
            if ( owner )
            {
                s_Start[lane] = taken;
                s_Kept[lane]  = kept;
                s_Low[lane]   = low;
                s_High[lane]  = high;
                s_Run[lane]   = uvec4( first, page, slot, views );
            }
            // The slots the law wrote and the chunks' words, before the other subgroups' lanes read them (3).
            memoryBarrierBuffer();
            memoryBarrierShared();
            barrier();
            if ( lane == 0u )
            {
                uint sum = 0u;
                for ( uint j = 0u; j < kChunksPerGroup; ++j )
                {
                    uint n     = s_Start[j];
                    s_Start[j] = sum;
                    sum += n;
                }
                s_Start[kChunksPerGroup] = sum;
            }
            memoryBarrierShared();
            barrier();

            // (3) The kept records of the chunks a view met, packed over the group's lanes, a lane each.
            uint           total  = s_Start[kChunksPerGroup];
            GrassSegments  pool   = GrassSegments( VarietyRows[r].TilePool );
            GrassLawScales scales = GrassLawScales( VarietyRows[r].LawScales );
            float          snow   = 0.0;
            bool           blade  = false;
            if ( total != 0u )
            {
                GrassParams p = GrassParamsRef( VarietyRows[r].Params ).Data[0];
                snow          = p.Lean.z;
                blade         = ( floatBitsToUint( p.ScaleX.w ) & kFlagBlade ) != 0u;
            }
            for ( uint base = 0u; base < total; base += kCullGroupThreads )
            {
                uint t = base + lane;
                if ( t >= total )
                    continue;
                // The chunk that holds packed record t: the last whose start is at most t (one that took nothing
                // shares its successor's start and is passed over; the last one's start is then the total).
                uint from = 0u;
                for ( uint j = 1u; j < kChunksPerGroup; ++j )
                {
                    if ( s_Start[j] <= t )
                        from = j;
                }
                uint  k          = t - s_Start[from];
                uvec2 mask       = s_Kept[from];
                uvec4 lowPlanes  = s_Low[from];
                uvec4 highPlanes = s_High[from];
                uvec4 run        = s_Run[from];
                uint  runFirst   = run.x;
                uint  runPage    = run.y;
                uint  runSlot    = run.z;
                uint  met        = run.w;
                uint  below      = uint( bitCount( mask.x ) );
                uint  i     = 0u;
                uvec4 planes;
                if ( k < below )
                {
                    i      = NthSetBit( mask.x, k );
                    planes = lowPlanes;
                }
                else
                {
                    i      = 32u + NthSetBit( mask.y, k - below );
                    planes = highPlanes;
                }
                uint bit    = i % 32u;
                uint code   = ( ( planes.x >> bit ) & 1u ) | ( ( ( planes.y >> bit ) & 1u ) << 1u ) |
                              ( ( ( planes.z >> bit ) & 1u ) << 2u ) | ( ( ( planes.w >> bit ) & 1u ) << 3u );
                uint record = GrassChunkRecord( runFirst, runPage, i );
                uint word   = 3u * ( runSlot + i );
                // The pool record's root, rotation and draws (its scale and clump are the law's inputs, which the slot's
                // scale replaces).
                GrassQuads quads    = GrassPoolSegment( pool, record );
                uint       quad     = GrassSegmentRecord( record ) * kGrassTilePoolWords;
                TileRecord tile     = GrassUnpackRecord( quads.Quads[quad], uvec4( 0u ), quads.Quads[quad + 2u] );
                vec3       scale    = vec3( scales.Scales[word], scales.Scales[word + 1u], scales.Scales[word + 2u] );
                mat4       instance = GrassLawInstance( tile, scale, blade, snow );
                CullInstance( r, code - 1u, instance, met & kCascadeViews, ( met & kCameraView ) != 0u,
                              ( met & kChunkHidden ) != 0u, held );
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
            // The law's slots handed out this law generation, uncapped: the renderer sizes LawScales from them.
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
