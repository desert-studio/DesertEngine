// DesertAsset {"Kind":"Shader","Guid":"45b47d20a7974d9eb7bcb2b6d531f624","Versions":{"SHDR":1},"Dependencies":[]}
// THE BLADE SETUP (FPS-GRASS-RESOLVE-2; Ghost of Tsushima, GDC 2021 "Procedural Grass in Ghost of Tsushima": a
// compute pass evaluates each visible blade ONCE into a compact buffer, and the raster and the shading read it
// instead of deriving it again per pixel). After the camera's lists are final (phase 1's records, and phase 2's when
// "Grass: Post-Occlusion" ran), one thread per slot of a blade variety's camera list that a record drew evaluates
// that blade's curve now and one wind step before (Mesh/Surface/GrassBlade.glslh BladeCurveAt - the raster's one
// statement of it, with the raster's wind, local wind, gust and deformation field read at the root), its strip
// width, its card and its strip level, into the blade's row of the view's setup records (GrassBladeSetupData,
// indexed by the raster-stream row Visibility.x + slot, the row the visibility word names).
// "Grass: Visibility Resolve" (Mesh/Surface/Pass_VisibilityResolve.glslh) reads that record per pixel in place of
// the transform, the ids, the deformation fetch, both local wind loops and the two BladeCurveAt per corner.
// A slot no record drew this frame is not written: no word names its row.
// Dispatch: x = slots, kSetupGroupThreads a group (BladeSetupInfo.y groups cover the largest variety's list),
// y = the table's rows (GrassRenderer::AddFrameBladeSetup).
Shader "GrassBladeSetup"
{
    Compute
    {
        LocalSize(64, 1, 1);

        // The camera block (Common/CameraUB.glslh, ShaderProtocols::MakeCameraUB of the view's frame): the gust and
        // the local wind sources, now and one step before - the very block the raster's vertex stage read.
        #include <Common/LocalWind.glslh>
        #include <Mesh/Surface/GrassBlade.glslh>
        #include <Deformation/DeformationFieldTrample.glslh>

        // Mirrors Graphic::System::GrassBladeSetupRowGpu (GrassRenderer.cpp), std430: a blade variety's shape (the
        // GrassBladeShapeData its cells read, without the hull), its view wind (Graphic::PackViewInstanceWind, the
        // raster's draw row's WindA / WindB) and where its camera records lie.
        struct GrassBladeSetupRow
        {
            GrassBladeShapeData Shape;
            vec4                WindA;
            vec4                WindB;
            // x = the camera list's records' first word in BladeSetupArgs (VarietyGpu::CameraArgs), y = phase 2's
            // (VarietyGpu::PostArgs; kNoPostArgs: phase 2 drew nothing this frame), z = the records (one per strip
            // level), w = 0.
            uvec4 Args;
        };

        ReadBuffer(1) BladeSetupTable
        {
            uvec4              BladeSetupInfo; // x = rows, y = groups along x, zw = 0
            GrassBladeSetupRow BladeSetupRows[];
        };

        // The shared pools (GrassRenderer m_InstancesPool / m_IdsPool / m_ArgsPool): the rows, the camera lists' ids
        // (a slot's id is absolute in the rows) and the indexed-indirect records the raster drew.
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

        const uint kSetupGroupThreads = 64u; // LocalSize
        const uint kNoPostArgs        = 0xFFFFFFFFu;
        const uint kRecordWords       = 5u; // VkDrawIndexedIndirectCommand
        const uint kArgsHeaderWords   = 4u; // GpuInstances.hpp: the records follow a 4-word header

        // Whether a record of the list at word @p at of the args (@p records records) drew slot @p slot of a
        // list whose first slot in the ids pool is @p base (a record's firstInstance is absolute in the pool).
        bool SlotDrawn( uint at, uint records, uint base, uint slot )
        {
            for ( uint s = 0u; s < records; ++s )
            {
                const uint record = at + kArgsHeaderWords + s * kRecordWords;
                const uint count  = BladeSetupArgWords[record + 1u];
                const uint first  = BladeSetupArgWords[record + 4u];
                if ( first < base )
                    continue;
                const uint lo = first - base;
                if ( slot >= lo && slot - lo < count )
                    return true;
            }
            return false;
        }

        void main()
        {
            const uint r = gl_WorkGroupID.y;
            if ( r >= BladeSetupInfo.x )
                return;
            const GrassBladeShapeData shape = BladeSetupRows[r].Shape;
            const uint                slot  = gl_GlobalInvocationID.x;
            if ( slot >= shape.Visibility.w )
                return;
            const uvec4 args = BladeSetupRows[r].Args;
            const uint  base = shape.Lists.x;
            if ( !SlotDrawn( args.x, args.z, base, slot ) &&
                 ( args.y == kNoPostArgs || !SlotDrawn( args.y, args.z, base, slot ) ) )
                return;
            const uint idSlot = base + slot;
            if ( idSlot >= uint( BladeSetupInstanceIds.length() ) )
                return;
            const uint id = BladeSetupInstanceIds[idSlot];
            if ( id >= uint( BladeSetupInstances.length() ) )
                return;
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
    }
}
