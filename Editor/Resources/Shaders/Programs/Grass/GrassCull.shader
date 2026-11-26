// DesertAsset {"Kind":"Shader","Guid":"5e0c7a3f2b9d4c61a8f4e2b7d9c1a306","Versions":{"SHDR":1},"Dependencies":[]}
// THE GRASS CAMERA CULL (GRASS-VIS-24, GRASS-CULL-1, GRASS-CULL-2; UE InstanceCulling: the instances of every
// primitive culled by ONE dispatch over one table, sized indirect by the GPU's own counts, in a program of its own
// apart from the builder's). Every pass covers the first Rows.x rows of the variety table
// (Common/GrassVarietyRows.glslh: the frame's camera-culled varieties) with one dispatch. A workgroup of 64 threads
// works inside ONE row - its variety, found by a binary search of the rows' first groups (CullDispatch) - so every
// address it reads is uniform across it and a subgroup's appends name one variety's counters. Modes (Hzb.w,
// GrassRenderer.cpp kCullMode*):
//   0 THE DISPATCH (FPS-GRASS-CULL-COST: no pass - the renderer's upload at the graph's head, UE InstanceCulling's
//     CPU-built node dispatch): per row the tile pass's groups - kTileSlotsPerGroup of its tile slots a group (its
//     chunk room / kGrassTileChunks) - laid end to end in row order as the rows' first groups, and their sum as
//     mode 6's indirect dispatch (CullDispatchData[0], one group at least); phase 2's command zeroed; the met
//     chunks' list and the tiles' list emptied (their counts, their dispatches). Mode 6's first group restarts
//     the rows (RestartRows): a row of a new law generation its law's slots (Chunks' header y, w), a row whose
//     cascades are culled its full list's counts.
//   6 THE CAMERA CULL'S TILES (FPS-GRASS-CULL-GPU2; UE InstanceCulling / Nanite's cull hierarchy: a node's
//     conservative bound rejects its children whole): a lane per tile slot, its node (the union of its chunks'
//     root boxes at their largest scale, Common/GrassVarietyRows.glslh) against the views by ChunkMayMeet's own
//     bound (TileMayMeet); a slot that may meet one is appended to CullTiles, whose length grows mode 1's
//     dispatch.
//   1 THE CAMERA CULL (GRASS-CULL-HIER-1, GRASS-CULL-COST-3; UE GPU Scene / InstanceCulling / Nanite's cluster
//     cull and UE foliage culling: an instance's data is written once when it changes, the views only cull it - a
//     node per lane, then one group per node that survives over its elements): its chunk pass over mode 6's tiles,
//     FPS-GRASS-CULL: a workgroup per chunk - a tile cluster's run of records (Common/GrassVarietyRows.glslh, the
//     chunk contract), kGrassTileChunks groups a tile - and a lane per record: the chunk's words are uniform
//     across the group, its records culled by their own lanes, so no met chunks' list, no work list and no
//     serial loop over records (FPS-GRASS-CULL-2's subgroup carried 32 chunks' records round after round).
//     (a) THE LAW, once per law generation
//     (LawGeneration: the params with the field eye, the field, the pool - a still or turning camera keeps it): a
//     chunk a view may meet whose law word carries another generation is lawed by its workgroup
//     (LawChunk) - each kept record's row (GrassLawInstance, the law's one statement of it) into its slot of
//     Instances (FPS-ARCH-GRASSROWS: the 64-byte rows are written once per law generation), its bucket + 1 into
//     the chunk's code planes,
//     the box of the kept instances' reach into its law words. A mesh record whose level differs at the row's two
//     LOD eyes (LodEyes, the view's temporal LOD samples) gets a fade twin, its older level's row (LOD-DITHER-
//     GRASS-SIDE; UE DitheredLODTransition), which the instance cull culls with it. (b) The kept box meets the views: each cascade
//     list's caster box (none while the row's lists hold, kGrassRowCascadesHeld), the camera's side planes and the
//     previous frame's HZB; the views whose volume holds the box whole are marked (kInsideViews: their planes
//     are not tested again per instance, an instance's spheres lying inside its chunk's kept box).
//     (c) THE INSTANCES (FPS-GRASS-CULL-2: was mode 5, a pass of its own over a work list): each kept record of a
//     met chunk reads its law row and is tested only by the views its chunk met and does not lie inside whole; a
//     list that takes it gets its 4-byte id (row +
//     InstancesBase), never a copy of the row: into each cascade list whose box met the chunk (CascadeCull), and -
//     the camera's planes met - by its own bounding sphere. Its bounding sphere - from its transform: the bounds centre along its up axis, the bounds radius at
//     its largest axis scale plus a mesh's sway - inside the camera's side planes enters the camera's list in the
//     same bucket, or, hidden by the previous frame's furthest HZB (Hzb.z != 0; Common/GpuCullTests.glslh
//     GpuCull_IsOccludedByHzb, the instance cull's test, through that frame's ViewProjection), becomes phase 2's
//     candidate (its sphere and its row of Instances). Hzb.z = 0: the camera's planes alone. A chunk the previous
//     HZB hides whole makes its instances candidates without their own test (an instance's box lies inside its
//     chunk's).
//   2 THE CAMERA CULL'S FINALIZE (FPS-GRASS-CULL-COST: run by mode 1's LAST group, not a pass of its own - the
//     last-group-done pattern of AMD SPD / UE's single-pass reductions: every mode 1 group counts itself done on
//     CullDispatchData[1].w after its writes, and the group that brings the count to the dispatch's size, so the last
//     to finish, finalizes; one barrier and one launch less on the async chain): per row the camera's records get phase
//     1's counts (each bucket's min(count, capacity)) and first instances, each bucket's phase words freeze this
//     frame's candidates and phase 1's drawn count and restart the counters, Feedback gets phase 1's drawn count; the
//     frozen candidates' groups are laid out as phase 2's first groups and dispatch (CullDispatchData[1]).
//   3 PHASE 2 (UE two-phase occlusion): one thread per frozen candidate, after this frame's furthest HZB of the
//     phase-1 depth; a candidate that HZB no longer hides is appended to the camera's list after phase 1's drawn
//     ones. Hzb.z = 0 (no HZB this frame): every candidate is appended, so a missing HZB never drops grass.
//   4 THE POST FINALIZE (one group): per row PostArgs' records - phase 2's appended instances, within what phase 1
//     left of each bucket's capacity, right after phase 1's - and Feedback's phase-2 count.
//   7 THE HELD SETUP (FPS-GRASS-SETUP-FUSE): one thread per phase-1 drawn slot of every row whose camera list holds
//     (kGrassRowCameraHeld: no mode 1 thread appended it this frame, but the wind moved) - its setup record
//     (SetUpSlot). The finalize lays its groups out (CullDispatchData[2], the rows' z); none while no row holds.
// Both lists are appended to with one atomic per subgroup and counter (WaveAppend).
// THE BLADE SETUP FUSED INTO THE CULL (FPS-GRASS-SETUP-FUSE; Ghost of Tsushima, GDC 2021 "Procedural Grass in Ghost
// of Tsushima": the compute that appends a visible blade writes its render data in the same thread): the thread that
// appends an instance to the camera's list - mode 1's CullInstance, phase 2's PostOcclusion, mode 7 for a held list
// - writes that slot's GrassBladeSetupData (Mesh/Surface/GrassBlade.glslh) at the raster-stream row
// Visibility.x + slot of the view's setup records (CullSetupRecords) from the transform it already holds, the
// row's shape and view wind (CullSetupRows) and the view's camera block and deformation field. No pass of its own,
// no prefix of drawn counts, no search of a blade's record: the slot IS the position the append returned. A blade
// variety's record is its curve now and one wind step before, its strip width, card and level; a mesh variety's
// (FPS-GRASS-MESH-SETUP) its wind sway now and before and its LOD level.
Shader "GrassCull"
{
    Compute
    {
        // WaveAppend: one atomic per subgroup and counter (subgroupBallot / subgroupElect / subgroupBroadcastFirst);
        // the tile pass's append the same. FPS-GRASS-CULL: the camera cull's law reduces across its workgroup in
        // shared memory, so no subgroup arithmetic or shuffle is left.
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
        // FPS-GRASS-SETUP-FUSE: the camera block (Common/CameraUB.glslh, binding 0: ShaderProtocols::MakeCameraUB of
        // the view's frame - the gust and the local wind sources, now and one step before, the very block the
        // raster's vertex stage reads), the blade curve and the deformation field's trample.
        #include <Common/LocalWind.glslh>
        #include <Common/FoliageWind.glslh>
        #include <Mesh/Surface/GrassBlade.glslh>
        #include <Deformation/DeformationFieldTrample.glslh>

        // GrassRenderer.cpp GrassVarietyTableGpu: the camera's side planes, the cascades' caster boxes and the shadow
        // wind rule's origin, then the rows (this pass's first).
        ReadBuffer(4) Varieties
        {
            vec4            VarietyCameraPlanes[4]; // left / right / bottom / top, xyz.n + w >= 0 inside
            // GRASS-CULL-UNIFY-1: list k's caster box at 6k..6k+5 (the six planes of the matrix the k-th live cascade
            // view draws with, MeshRenderer::CascadeCasterBoxSet; GrassRenderer kGrassCascadeLists = 8 lists).
            vec4            VarietyCascadePlanes[48];
            vec4            VarietyCasterWindOrigin; // xyz = MeshRenderer::CasterWindOrigin (the main camera)
            // DEFORM-GRASS-3: where the deformation field holds a press (GrassRenderer::SetTrampleWindow,
            // GRASS-RASTER-COST: DeformationFieldRenderer::TrampledBox): xy world X, Z min, zw max (cm); min > max =
            // nothing pressed.
            vec4            VarietyTrampleWindow;
            GrassVarietyRow VarietyRows[];
        };

        // This frame's (a graph buffer): [0] the camera cull's VkDispatchIndirectCommand (w: its groups), [1] phase
        // 2's, [2] the held setup's (mode 7), then per row x = its first group of the camera cull, y = of phase 2,
        // z = of the held setup (GrassRenderer.cpp kCullDispatchHeader / kPostDispatchOffset /
        // kHeldSetupDispatchOffset).
        Buffer(1) CullDispatch
        {
            uvec4 CullDispatchData[];
        };

        // This frame's (a graph buffer, GrassRenderer.cpp kTileHeaderBytes; FPS-GRASS-CULL-GPU2): the tiles mode 6
        // let through. TileCommand[0..2] is mode 1's VkDispatchIndirectCommand (kChunkGroupsPerTile groups a tile),
        // TileCommand[3] the tiles appended (both grown by mode 6's atomics from the upload's (1, 1, 1, 0): one group
        // at least, whose last finalizes - FPS-GRASS-CULL-2); then Rows.y entries, x = the tile's row, y = its slot.
        Buffer(3) CullTiles
        {
            uint  TileCommand[4];
            uvec2 TileList[];
        };

        Uniform sampler PointClampSampler;
        // The camera cull: the previous frame's furthest HZB of the view; phase 2: this frame's (neutral: none).
        Uniform(2) texture2D GrassHzbTexture;

        // FPS-GRASS-SETUP-FUSE: per row of this pass (the variety table's row r) what its camera list's setup
        // records need - mirrors Graphic::System::GrassBladeSetupRowGpu (GrassRenderer.cpp), std430: a blade
        // variety's shape (the GrassBladeShapeData its cells read, without the hull; a mesh variety's setup section's
        // rows in Visibility / BucketFirst), its view wind (Graphic::PackViewInstanceWind, the raster's draw row's
        // WindA / WindB) and Kind.x (kCullSetupNone: nothing reads this row's records this frame).
        struct GrassCullSetupRow
        {
            GrassBladeShapeData Shape;
            vec4                WindA;
            vec4                WindB;
            uvec4               Kind;
        };
        ReadBuffer(5) CullSetupRows
        {
            GrassCullSetupRow CullSetup[];
        };
        // The view's setup records (GrassRenderer m_BladeSetup), one per raster-stream row.
        Buffer(6) CullSetupRecords
        {
            GrassBladeSetupData CullSetupRecord[];
        };
        // The view's deformation field (FrameGraphRefs.hpp DeformationFieldReads; System.Black and zero levels where
        // the view has none: every read answers "untouched"), point-sampled as the raster reads it.
        Uniform sampler DeformationFieldSampler;
        Uniform(7) texture2D DeformHeight0;
        Uniform(8) texture2D DeformHeight1;
        Uniform(9) texture2D DeformDetail0;
        Uniform(10) texture2D DeformDetail1;
        ReadBuffer(11) DeformationParams
        {
            ivec4 FieldWindows; // Deformation::DeformationFieldParamsGpu: xy level 0's minimum cell, zw level 1's
            vec4  FieldLevels;  // x, y: texel (cm) of level 0, 1; z, w: their resolution (0: no field)
        };

        // Mirrors Graphic::System::GrassCullPush (GrassRenderer.cpp).
        PushConstant PushConstants
        {
            mat4  HzbViewProjection; // unjittered, what GrassHzbTexture was built through
            uvec4 Hzb;               // xy = GrassHzbTexture's mip-0 size, z = its mip count (0: nothing tested),
                                     // w = the mode
            uvec4 Rows;              // x = the rows this pass covers (the table's first), y = CullTiles' room for
                                     // tiles (mode 6 and 1; 0 elsewhere), zw = 0 (the block's uvec4 tail)
        };

        // 0: the dispatch - no pass of its own (FPS-GRASS-CULL-COST): the renderer uploads the tiles' header and the
        // tile pass's layout, mode 6's first group restarts the rows (RestartRows).
        const uint kModeCameraCull    = 1u;
        // 2: the camera cull's finalize - no mode of its own: mode 1's last group runs it (CameraCullGroup).
        const uint kModePostOcclusion = 3u;
        const uint kModePostFinalize  = 4u;
        // 5: the camera cull's instances - no mode of its own (FPS-GRASS-CULL-2): mode 1's lanes cull their chunk's
        // records (CameraCull).
        const uint kModeCameraTiles   = 6u;
        const uint kModeHeldSetup     = 7u;
        // GrassCullSetupRow Kind.x (GrassRenderer.cpp kCullSetup*).
        const uint kCullSetupNone  = 0u;
        const uint kCullSetupBlade = 1u;
        const uint kCullSetupMesh  = 2u;

        const uint kCullGroupThreads = 64u;    // LocalSize
        // The camera cull's chunk pass (FPS-GRASS-CULL): a workgroup per chunk of the tiles mode 6 let through,
        // kGrassTileChunks groups a tile, a lane per record - a chunk holds at most kGrassTilePageRecords records,
        // which must not pass the group's lanes (CameraCull, LawChunk).
        const uint kChunkGroupsPerTile = kGrassTileChunks;
        // The tile pass (mode 6, FPS-GRASS-CULL-GPU2): a lane per tile slot of a row, kCullGroupThreads a group.
        const uint kTileSlotsPerGroup = kCullGroupThreads;
        // A tile node's test grows its bound past its chunks' by this pad (cm) and share of the law's scale bound:
        // the node's box holds each chunk's and its farthest is at least each chunk's, so the bound is the chunks'
        // in exact arithmetic; the pad and share cover the rounding of a different box (a few ulps) and the
        // approximate inversesqrt of GrassFieldShareAt's Widen, so a tile never fails a test one of its chunks
        // passes.
        const float kTileNodePad   = 1.0;
        const float kTileNodeSlack = 1.0e-3;
        // The views a chunk met (BoxMeetsViews): bits 0..7 the cascade lists (GrassRenderer kGrassCascadeLists), then
        // the camera's side planes, then - those met - the previous frame's HZB hid its whole box.
        const uint kCascadeViews = 0xFFu;
        const uint kCameraView   = 1u << 8u;
        const uint kChunkHidden  = 1u << 9u;
        // FPS-GRASS-CULL-2 (UE InstanceCulling's "fully inside" node flag: a node inside a view's volume whole skips
        // its instances' plane tests): of the views a chunk met, those whose volume holds its kept box whole
        // (BoxInsideViews) - bits 10..17 the cascade lists' caster boxes, bit 18 the camera's side planes. Every
        // instance's spheres lie inside its chunk's kept box (LawChunk's reach), so each would pass that view's
        // planes: CullInstance does not test them.
        const uint kInsideShift   = 10u;
        const uint kCameraInside  = 1u << 18u;
        // Feedback's word past the bucket counts that takes the law's slots handed out (GrassRenderer
        // kFeedbackLawWord: after the written mark, three drawn counts and eight cascade views').
        const uint kFeedbackLawWord = 12u;
        // FPS-GRASS-VIS, THE RASTER'S CENSUS (UE's GPU-culling stats per LOD): Feedback's words past the law word, one
        // per bucket - the instances the camera's phase-1 list drew of that bucket (GrassRenderer
        // kFeedbackCameraWord; a camera without a list of its own: the full list's), so the renderer prices the
        // raster in triangles by the bucket's strip level or LOD.
        const uint kFeedbackCameraWord = 13u;
        const uint kCullGroupsX    = 65535u; // Vulkan's guaranteed maxComputeWorkGroupCount[0]
        const uint kDispatchHeader = 3u;     // CullDispatchData's three commands before the rows
        // A chunk's reach is grown by this share: the tile cull and this pass compute the same largest axis scale,
        // up to their rounding (a few ulps), and the chunk must hold every instance's sphere.
        const float kChunkReachSlack = 1.0e-4;

        // LOD-DITHER-GRASS-SIDE: a fade twin's slot rides in its record's row as a float (column 1 w), exact below
        // 2^24.
        const uint kGrassFadeTwinRows = 1u << 24u;

        shared uint s_Groups[64];
        shared uint s_HeldGroups[64]; // FPS-GRASS-SETUP-FUSE: the rows' held setup groups (LayOutRows)
        // FPS-GRASS-CULL-COST: whether this mode 1 group is the dispatch's last to finish (CameraCullGroup).
        shared bool s_LastGroup;
        // FPS-GRASS-CULL, LawChunk's reductions across the workgroup's subgroups: the run's end, its first slot, the
        // kept box (FloatOrder: min xyz, max xyz), the code planes (plane b's records 0..31 at 2b, 32..63 at 2b + 1),
        // the fade twins counted and their first slot.
        shared uint s_LawRun;
        shared uint s_LawSlot;
        shared uint s_LawBox[6];
        shared uint s_LawPlanes[8];
        shared uint s_LawTwins;
        shared uint s_LawTwinFirst;

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

        // FPS-GRASS-SETUP-FUSE (was GrassBladeSetup.shader SetUpMeshInstance; FPS-GRASS-MESH-SETUP, UE 5 Nanite's
        // instance scene data, computed once per instance and only fetched by the material pass): mesh row @p r's
        // camera slot @p slot, the law row @p row (@p origin its root): ONE record a variety (FPS-GRASS-BLADE-SETUP:
        // every section reads the same one - GrassRenderer.cpp MeshSetupSection), at row Visibility.x + slot of the
        // variety's setup section (that section owns slots x Visibility.y >= slots rows from Visibility.x, so no
        // blade's record shares one). GrassMeshSetupData's layout (Mesh/Surface/GrassMeshVisibility.glslh) in
        // GrassBladeSetupData's words: Root = the sway now (Common/FoliageWind.glslh FoliageWindInstanceSway at the
        // raster's wind, gust and local wind), Tip = the sway one wind step before (WindB.z, the previous gust and
        // local wind, at the previous row WindB.w further), Before = 0, Frame.x = the LOD level the slot was drawn
        // with (GrassMeshVisibilityLevel's rule over the section's bucket firsts).
        void SetUpMeshSlot( uint r, uint slot, uint row, vec3 origin )
        {
            const uvec4 visibility = CullSetup[r].Shape.Visibility;
            const uint  record     = visibility.x + slot;
            if ( visibility.y == 0u || slot >= visibility.w || record >= uint( CullSetupRecord.length() ) )
                return;
            const vec4 windA   = CullSetup[r].WindA;
            const vec4 windB   = CullSetup[r].WindB;
            const uint prevRow = row + floatBitsToUint( windB.w );
            if ( prevRow >= VarietyRows[r].LawCapacity )
                return;
            const vec3 prevOrigin =
                 prevRow == row ? origin : vec3( GrassLawRows( VarietyRows[r].Instances ).Rows[prevRow][3] );
            uint level = 0u;
            for ( uint b = 1u; b < min( visibility.z, 8u ); ++b )
            {
                if ( slot < CullSetup[r].Shape.BucketFirst[b / 4u][b % 4u] )
                    break;
                level = b;
            }
            GrassBladeSetupData setup;
            setup.Root   = FoliageWindInstanceSway( origin, windA, windB, cameraUB.WindGust,
                                                    LocalWindXZ( origin, false ) );
            setup.Tip    = FoliageWindInstanceSway( prevOrigin, windA, vec4( windB.x, windB.z, 0.0, 0.0 ),
                                                    cameraUB.PrevWindGust, LocalWindXZ( prevOrigin, true ) );
            setup.Before = vec4( 0.0 );
            // GRASS-MESH-RESOLVE-LOST: Frame.y names the slot the record was written for (slot + 1, never 0), so the
            // resolve refuses a row holding anything else (a blade's record, another variety's, never written).
            setup.Frame             = uvec4( level, slot + 1u, 0u, 0u );
            CullSetupRecord[record] = setup;
        }

        // FPS-GRASS-SETUP-FUSE: row @p r's camera list took law row @p row (@p instance, the transform the appending
        // thread already holds) at slot @p slot - its setup record, the raster's blade (Vertex_GrassBlade.glslh):
        // the field under the root, the local wind at it now and one step before, the curve at the draw row's wind
        // and at its previous step (WindB.z), its strip width, card and level, at raster-stream row Visibility.x +
        // slot (the row the visibility word names). A mesh row: SetUpMeshSlot. A row nothing reads this frame
        // (kCullSetupNone): none.
        void SetUpSlot( uint r, uint slot, uint row, mat4 instance )
        {
            const uint kind = CullSetup[r].Kind.x;
            if ( kind == kCullSetupMesh )
            {
                SetUpMeshSlot( r, slot, row, vec3( instance[3] ) );
                return;
            }
            if ( kind != kCullSetupBlade )
                return;
            const GrassBladeShapeData shape  = CullSetup[r].Shape;
            const uint                record = shape.Visibility.x + slot;
            if ( slot >= shape.Visibility.w || record >= uint( CullSetupRecord.length() ) )
                return;
            const vec3 root    = vec3( instance[3] );
            const vec4 windA   = CullSetup[r].WindA;
            const vec4 windB   = CullSetup[r].WindB;
            const vec3 trample = DeformationFieldTrampleFrom( DeformHeight0, DeformHeight1, DeformDetail0,
                                                              DeformDetail1, DeformationFieldSampler, FieldWindows,
                                                              FieldLevels, root );
            const BladeCurve now = BladeCurveAt( shape.Size, shape.Bend, instance, windA, windB, cameraUB.WindGust,
                                                 LocalWindXZ( root, false ), trample );
            const BladeCurve before =
                 BladeCurveAt( shape.Size, shape.Bend, instance, windA, vec4( windB.x, windB.z, 0.0, 0.0 ),
                               cameraUB.PrevWindGust, LocalWindXZ( root, true ), trample );
            CullSetupRecord[record] = BladeSetupOf( now, before, BladeWidth( shape, instance ),
                                                    BladeCardIndex( shape, instance[0].w ),
                                                    BladeLevelSegments( shape, slot ), instance[2].w );
        }

        // THE WAVE'S APPEND (GRASS-CULL-1; UE InstanceCulling: one atomic per wave and list): every invocation that
        // calls it takes one entry of the counter at word @p at of @p counters, row @p r's. The subgroup's
        // invocations that name the same counter of the same row add their number with ONE atomicAdd and each takes
        // its rank among them. One round per distinct (row, counter) among the callers - mode 1's subgroup holds one
        // chunk's records (FPS-GRASS-CULL: a workgroup per chunk), so it spans one row and few bucket boundaries,
        // and its instances split into the visible and the hidden.
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

        // FPS-GRASS-CULL-COST, A COUNTER READ BY THE LAST GROUP: a word other workgroups of the same dispatch
        // appended to (WaveAppend's atomics), read at the device scope - an atomic, never a load another group's
        // cache line could serve stale. The finalize (mode 2), now run by mode 1's last group, reads its counts so.
        uint CounterRead( GrassWords counters, uint at )
        {
            return atomicAdd( counters.Words[at], 0u );
        }

        // A mesh's level seen from @p eye (UE ComputeStaticMeshLOD over the grass HISM's instances, Geometry::
        // SelectLODFromBounds): the coarsest whose screen size the instance's bounding sphere (@p centre, the law's
        // @p scale) is still below - at the distance x the Foliage quality's MeshLodDistanceScale (UE
        // foliage.LODDistanceScale; p.Lod.y carries its square, GrassRenderer::PlanVariety).
        uint GrassMeshLevel( GrassParams p, GrassWords args, vec3 centre, vec3 scale, vec3 eye )
        {
            float r        = p.Lod.x * max( scale.x, max( scale.y, scale.z ) );
            vec3  d        = centre - eye;
            float screenSq = r * r / max( 1.0, dot( d, d ) * p.Lod.y );
            for ( uint level = p.Buckets.x - 1u; level > 0u; --level )
            {
                float halfSize = uintBitsToFloat( args.Words[p.Buckets.y + level * 4u + 3u] );
                if ( halfSize * halfSize > screenSq )
                    return level;
            }
            return 0u;
        }

        // THE VIEW LAW (GRASS-VIEW-LAW-1, moved unchanged from GrassGenerate's tile cull, which is gone; UE landscape
        // grass: the instance cull's distance cull, LOD and PerInstanceFadeAmount, per view and frame): record @p r of
        // a variety with params @p p and bucket table in @p args - false when the law drops it, else its scale
        // @p outScale (GrassLawInstance makes its row of it) and its bucket @p outBucket. Everything view-dependent
        // is here. LOD-DITHER-GRASS-SIDE: a mesh's level is selected at the law's two LOD eyes (@p eyes, the row's
        // LodEyes: [0] older, [1] newer) - @p outBucket the newer's, @p outOlder the older's; they differ while the
        // instance cross-fades (LawChunk's fade twin). A blade's @p outOlder is its bucket.
        bool GrassViewLaw( GrassParams p, GrassWords args, TileRecord r, vec4 eyes[2], out vec3 outScale,
                           out uint outBucket, out uint outOlder )
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
                // THE LOD OF THE CURVE (FPS-GRASS-RASTER-COST, GrassFieldShare.glslh GrassBladeCurveLevel): the
                // blade's height on screen at the scale it is drawn with (the thinning's lowering, the fade and the
                // clump's height in) picks the coarsest strip whose segments stay BladeSegmentPixels tall - never
                // finer than its ring or far LOD - and a blade below half a pixel is culled (p.Lod.x / y,
                // p.Buckets.w; 0: the rings alone).
                if ( p.Buckets.w > 0u )
                {
                    float pixelsTall = p.Lod.x * scale.y / max( distance, 1.0 );
                    if ( pixelsTall < kGrassBladeCullPixels )
                        return false;
                    lod.Level = GrassBladeCurveLevel( pixelsTall, p.Lod.y, p.Buckets.w, lod.Level, bladeLevels );
                }
                bucket = lod.Level;
                if ( p.Lod.w > 0.0 && lod.Level + 1u == bladeLevels && distance >= p.Lod.w )
                    bucket = bladeLevels;
            }
            uint older = bucket;
            if ( !blade && p.Buckets.x > 1u )
            {
                // A mesh's level at each LOD eye (UE ComputeStaticMeshLOD over the grass HISM's instances at the
                // view's two temporal LOD samples, Geometry::SelectDitheredLODFromBounds).
                bucket = GrassMeshLevel( p, args, centre, scale, eyes[1].xyz );
                older  = GrassMeshLevel( p, args, centre, scale, eyes[0].xyz );
            }
            outScale  = scale;
            outBucket = bucket;
            outOlder  = older;
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
        // FPS-GRASS-CULL-2: a list of @p inside (bit k: list k's caster box holds the chunk's kept box whole,
        // BoxInsideViews) holds it untested - its reach lies inside that box.
        uint CascadeHolds( uint r, vec3 root, float scale, uint met, uint inside )
        {
            uint held = 0u;
            if ( VarietyRows[r].Info.w == 0u || met == 0u )
                return held;
            vec4  bounds = VarietyRows[r].Bounds;
            float reach  = ( abs( bounds.y ) + bounds.x ) * scale + bounds.z;
            uint  lists  = uint( bitCount( VarietyRows[r].Info.w ) );
            for ( uint k = 0u; k < lists; ++k )
            {
                uint list  = 1u << k;
                bool holds = ( met & list ) != 0u;
                for ( uint i = 0u; i < 6u && holds && ( inside & list ) == 0u; ++i )
                {
                    vec4 plane = VarietyCascadePlanes[k * 6u + i];
                    holds      = dot( plane.xyz, root ) + plane.w >= -reach;
                }
                if ( holds )
                    held |= list;
            }
            return held;
        }

        // The instance cull (CullInstance), a lane's instance of a met chunk.
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
                    uint rest   = min( CounterRead( cascade, rests + bucket ), room );
                    cascade.Words[base + 4u + s * 5u + 1u]   = min( CounterRead( cascade, at ), room );
                    cascade.Words[base + 4u + s * 5u + 4u]   = cascade.Words[at + 1u] + idsBase;
                    cascade.Words[base + args + s * 5u + 1u] = rest;
                    cascade.Words[base + args + s * 5u + 4u] = cascade.Words[at + 1u] + room - rest + idsBase;
                }
                uint drawn = 0u;
                for ( uint b = 0u; b < buckets; ++b )
                {
                    uint at = table + b * 4u;
                    drawn += min( CounterRead( cascade, at ), cascade.Words[at + 2u] ) +
                             min( CounterRead( cascade, rests + b ), cascade.Words[at + 2u] );
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

        // The instance cull, one lane: row @p r's instance @p instance, the law's row @p row of Instances
        // (FPS-ARCH-GRASSROWS; UE GPU Scene / InstanceCulling: an instance's data is written once when it
        // changes, the views' lists hold ids), in bucket @p b, of a chunk whose views are @p views (BoxMeetsViews:
        // the cascade lists it met - bit k: list k -, the camera's side planes and - inside them - whether the
        // previous frame's HZB hid it whole; BoxInsideViews: the met views that hold it whole, whose planes this
        // instance is not tested by again); @p held: the row's cascades and its full list hold from an earlier frame. A view
        // that keeps it appends the row's id (row + InstancesBase) to its list - the cull writes 4 bytes per list,
        // never the 64-byte row, which LawChunk wrote once this law generation. The cascades and the full list (the
        // union of the cascades
        // and the camera's planes - RT and the list-less cascades draw it, so grass the camera cannot see still
        // casts and reflects) take a slot of the full list's bucket; the camera's list and its candidates their
        // own. A held full list keeps the ids of the frame that culled it, as its cascades keep their depth: the
        // law generation is in their key (GrassRenderer CascadeKey), so the rows they name stand.
        void CullInstance( uint r, uint b, uint row, mat4 instance, uint views, bool held )
        {
            uint       met          = views & kCascadeViews;
            bool       seen         = ( views & kCameraView ) != 0u;
            bool       chunkHidden  = ( views & kChunkHidden ) != 0u;
            bool       cameraInside = ( views & kCameraInside ) != 0u;
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
            // DEFORM-GRASS-3 (UE: a deformed instance widens its bounds by its largest displacement): a blade rooted
            // inside the deformation field's window may lie flat in any direction, so the camera tests the
            // root-centred sphere of its laid-flat reach - the cascades' sphere (CascadeHolds), which the chunk and
            // tile bounds already hold. Outside the window the upright sphere stands.
            vec2 root = instance[3].xz;
            if ( ( VarietyRows[r].Info.z & kGrassRowBlade ) != 0u &&
                 all( greaterThanEqual( root, VarietyTrampleWindow.xy ) ) &&
                 all( lessThanEqual( root, VarietyTrampleWindow.zw ) ) )
            {
                centre = instance[3].xyz;
                radius = ( abs( bounds.y ) + bounds.x ) * scale + bounds.z;
            }
            // The shadow views first: an instance outside the camera's volume still casts into a cascade.
            uint holds  = CascadeHolds( r, instance[3].xyz, scale, met, ( views >> kInsideShift ) & kCascadeViews );
            bool inside = seen;
            for ( int i = 0; i < 4 && inside && !cameraInside; ++i )
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
            {
                GrassWords( VarietyRows[r].CameraIds ).Words[first + index] = id;
                // FPS-GRASS-SETUP-FUSE: the slot just taken gets its setup record from this thread's transform.
                SetUpSlot( r, first + index, row, instance );
            }
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

        // A box (@p centre, @p extent) lies wholly on the inner side of @p plane (xyz.n + w >= 0 inside).
        bool BoxInsidePlane( vec4 plane, vec3 centre, vec3 extent )
        {
            return dot( plane.xyz, centre ) + plane.w >= dot( abs( plane.xyz ), extent );
        }

        // FPS-GRASS-CULL-2: of the views @p views a box (@p lo..@p hi) met (BoxMeetsViews), those whose volume holds
        // it whole, as kInsideShift's bits (kCameraInside: the camera's side planes) - a view's every plane tested
        // only for the views it met.
        uint BoxInsideViews( uint r, vec3 lo, vec3 hi, uint views )
        {
            vec3 centre = 0.5 * ( lo + hi );
            vec3 extent = 0.5 * ( hi - lo );
            uint inside = 0u;
            uint lists  = uint( bitCount( VarietyRows[r].Info.w ) );
            for ( uint k = 0u; k < lists; ++k )
            {
                bool whole = ( views & ( 1u << k ) ) != 0u;
                for ( uint i = 0u; i < 6u && whole; ++i )
                    whole = BoxInsidePlane( VarietyCascadePlanes[k * 6u + i], centre, extent );
                if ( whole )
                    inside |= 1u << ( kInsideShift + k );
            }
            bool whole = ( views & kCameraView ) != 0u;
            for ( uint i = 0u; i < 4u && whole; ++i )
                whole = BoxInsidePlane( VarietyCameraPlanes[i], centre, extent );
            if ( whole )
                inside |= kCameraInside;
            return inside;
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

        // Row @p r's tile slots: the slots whose chunks ChunkCount covers.
        uint TileCount( uint r )
        {
            return ( ChunkCount( r ) + kGrassTileChunks - 1u ) / kGrassTileChunks;
        }

        // FPS-GRASS-CULL-GPU2 (UE InstanceCulling / Nanite's cull hierarchy: a node's conservative bound rejects
        // its children whole): whether ChunkMayMeet may hold for any chunk of tile slot @p slot of row @p r, from
        // the slot's node (Common/GrassVarietyRows.glslh) - the same test on the union of its chunks' root boxes at
        // their largest scale. Every step only grows with the box and the scale: the nearest point of a larger box
        // is no farther (the cull radius passes for the node when it does for a chunk), its farthest corner no
        // nearer, GrassLawScaleBound grows with the farthest and the scale (the thinning's Widen =
        // 1 / sqrt(max(Kept, 0.05)) with Kept falling with distance, the floor's Lean.w x farthest, the max), the
        // reach with the scale, and a box that holds another meets every plane and cascade box the other meets. So
        // a slot it rejects holds no chunk the law would law this frame, and - a lawed chunk's kept box lying
        // inside its ChunkMayMeet box, the law's instances' reach at their own distance and scale - none whose kept
        // box a view meets: the chunk pass over the slots it keeps meets the same chunks the walk of every chunk
        // did. A node with no record (scale word 0) holds no chunk with records.
        bool TileMayMeet( uint r, uint slot, GrassParams p, bool held )
        {
            GrassWords chunks  = GrassWords( VarietyRows[r].Chunks );
            uint       at      = GrassTileNodeAt( chunks.Words[2], slot );
            uint       largest = chunks.Words[at + kGrassTileNodeScaleWord];
            if ( largest == 0u )
                return false;
            vec3 lo = uintBitsToFloat( uvec3( chunks.Words[at], chunks.Words[at + 1u], chunks.Words[at + 2u] ) );
            vec3 hi = uintBitsToFloat( uvec3( chunks.Words[at + 3u], chunks.Words[at + 4u], chunks.Words[at + 5u] ) );
            const vec3 eye = p.Camera.xyz;
            if ( length( clamp( eye, lo, hi ) - eye ) > p.Cull.y + kTileNodePad )
                return false;
            float farthest = length( max( abs( lo - eye ), abs( hi - eye ) ) ) + kTileNodePad;
            float scale    = GrassLawScaleBound( p, farthest, uintBitsToFloat( largest ) ) *
                          ( 1.0 + kTileNodeSlack );
            vec4  bounds   = VarietyRows[r].Bounds;
            float reach    = ( ( abs( bounds.y ) + bounds.x ) * scale + bounds.z ) * ( 1.0 + kChunkReachSlack ) +
                          kTileNodePad;
            lo.y -= p.Lean.z;
            hi.y -= p.Lean.z;
            return BoxMeetsViews( r, lo - vec3( reach ), hi + vec3( reach ), held ) != 0u;
        }

        // CullTiles' tiles appended (never past its room, Rows.y).
        uint TilesListed()
        {
            return min( TileCommand[3], Rows.y );
        }

        // FPS-GRASS-CULL-COST, THE ROWS' RESTART (was mode 0's, a one-group pass of its own): by mode 6's first
        // group, a row a lane, before the chunk pass (LawChunk's slots) and the instance pass (the full list's
        // appends) - a barrier after it. A row whose cascades are culled this frame appends its full list anew
        // (CullInstance: Args' bucket counts zeroed); a new law generation hands the law's slots out anew (LawChunk;
        // the chunk contract's header y, w). A row whose camera holds restarts its slots too, as before: its Feedback
        // reports them (HeldCamera).
        void RestartRows()
        {
            for ( uint r = gl_LocalInvocationIndex; r < Rows.x; r += kCullGroupThreads )
            {
                if ( ( VarietyRows[r].Info.z & kGrassRowCascadesHeld ) == 0u )
                {
                    GrassWords args    = GrassWords( VarietyRows[r].Args );
                    uint       tableAt = VarietyRows[r].Info.y;
                    for ( uint b = 0u; b < VarietyRows[r].Info.x; ++b )
                        args.Words[tableAt + b * 4u] = 0u;
                }
                GrassWords chunks = GrassWords( VarietyRows[r].Chunks );
                if ( chunks.Words[kGrassChunkSlotsGenWord] != VarietyRows[r].LawGeneration )
                {
                    chunks.Words[kGrassChunkSlotsUsedWord] = 0u;
                    chunks.Words[kGrassChunkSlotsGenWord]  = VarietyRows[r].LawGeneration;
                }
            }
        }

        // Mode 6, THE CAMERA CULL'S TILES (FPS-GRASS-CULL-GPU2; UE InstanceCulling / Nanite: the nodes' pass appends
        // the nodes that survive and grows the children's indirect dispatch): kTileSlotsPerGroup tile slots of one row
        // per workgroup (CullDispatchData[0], laid out by mode 0), a lane each. A slot whose node a view may meet
        // (TileMayMeet) is appended to CullTiles with one atomic per subgroup, and mode 1's dispatch grows to cover it
        // (atomicMax: the last append's end is the largest). The rows' slots out of every view - most of a field's -
        // cost one lane here and nothing in mode 1.
        void CameraTiles()
        {
            uint group = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
            if ( group == 0u )
                RestartRows();
            if ( group >= CullDispatchData[0].w )
                return;
            uint r    = RowOfGroup( group, 0u );
            uint slot = ( group - FirstGroup( r, 0u ) ) * kTileSlotsPerGroup + gl_LocalInvocationIndex;
            bool held = ( VarietyRows[r].Info.z & kGrassRowCascadesHeld ) != 0u;
            bool met  = false;
            // The renderer lays out every row's slots (GrassRenderer, its chunk room): a row whose camera holds (and
            // so its cascades and full list) culls nothing this frame - its lanes meet nothing.
            if ( slot < TileCount( r ) && ( VarietyRows[r].Info.z & kGrassRowCameraHeld ) == 0u )
                met = TileMayMeet( r, slot, GrassParamsRef( VarietyRows[r].Params ).Data[0], held );
            uvec4 ballot = subgroupBallot( met );
            uint  n      = subgroupBallotBitCount( ballot );
            if ( n == 0u )
                return;
            uint base = 0u;
            if ( subgroupElect() )
            {
                base        = atomicAdd( TileCommand[3], n );
                uint end    = min( base + n, Rows.y );
                uint groups = end * kChunkGroupsPerTile;
                atomicMax( TileCommand[0], min( groups, kCullGroupsX ) );
                atomicMax( TileCommand[1], ( groups + kCullGroupsX - 1u ) / kCullGroupsX );
            }
            uint index = subgroupBroadcastFirst( base ) + subgroupBallotExclusiveBitCount( ballot );
            if ( met && index < Rows.y )
                TileList[index] = uvec2( r, slot );
        }

        // A float's bits in an order that compares as the float does (non-NaN): the kept box's min / max reduced by
        // shared atomics across the workgroup's subgroups (LawChunk).
        uint FloatOrder( float value )
        {
            uint bits = floatBitsToUint( value );
            return ( bits & 0x80000000u ) != 0u ? ~bits : bits | 0x80000000u;
        }
        float OrderFloat( uint order )
        {
            return uintBitsToFloat( ( order & 0x80000000u ) != 0u ? order & 0x7FFFFFFFu : ~order );
        }

        // THE LAW, ONCE PER LAW GENERATION (GRASS-CULL-COST-3; UE GPU Scene: an instance's data is written once when
        // it changes, the views only cull it): the view law sees nothing of the view but the field eye
        // (GrassParams.Camera, the camera snapped to the tile grid), so what it makes of a record holds for its row's
        // law generation (LawGeneration). The chunk at word @p at of row @p r, @p count records, lawed by its WHOLE
        // WORKGROUP (FPS-GRASS-CULL: mode 1 runs a workgroup per chunk), lane i = record i (count <=
        // kGrassTilePageRecords = kCullGroupThreads): the run past the cut at the box's nearest point is never kept
        // (the run ascends by fade draw and the share only falls with distance), the whole chunk past the cull
        // radius. The run before the cut gets its slots of Instances, one per record (Chunks' header y; none when
        // they would pass LawCapacity - the law then keeps nothing of the chunk this generation, and the renderer,
        // fed the uncapped count, grows the law's rows, which is a new generation). Every kept record's row
        // (GrassLawInstance; 64 B, FPS-ARCH-GRASSROWS: written here once per law generation, the views' lists only
        // name it) goes into its slot, its bucket + 1 into the code planes (@p outLow: records 0..31, @p outHigh:
        // 32..63, plane b at [b]) and the reach of its cascade sphere about its root - which holds its camera sphere
        // - into the kept box (@p outLo .. @p outHi; inverted when nothing is kept). They become the chunk's law
        // words, stamped with the generation; @p outSlot: the chunk's first slot. @p outInstance: lane i's record's
        // row as written (its fade side marked), so the instance cull reads no row this lane just wrote. Every
        // invocation of the workgroup calls it, in uniform control flow (its barriers); the reductions across the
        // subgroups go through shared memory (the run's end, the kept box, the code planes, the twins' count).
        void LawChunk( uint r, uint at, uint count, GrassParams p, out vec3 outLo, out vec3 outHi, out uvec4 outLow,
                       out uvec4 outHigh, out uint outSlot, out mat4 outInstance )
        {
            uint       i      = gl_LocalInvocationIndex;
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
            GrassSegments pool  = GrassSegments( VarietyRows[r].TilePool );
            uint          first = chunks.Words[at + kGrassChunkFirstWord];
            uint          page  = chunks.Words[at + kGrassChunkPageWord];
            if ( i < 8u )
                s_LawPlanes[i] = 0u;
            if ( i < 3u )
            {
                s_LawBox[i]      = FloatOrder( 1.0e30 );
                s_LawBox[i + 3u] = FloatOrder( -1.0e30 );
            }
            if ( i == 0u )
            {
                s_LawRun   = 0u;
                s_LawTwins = 0u;
            }
            // Lane i's record, read once: its fade draw decides the run, its words the law.
            GrassQuads quads  = GrassQuads( uvec2( 0u ) ); // read only below i < count
            uint       quad   = 0u;
            uvec4      head0  = uvec4( 0u );
            bool       passes = false;
            if ( i < count )
            {
                uint record = GrassChunkRecord( first, page, i );
                quads       = GrassPoolSegment( pool, record );
                quad        = GrassSegmentRecord( record ) * kGrassTilePoolWords;
                head0       = quads.Quads[quad];
                passes      = unpackUnorm2x16( head0.w ).x < cut;
            }
            barrier();
            // The run before the cut: up to its last record whose fade draw passes the cut.
            if ( passes )
                atomicMax( s_LawRun, i + 1u );
            barrier();
            uint run = s_LawRun;
            if ( i == 0u )
            {
                uint taken = 0u;
                if ( run != 0u )
                    taken = atomicAdd( chunks.Words[kGrassChunkSlotsUsedWord], run );
                s_LawSlot = run == 0u || taken + run > VarietyRows[r].LawCapacity ? kGrassLawNoSlot : taken;
            }
            barrier();
            uint slot = s_LawSlot;

            g_Field                  = GrassWords( VarietyRows[r].Field );
            GrassWords        args   = GrassWords( VarietyRows[r].Args );
            GrassInstanceRows rows   = GrassInstanceRows( VarietyRows[r].Instances );
            vec4              bounds = VarietyRows[r].Bounds;
            bool              blade  = ( floatBitsToUint( p.ScaleX.w ) & kFlagBlade ) != 0u;
            uint              lawed  = slot == kGrassLawNoSlot ? 0u : run;
            vec4              eyes[2] = VarietyRows[r].LodEyes;
            uint              code     = 0u;
            uint              older    = 0u;
            mat4              instance = mat4( 0.0 );
            if ( i < lawed && passes )
            {
                TileRecord tile = GrassUnpackRecord( head0, quads.Quads[quad + 1u], quads.Quads[quad + 2u] );
                vec3       scale;
                uint       b;
                if ( GrassViewLaw( p, args, tile, eyes, scale, b, older ) )
                {
                    instance    = GrassLawInstance( tile, scale, blade, p.Lean.z );
                    float reach = ( ( abs( bounds.y ) + bounds.x ) * InstanceScale( instance ) + bounds.z ) *
                                  ( 1.0 + kChunkReachSlack );
                    vec3 keptLo = instance[3].xyz - vec3( reach );
                    vec3 keptHi = instance[3].xyz + vec3( reach );
                    for ( uint k = 0u; k < 3u; ++k )
                    {
                        atomicMin( s_LawBox[k], FloatOrder( keptLo[k] ) );
                        atomicMax( s_LawBox[k + 3u], FloatOrder( keptHi[k] ) );
                    }
                    code = b + 1u;
                }
            }
            // Plane b's bit i: record i's code bit b (word 2b: records 0..31, 2b + 1: 32..63).
            for ( uint plane = 0u; plane < 4u; ++plane )
            {
                if ( ( ( code >> plane ) & 1u ) != 0u )
                    atomicOr( s_LawPlanes[plane * 2u + i / 32u], 1u << ( i % 32u ) );
            }
            // LOD-DITHER-GRASS-SIDE, THE FADE TWIN (UE DitheredLODTransition: both levels drawn during the fade,
            // complementary dither; the cascade lists' rest twins are the same idea): a kept record whose two LOD
            // eyes select different levels takes a second slot of Instances, from the same counter as the runs (so
            // the renderer, fed the uncapped count, grows LawCapacity by the twins). Its row is the record's with
            // column 0 w = +1 (the older level fades out); the record's own row carries -1 (the newer fades in), the
            // twin's slot in column 1 w and the older bucket in column 2 w (exact floats: InstancedWorldPosition and
            // the normal matrix read only the columns' xyz, a mesh row's w are 0 otherwise). The instance cull culls
            // the twin with its record (CameraCull). A twin past LawCapacity, or past what a float holds exactly, is
            // not made: the record draws its newer level alone (side 0). The chunk's twins take one atomic.
            bool fades = code != 0u && older + 1u != code;
            uint rank  = 0u;
            if ( fades )
                rank = atomicAdd( s_LawTwins, 1u );
            barrier();
            if ( i == 0u )
            {
                uint n         = s_LawTwins;
                s_LawTwinFirst = n == 0u ? 0u : atomicAdd( chunks.Words[kGrassChunkSlotsUsedWord], n );
            }
            barrier();
            if ( code != 0u )
            {
                uint twin = s_LawTwinFirst + rank;
                if ( fades && twin < VarietyRows[r].LawCapacity && twin < kGrassFadeTwinRows )
                {
                    mat4 faded      = instance;
                    faded[0].w      = 1.0;
                    rows.Rows[twin] = faded;
                    instance[0].w   = -1.0;
                    instance[1].w   = float( twin );
                    instance[2].w   = float( older );
                }
                rows.Rows[slot + i] = instance;
            }
            outInstance = instance;
            outLo       = vec3( OrderFloat( s_LawBox[0] ), OrderFloat( s_LawBox[1] ), OrderFloat( s_LawBox[2] ) );
            outHi       = vec3( OrderFloat( s_LawBox[3] ), OrderFloat( s_LawBox[4] ), OrderFloat( s_LawBox[5] ) );
            outLow      = uvec4( s_LawPlanes[0], s_LawPlanes[2], s_LawPlanes[4], s_LawPlanes[6] );
            outHigh     = uvec4( s_LawPlanes[1], s_LawPlanes[3], s_LawPlanes[5], s_LawPlanes[7] );
            outSlot     = slot;
            if ( i == 0u )
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

        // Mode 1 (GRASS-CULL-COST-3, FPS-GRASS-CULL; UE InstanceCulling / Nanite's cluster cull, Ghost of Tsushima's
        // grass: a node that survives has its elements culled, one thread each): A WORKGROUP PER CHUNK of the tiles
        // mode 6 let through (CullTiles, indirect by its TileCommand: kGrassTileChunks groups a tile), lane i = the
        // chunk's record i (a chunk holds at most kGrassTilePageRecords = kCullGroupThreads records). The chunk's
        // words are uniform across the group: (1) the law, once per law generation - a chunk a view may meet whose
        // law word carries another generation is lawed by the whole group (LawChunk), its lanes keeping the rows they
        // wrote; (2) the chunk's kept box meets the views and, the camera's planes met, the previous frame's HZB -
        // a chunk no view met, or with no kept record, leaves the group at once; (3) each lane whose record the chunk
        // keeps (below LawCapacity, FPS-DEVLOST) culls it (CullInstance) with its fade twin, by the views its chunk
        // met and does not lie inside whole (BoxInsideViews). FPS-GRASS-CULL: was a lane per chunk and its subgroup
        // looping over the met chunks' records compacted across them (CullSubgroupRecords) - a subgroup carried 32
        // chunks' records (up to 64 rounds of the instance cull and blade setup, each a chain of dependent atomics,
        // row and HZB reads) and the field's few hundred subgroups ran them serially; now every record has its own
        // lane in its own short group, so the GPU runs them concurrently and the latency hides. A still or turning
        // camera laws nothing (the law generation holds).
        void CameraCull()
        {
            uint group = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
            uint entry = group / kGrassTileChunks;
            if ( entry >= TilesListed() )
                return;
            uvec2 tile = TileList[entry];
            uint  r    = tile.x;
            uint  c    = tile.y * kGrassTileChunks + group % kGrassTileChunks;
            if ( c >= ChunkCount( r ) )
                return;
            GrassWords chunks = GrassWords( VarietyRows[r].Chunks );
            uint       at     = GrassChunkAt( c );
            uint       count  = chunks.Words[at + kGrassChunkCountWord];
            if ( count == 0u )
                return;
            bool  held     = ( VarietyRows[r].Info.z & kGrassRowCascadesHeld ) != 0u;
            uint  i        = gl_LocalInvocationIndex;
            vec3  lo       = vec3( 0.0 );
            vec3  hi       = vec3( 0.0 );
            uvec4 low      = uvec4( 0u );
            uvec4 high     = uvec4( 0u );
            uint  slot     = kGrassLawNoSlot;
            mat4  instance = mat4( 0.0 );
            // (1) Every lane read the chunk's law word before any lane of this group (its only writer) writes it:
            // the branch is uniform, LawChunk's barriers with it.
            bool fresh = chunks.Words[at + kGrassChunkLawWord] != VarietyRows[r].LawGeneration;
            if ( fresh )
            {
                GrassParams p = GrassParamsRef( VarietyRows[r].Params ).Data[0];
                if ( !ChunkMayMeet( r, at, p, held ) )
                    return;
                LawChunk( r, at, count, p, lo, hi, low, high, slot, instance );
            }
            else
                ChunkLawWords( chunks, at, lo, hi, low, high, slot );

            // (2) The chunk's kept box against the views - uniform across the group, so one scalar test.
            uvec2 kept = uvec2( low.x | low.y | low.z | low.w, high.x | high.y | high.z | high.w );
            if ( ( kept.x | kept.y ) == 0u || slot == kGrassLawNoSlot || slot >= VarietyRows[r].LawCapacity )
                return;
            uint views = BoxMeetsViews( r, lo, hi, held );
            if ( views == 0u )
                return;
            if ( ( views & kCameraView ) != 0u && Hzb.z != 0u )
            {
                GpuCullView view;
                view.HzbExtent = uvec4( Hzb.xyz, 0u );
                if ( GpuCull_IsOccludedByHzb( GrassHzbTexture, PointClampSampler, view, HzbViewProjection, lo, hi ) )
                    views |= kChunkHidden;
            }
            views |= BoxInsideViews( r, lo, hi, views );

            // (3) Lane i's record, kept and below LawCapacity (FPS-DEVLOST: a record past it is never read).
            uvec2 records = kept & RecordsBelow( VarietyRows[r].LawCapacity - slot );
            uint  bit     = i % 32u;
            if ( ( ( ( i < 32u ? records.x : records.y ) >> bit ) & 1u ) == 0u )
                return;
            uvec4 planes = i < 32u ? low : high;
            uint  code   = ( ( planes.x >> bit ) & 1u ) | ( ( ( planes.y >> bit ) & 1u ) << 1u ) |
                         ( ( ( planes.z >> bit ) & 1u ) << 2u ) | ( ( ( planes.w >> bit ) & 1u ) << 3u );
            // The record's row as LawChunk wrote it this law generation (its slot: the chunk's first + i) - this
            // lane's own registers when the group lawed it just now.
            uint         row  = slot + i;
            GrassLawRows rows = GrassLawRows( VarietyRows[r].Instances );
            if ( !fresh )
                instance = rows.Rows[row];
            CullInstance( r, code - 1u, row, instance, views, held );
            // LOD-DITHER-GRASS-SIDE: a record in a LOD cross-fade (column 0 w = -1, LawChunk's fade twin) brings its
            // twin - the older level's row, at the slot its column 1 w names, into the older bucket (column 2 w) - to
            // the same views (the same transform, so the same tests): the two levels' dithers are complementary. A
            // blade's column 0 w is its draw (>= 0), never a side. The twin's row: written by this very lane when
            // the group lawed the chunk now.
            if ( ( VarietyRows[r].Info.z & kGrassRowBlade ) == 0u && instance[0].w < 0.0 )
            {
                uint twin  = uint( instance[1].w );
                uint older = uint( instance[2].w );
                if ( twin < VarietyRows[r].LawCapacity && older < VarietyRows[r].Info.x )
                    CullInstance( r, older, twin, rows.Rows[twin], views, held );
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
            uint index = camera.Words[phase + 2u] + WaveAppend( camera, phase + 3u, r );
            if ( index >= args.Words[table + 2u] )
                return;
            uint row = candidates.Quads[2u * slot + 1u].x;
            GrassWords( VarietyRows[r].CameraIds ).Words[first + index] = row + VarietyRows[r].InstancesBase;
            // FPS-GRASS-SETUP-FUSE: phase 2's slots follow phase 1's in the same list, so its records land on rows
            // phase 1 did not write.
            if ( CullSetup[r].Kind.x != kCullSetupNone && row < VarietyRows[r].LawCapacity )
                SetUpSlot( r, first + index, row, GrassLawRows( VarietyRows[r].Instances ).Rows[row] );
        }

        // Mode 7 (FPS-GRASS-SETUP-FUSE): one thread per phase-1 drawn slot of every row whose camera list holds - the
        // groups the camera cull's finalize laid out (LayOutRows, CullDispatchData[2] and the rows' z). The held
        // list's slots, bucket by bucket (each bucket's drawn count frozen in its third phase word), get their setup
        // records at this frame's wind from the law rows their ids name.
        void HeldSetup()
        {
            uint group = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
            if ( group >= CullDispatchData[2].w )
                return;
            uint       r       = RowOfGroup( group, 2u );
            uint       t       = ( group - FirstGroup( r, 2u ) ) * kCullGroupThreads + gl_LocalInvocationIndex;
            GrassWords args    = GrassWords( VarietyRows[r].Args );
            GrassWords camera  = GrassWords( VarietyRows[r].CameraArgs );
            uint       buckets = VarietyRows[r].Info.x;
            uint       tables  = VarietyRows[r].Info.y;
            uint       phases  = tables + buckets * 4u;
            uint       b       = 0u;
            for ( ; b < buckets; ++b )
            {
                uint seen = min( camera.Words[phases + b * 4u + 2u], args.Words[tables + b * 4u + 2u] );
                if ( t < seen )
                    break;
                t -= seen;
            }
            if ( b == buckets )
                return;
            uint slot = args.Words[tables + b * 4u + 1u] + t;
            uint id   = GrassWords( VarietyRows[r].CameraIds ).Words[slot];
            uint base = VarietyRows[r].InstancesBase;
            if ( id < base || id - base >= VarietyRows[r].LawCapacity )
                return;
            uint row = id - base;
            SetUpSlot( r, slot, row, GrassLawRows( VarietyRows[r].Instances ).Rows[row] );
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

        // Modes 6 / 2, row @p r's FULL LIST (GRASS-VIEW-LAW-1; moved from GrassFinalize.shader): Args' bucket counts
        // zeroed before a frame that culls its cascades appends anew (RestartRows), then its records, its unclamped
        // counts into this frame in flight's Feedback (the renderer sizes the capacities from them), the written mark
        // and the drawn count (mode 2). A row whose cascades hold keeps the counts and the list of the frame that
        // culled them, so every frame in flight's copy gets them.
        // FPS-DEVLOST6, THE LOOPS' BOUNDS ARE THE CPU'S (UE GPU culling: the bounds and capacities of a list come
        // from the CPU's per-group constants, GPU-written words only ever as counts clamped to them): a row's
        // bucket count and record count come from its row (Info.x; Info.y = the bucket table's first word =
        // GpuInstances.hpp GpuInstanceArgsOffset( records ) / 4 = 4 + records x 5), never from its lists'
        // header words (x = buckets, z = records), which the GPU can read before their upload lands or after a
        // stray write - one such word near 2^32 made the one-group finalize loop ~4e9 times and write past its
        // range through the device address. Every write below is inside [0, Info.y + Info.x x 4), the list's
        // range (GrassRenderer argBytes). A row with no bucket has no record to draw: it writes nothing.
        uint RowBuckets( uint r )
        {
            return VarietyRows[r].Info.x;
        }
        uint RowRecords( uint r )
        {
            return ( max( VarietyRows[r].Info.y, 4u ) - 4u ) / 5u;
        }

        void FinalizeFull( uint r )
        {
            GrassWords args     = GrassWords( VarietyRows[r].Args );
            GrassWords feedback = GrassWords( VarietyRows[r].Feedback );
            bool       blade    = ( VarietyRows[r].Info.z & kGrassRowBlade ) != 0u;
            uint       buckets  = RowBuckets( r );
            uint       records  = buckets == 0u ? 0u : RowRecords( r );
            uint       table    = 4u + records * 5u;
            for ( uint s = 0u; s < records; ++s )
            {
                uint at = table + GrassBucketOfRecord( s, records, buckets, blade ) * 4u;
                args.Words[4u + s * 5u + 1u] = min( CounterRead( args, at ), args.Words[at + 2u] );
                args.Words[4u + s * 5u + 4u] = args.Words[at + 1u] + VarietyRows[r].FullIdsBase;
            }
            uint drawn = 0u;
            for ( uint b = 0u; b < buckets; ++b )
            {
                uint count = CounterRead( args, table + b * 4u );
                drawn += min( count, args.Words[table + b * 4u + 2u] );
                feedback.Words[b] = count;
                feedback.Words[buckets + kFeedbackCameraWord + b] = min( count, args.Words[table + b * 4u + 2u] );
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
            uint       buckets = RowBuckets( r );
            uint       records = buckets == 0u ? 0u : RowRecords( r );
            uint       phases  = 4u + records * 5u + buckets * 4u;
            HeldCascadeFeedback( r, buckets );
            uint candidates = 0u;
            for ( uint b = 0u; b < buckets; ++b )
            {
                candidates += camera.Words[phases + b * 4u + 1u];
                camera.Words[phases + b * 4u + 3u] = 0u;
            }
            GrassWords feedback = GrassWords( VarietyRows[r].Feedback );
            for ( uint b = 0u; b < buckets; ++b )
                feedback.Words[buckets + kFeedbackCameraWord + b] = camera.Words[phases + b * 4u + 2u];
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
            uint       buckets = RowBuckets( r );
            uint       records = buckets == 0u ? 0u : RowRecords( r );
            uint       table   = 4u + records * 5u;
            uint       phases  = table + buckets * 4u;
            for ( uint s = 0u; s < records; ++s )
            {
                uint at = table + GrassBucketOfRecord( s, records, buckets, blade ) * 4u;
                camera.Words[4u + s * 5u + 1u] = min( CounterRead( camera, at ), camera.Words[at + 2u] );
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
                uint seen  = min( CounterRead( camera, at ), camera.Words[at + 2u] );
                drawn += seen;
                camera.Words[phase + 1u] = min( CounterRead( camera, phase ), camera.Words[at + 2u] );
                candidates += camera.Words[phase + 1u];
                camera.Words[phase]      = 0u;
                camera.Words[phase + 2u] = seen;
                camera.Words[phase + 3u] = 0u;
                camera.Words[at]         = 0u;
            }
            camera.Words[3] = drawn;
            GrassWords feedback = GrassWords( VarietyRows[r].Feedback );
            for ( uint b = 0u; b < buckets; ++b )
                feedback.Words[buckets + kFeedbackCameraWord + b] = camera.Words[phases + b * 4u + 2u];
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
            uint       buckets = RowBuckets( r );
            uint       records = buckets == 0u ? 0u : RowRecords( r );
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

        // What row @p r gives phase 2, finalizing its camera list: its frozen candidates (mode 2, a thread each).
        uint RowGroups( uint r )
        {
            return ( FinalizeCamera( r ) + kCullGroupThreads - 1u ) / kCullGroupThreads;
        }

        // Mode 2, one group (mode 1's last): row i's work by thread i % 64, 64 rows at a time, and the rows' groups
        // of phase 2 laid end to end in row order - row i's first group into its CullDispatchData entry (y for
        // @p command 1; x, the tile pass's, the renderer lays out), their sum into command @p command.
        // FPS-GRASS-SETUP-FUSE: with them the held setup's (mode 7) - a row whose camera list holds and whose records
        // are read gets a thread per phase-1 drawn slot (its held drawn count, CameraArgs word 3) - into the rows' z
        // and command 2.
        void LayOutRows( uint command )
        {
            uint base     = 0u;
            uint heldBase = 0u;
            for ( uint chunk = 0u; chunk < Rows.x; chunk += kCullGroupThreads )
            {
                uint row    = chunk + gl_LocalInvocationIndex;
                uint groups = 0u;
                uint held   = 0u;
                if ( row < Rows.x )
                {
                    groups = RowGroups( row );
                    if ( ( VarietyRows[row].Info.z & kGrassRowCameraHeld ) != 0u &&
                         CullSetup[row].Kind.x != kCullSetupNone )
                        held = ( GrassWords( VarietyRows[row].CameraArgs ).Words[3] + kCullGroupThreads - 1u ) /
                               kCullGroupThreads;
                }
                s_Groups[gl_LocalInvocationIndex]     = groups;
                s_HeldGroups[gl_LocalInvocationIndex] = held;
                barrier();
                uint first     = base;
                uint heldFirst = heldBase;
                for ( uint k = 0u; k < gl_LocalInvocationIndex; ++k )
                {
                    first += s_Groups[k];
                    heldFirst += s_HeldGroups[k];
                }
                uint total     = 0u;
                uint heldTotal = 0u;
                for ( uint k = 0u; k < kCullGroupThreads; ++k )
                {
                    total += s_Groups[k];
                    heldTotal += s_HeldGroups[k];
                }
                if ( row < Rows.x )
                {
                    CullDispatchData[kDispatchHeader + row][command] = first;
                    CullDispatchData[kDispatchHeader + row][2]       = heldFirst;
                }
                base += total;
                heldBase += heldTotal;
                barrier();
            }
            if ( gl_LocalInvocationIndex == 0u )
            {
                CullDispatchData[command] = CullGroups( base );
                CullDispatchData[2]       = CullGroups( heldBase );
            }
        }

        // Mode 1's workgroup (FPS-GRASS-CULL-COST, FPS-GRASS-CULL-2: was mode 5's): its lanes' chunks and their
        // instances (CameraCull), then the group counts itself done on CullDispatchData[1].w (mode 0 zeroed it; the
        // finalize's layout overwrites it) - every invocation's writes made visible first, one atomic per group - and
        // the dispatch's last group to finish runs the camera cull's finalize (mode 2: LayOutRows( 1u )), its counter
        // reads atomic (CounterRead). Every group of the dispatch counts, a group past the tiles' list too (mode 0's
        // TileCommand gives mode 1 one group at least), so the finalize runs exactly once, after every append of the
        // pass.
        void CameraCullGroup()
        {
            CameraCull();
            memoryBarrierBuffer();
            barrier();
            if ( gl_LocalInvocationIndex == 0u )
            {
                uint groups = gl_NumWorkGroups.x * gl_NumWorkGroups.y * gl_NumWorkGroups.z;
                s_LastGroup = atomicAdd( CullDispatchData[1].w, 1u ) + 1u == groups;
            }
            barrier();
            if ( !s_LastGroup )
                return;
            memoryBarrierBuffer();
            LayOutRows( 1u );
        }

        void main()
        {
            if ( Hzb.w == kModeCameraTiles )
            {
                CameraTiles();
                return;
            }
            if ( Hzb.w == kModeCameraCull )
            {
                CameraCullGroup();
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
            if ( Hzb.w == kModeHeldSetup )
            {
                HeldSetup();
                return;
            }
        }
    }
}
