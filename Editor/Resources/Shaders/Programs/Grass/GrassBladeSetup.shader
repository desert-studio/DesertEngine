// DesertAsset {"Kind":"Shader","Guid":"45b47d20a7974d9eb7bcb2b6d531f624","Versions":{"SHDR":1},"Dependencies":[]}
// THE BLADE SETUP (FPS-GRASS-RESOLVE-2; Ghost of Tsushima, GDC 2021 "Procedural Grass in Ghost of Tsushima": a
// compute pass evaluates each visible blade ONCE into a compact buffer, and the raster and the shading read it
// instead of deriving it again per pixel). Once per phase, right before that phase's visibility raster
// (SceneRenderer::AddFrameGrassVisibility -> AddFrameGrassBladeSetup), for the blade varieties that raster draws
// (MeshRenderer::DrawsGrassVisibilitySection: a variety on the G-buffer path that frame gets no records).
// FPS-GRASS-SETUP-TRIM (GoT: the per-blade compute runs only for the blades the cull kept, indirect-dispatched):
// two dispatches of this program, told apart by BladeSetupInfo.z.
// - kModeWork, one group: for every (variety row, raster record) entry, the slots that record drew (its
//   instanceCount, from the cull), prefix-summed into BladeSetupWork, and the indirect dispatch of the setup sized
//   by their total.
// - kModeSetup, indirect: one thread per DRAWN blade - the entry it falls in (a binary search of the prefix sums),
//   its slot in the variety's camera list (the record's firstInstance + the thread's rank in it) - evaluates that
//   blade's curve now and one wind step before (Mesh/Surface/GrassBlade.glslh BladeCurveAt - the raster's one
//   statement of it, with the raster's wind, local wind, gust and deformation field read at the root), its strip
//   width, its card and its strip level into the blade's row of the view's setup records (GrassBladeSetupData,
//   64 bytes, indexed by the raster-stream row Visibility.x + slot, the row the visibility word names).
// The visibility raster (Mesh/Surface/Vertex_GrassBlade.glslh) places its strip from that record per vertex, and
// "Grass: Visibility Resolve" (Mesh/Surface/Pass_VisibilityResolve.glslh) reads it per pixel. A slot no record
// drew this phase is not written: no word of this phase names its row.
// FPS-GRASS-MESH-SETUP: the same two dispatches set up the mesh grass sections the visibility raster draws (a row
// with Args.y != 0): one thread per drawn instance writes its wind sway now and before and its LOD level
// (SetUpMeshInstance), so the mesh resolve reads one record instead of evaluating the local wind, the sway and
// the gust for every vertex of every pixel.
Shader "GrassBladeSetup"
{
    Compute
    {
        LocalSize(64, 1, 1);

        // The camera block (Common/CameraUB.glslh, ShaderProtocols::MakeCameraUB of the view's frame): the gust and
        // the local wind sources, now and one step before - the very block the raster's vertex stage read.
        #include <Common/LocalWind.glslh>
        #include <Common/FoliageWind.glslh>
        #include <Mesh/Surface/GrassBlade.glslh>
        #include <Deformation/DeformationFieldTrample.glslh>

        // Mirrors Graphic::System::GrassBladeSetupRowGpu (GrassRenderer.cpp), std430: a blade variety's shape (the
        // GrassBladeShapeData its cells read, without the hull), its view wind (Graphic::PackViewInstanceWind, the
        // raster's draw row's WindA / WindB) and where this phase's raster records lie.
        struct GrassBladeSetupRow
        {
            GrassBladeShapeData Shape;
            vec4                WindA;
            vec4                WindB;
            // x = this phase's records' first word in BladeSetupArgs (VarietyGpu::CameraArgs for phase 1,
            // VarietyGpu::PostArgs for phase 2), y = 0 for a blade variety, a mesh section's submesh count (its
            // level L's slots are drawn by records L x y .. L x y + y - 1; entry L reads the first), z = the
            // entries (a blade's records, one per strip level; a mesh section's levels), w = the row's first
            // entry (the entries before it: the earlier rows' entries).
            uvec4 Args;
        };

        ReadBuffer(1) BladeSetupTable
        {
            uvec4              BladeSetupInfo; // x = rows, y = entries (all rows' records), z = the mode, w = 0
            GrassBladeSetupRow BladeSetupRows[];
        };

        // The shared pools (GrassRenderer m_InstancesPool / m_IdsPool / m_ArgsPool): the rows, the camera lists' ids
        // (a slot's id is absolute in the rows) and the indexed-indirect records the raster draws.
        ReadBuffer(2) BladeSetupTransforms
        {
            mat4 BladeSetupInstances[];
        };
        ReadBuffer(3) BladeSetupIds
        {
            uint BladeSetupInstanceIds[];
        };
        ReadBuffer(4) BladeSetupArgs
        {
            uint BladeSetupArgWords[];
        };

        // The view's setup records (GrassRenderer m_BladeSetup), one per raster-stream row.
        Buffer(5) BladeSetupRecords
        {
            GrassBladeSetupData BladeSetupRecord[];
        };

        // The view's deformation field (FrameGraphRefs.hpp DeformationFieldReads; System.Black and zero levels where
        // the view has none: every read answers "untouched"), point-sampled as the raster reads it.
        Uniform sampler DeformationFieldSampler;
        Uniform(6) texture2D DeformHeight0;
        Uniform(7) texture2D DeformHeight1;
        Uniform(8) texture2D DeformDetail0;
        Uniform(9) texture2D DeformDetail1;
        ReadBuffer(10) DeformationParams
        {
            ivec4 FieldWindows; // Deformation::DeformationFieldParamsGpu: xy level 0's minimum cell, zw level 1's
            vec4  FieldLevels;  // x, y: texel (cm) of level 0, 1; z, w: their resolution (0: no field)
        };

        // This phase's work (GrassRenderer AddFrameBladeSetup, a graph buffer): words 0-2 the setup's indirect
        // dispatch (VkDispatchIndirectCommand), 3 the drawn blades, then per entry the drawn blades before it.
        Buffer(11) BladeSetupWork
        {
            uint BladeSetupWorkWords[];
        };

        const uint kSetupGroupThreads = 64u; // LocalSize
        const uint kModeWork          = 0u;
        const uint kModeSetup         = 1u;
        const uint kWorkHeaderWords   = 4u;
        const uint kMaxGroupsX        = 65535u; // maxComputeWorkGroupCount floor
        const uint kRecordWords       = 5u;     // VkDrawIndexedIndirectCommand
        const uint kArgsHeaderWords   = 4u;     // GpuInstances.hpp: the records follow a 4-word header

        shared uint s_Scan[64];

        // The row entry @p entry belongs to: the last row whose first entry is at or before it.
        uint RowOfEntry( uint entry )
        {
            uint lo = 0u;
            uint hi = BladeSetupInfo.x;
            while ( hi - lo > 1u )
            {
                const uint mid = ( lo + hi ) / 2u;
                if ( BladeSetupRows[mid].Args.w <= entry )
                    lo = mid;
                else
                    hi = mid;
            }
            return lo;
        }

        // The args word of entry @p entry's record (row @p r): a blade's record per entry, a mesh section's level's
        // first submesh record (every submesh of a level draws the same slots).
        uint RecordWordOf( uint r, uint entry )
        {
            const uint stride = max( BladeSetupRows[r].Args.y, 1u );
            return BladeSetupRows[r].Args.x + kArgsHeaderWords +
                   ( entry - BladeSetupRows[r].Args.w ) * stride * kRecordWords;
        }

        // The slots entry @p entry's record drew: its instanceCount, from its firstInstance (absolute in the ids
        // pool) within the variety's list (Lists.x its first slot, Visibility.w its slots).
        uint EntryDrawn( uint entry )
        {
            const uint r      = RowOfEntry( entry );
            const uint record = RecordWordOf( r, entry );
            const uint count  = BladeSetupArgWords[record + 1u];
            const uint first  = BladeSetupArgWords[record + 4u];
            const uint base   = BladeSetupRows[r].Shape.Lists.x;
            const uint slots  = BladeSetupRows[r].Shape.Visibility.w;
            if ( first < base || first - base >= slots )
                return 0u;
            return min( count, slots - ( first - base ) );
        }

        // kModeWork: the entries' drawn slots prefix-summed (64 a step, a shared-memory scan), the total and the
        // setup's dispatch.
        void BuildWork()
        {
            const uint entries = BladeSetupInfo.y;
            const uint lane    = gl_LocalInvocationID.x;
            uint       carry   = 0u;
            for ( uint c = 0u; c < entries; c += kSetupGroupThreads )
            {
                const uint entry = c + lane;
                const uint drawn = entry < entries ? EntryDrawn( entry ) : 0u;
                s_Scan[lane]     = drawn;
                barrier();
                for ( uint o = 1u; o < kSetupGroupThreads; o <<= 1u )
                {
                    const uint add = lane >= o ? s_Scan[lane - o] : 0u;
                    barrier();
                    s_Scan[lane] += add;
                    barrier();
                }
                if ( entry < entries )
                    BladeSetupWorkWords[kWorkHeaderWords + entry] = carry + s_Scan[lane] - drawn;
                carry += s_Scan[kSetupGroupThreads - 1u];
                barrier();
            }
            if ( lane != 0u )
                return;
            const uint groups = ( carry + kSetupGroupThreads - 1u ) / kSetupGroupThreads;
            const uint x      = min( groups, kMaxGroupsX );
            BladeSetupWorkWords[0] = x;
            BladeSetupWorkWords[1] = x == 0u ? 0u : ( groups + x - 1u ) / x;
            BladeSetupWorkWords[2] = 1u;
            BladeSetupWorkWords[3] = carry;
        }

        // kModeSetup: drawn blade @p blade's entry - the last whose drawn blades before it are at or below it (an
        // entry that drew none shares its successor's sum, so the last such one drew this blade).
        uint EntryOfBlade( uint blade )
        {
            uint lo = 0u;
            uint hi = BladeSetupInfo.y;
            while ( hi - lo > 1u )
            {
                const uint mid = ( lo + hi ) / 2u;
                if ( BladeSetupWorkWords[kWorkHeaderWords + mid] <= blade )
                    lo = mid;
                else
                    hi = mid;
            }
            return lo;
        }

        // FPS-GRASS-MESH-SETUP ("Grass: Mesh Setup"; UE 5 Nanite's instance scene data, computed once per instance
        // and only fetched by the material pass): a drawn mesh grass instance's record, at its slot's first
        // raster-stream row Visibility.x + slot x Visibility.y (rows per instance) - the mesh section's own row, so
        // no blade's record shares it. GrassMeshSetupData's layout (Mesh/Surface/GrassMeshVisibility.glslh) in
        // GrassBladeSetupData's words: Root = the sway now (Common/FoliageWind.glslh FoliageWindInstanceSway at the
        // raster's wind, gust and local wind), Tip = the sway one wind step before (WindB.z, the previous gust and
        // local wind, at the previous row WindB.w further), Before = 0, Frame.x = the LOD level the slot was drawn
        // with (GrassMeshVisibilityLevel's rule over the section's bucket firsts).
        void SetUpMeshInstance( uint r, uint slot, uint id )
        {
            const GrassBladeShapeData shape = BladeSetupRows[r].Shape;
            const uint                row   = shape.Visibility.x + slot * shape.Visibility.y;
            if ( shape.Visibility.y == 0u || row >= uint( BladeSetupRecord.length() ) )
                return;
            const vec4 windA  = BladeSetupRows[r].WindA;
            const vec4 windB  = BladeSetupRows[r].WindB;
            const uint prevId = id + floatBitsToUint( windB.w );
            if ( prevId >= uint( BladeSetupInstances.length() ) )
                return;
            const vec3 origin     = vec3( BladeSetupInstances[id][3] );
            const vec3 prevOrigin = vec3( BladeSetupInstances[prevId][3] );
            uint       level      = 0u;
            for ( uint b = 1u; b < min( shape.Visibility.z, 8u ); ++b )
            {
                if ( slot < shape.BucketFirst[b / 4u][b % 4u] )
                    break;
                level = b;
            }
            GrassBladeSetupData record;
            record.Root   = FoliageWindInstanceSway( origin, windA, windB, cameraUB.WindGust,
                                                     LocalWindXZ( origin, false ) );
            record.Tip    = FoliageWindInstanceSway( prevOrigin, windA, vec4( windB.x, windB.z, 0.0, 0.0 ),
                                                     cameraUB.PrevWindGust, LocalWindXZ( prevOrigin, true ) );
            record.Before = vec4( 0.0 );
            record.Frame  = uvec4( level, 0u, 0u, 0u );
            BladeSetupRecord[row] = record;
        }

        void SetUpBlade()
        {
            const uint group = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
            const uint blade = group * kSetupGroupThreads + gl_LocalInvocationID.x;
            if ( blade >= BladeSetupWorkWords[3] || BladeSetupInfo.y == 0u )
                return;
            const uint entry = EntryOfBlade( blade );
            const uint r     = RowOfEntry( entry );
            const GrassBladeShapeData shape = BladeSetupRows[r].Shape;
            const uint first = BladeSetupArgWords[RecordWordOf( r, entry ) + 4u];
            const uint base  = shape.Lists.x;
            const uint slot  = first - base + ( blade - BladeSetupWorkWords[kWorkHeaderWords + entry] );
            if ( first < base || slot >= shape.Visibility.w )
                return;
            const uint idSlot = base + slot;
            if ( idSlot >= uint( BladeSetupInstanceIds.length() ) )
                return;
            const uint id = BladeSetupInstanceIds[idSlot];
            if ( id >= uint( BladeSetupInstances.length() ) )
                return;
            if ( BladeSetupRows[r].Args.y != 0u )
            {
                SetUpMeshInstance( r, slot, id );
                return;
            }
            const uint row = shape.Visibility.x + slot;
            if ( row >= uint( BladeSetupRecord.length() ) )
                return;

            // The raster's blade (Vertex_GrassBlade.glslh): the field under the root, the local wind at it now and
            // one step before, the curve at the draw row's wind and at its previous step (WindB.z).
            const mat4 model   = BladeSetupInstances[id];
            const vec3 root    = vec3( model[3] );
            const vec4 windA   = BladeSetupRows[r].WindA;
            const vec4 windB   = BladeSetupRows[r].WindB;
            const vec3 trample = DeformationFieldTrampleFrom( DeformHeight0, DeformHeight1, DeformDetail0,
                                                              DeformDetail1, DeformationFieldSampler, FieldWindows,
                                                              FieldLevels, root );
            const BladeCurve now = BladeCurveAt( shape.Size, shape.Bend, model, windA, windB, cameraUB.WindGust,
                                                 LocalWindXZ( root, false ), trample );
            const BladeCurve before =
                 BladeCurveAt( shape.Size, shape.Bend, model, windA, vec4( windB.x, windB.z, 0.0, 0.0 ),
                               cameraUB.PrevWindGust, LocalWindXZ( root, true ), trample );
            BladeSetupRecord[row] = BladeSetupOf( now, before, BladeWidth( shape, model ),
                                                  BladeCardIndex( shape, model[0].w ),
                                                  BladeLevelSegments( shape, slot ), model[2].w );
        }

        void main()
        {
            if ( BladeSetupInfo.z == kModeWork )
                BuildWork();
            else
                SetUpBlade();
        }
    }
}
