// DesertAsset {"Kind":"Shader","Guid":"a1e8438cb2974f95ab07bf109bd52564","Versions":{"SHDR":1},"Dependencies":[]}
// THE GPU-WRITTEN TLAS ROWS (RayTracingSceneRenderer.hpp, RayTracingGpuInstanceRows; UE RayTracingInstanceBufferUtil's
// GPU instance copy): one thread per row a GPU-instanced source reserved. A row under the drawn count becomes a
// VkAccelerationStructureInstanceKHR of the instance the producer generated - its transform the very mat4 the
// raster's vertex stage reads (InstanceTransforms), its count the indexed-indirect record GrassFinalize wrote; a
// row past it is inactive (every word 0: mask 0 and no BLAS, UE does the same).
// DEFORM-RT-TRAMPLE (UE: the ray-tracing geometry takes the same WPO/deformation the raster evaluates, never a copy
// of its own): a grass blade source (PressSteps > 0) reads the view's deformation field at each row's root with the
// raster's own read (Deformation/DeformationFieldTrample.glslh), only inside the field's trampled box. A pressed row
// instances the level's pressed BLAS of the nearest step (GrassBladeRayTracing.hpp) through the affine map that
// takes that blade-space blade onto BladeCurveAt's curve of THIS instance and press (Mesh/Surface/GrassBlade.glslh):
// across = the instance's own width axis, up = its up scaled to the pressed rise, forward = the pressed tip - the
// raster's laid blade but for the curve's power, which is the step's. No BLAS is built for it: the rows move.
Shader "RayTracingGpuInstances"
{
    Compute
    {
        LocalSize(64, 1, 1);

        ReadBuffer(0) Transforms
        {
            mat4 InstanceTransforms[]; // the producer's instances, GpuInstanceRefs::Instances
        };

        ReadBuffer(1) Args
        {
            uint Arguments[]; // GpuInstances.hpp: header, 5 words per indexed-indirect record, 4 per bucket
        };

        // GRASS-VIEW-LAW-1: the producer's ids (GpuInstanceRefs::Ids: per drawn slot its row of Transforms) when
        // ThroughIds is 1 - grass's full list, its rows of Instances (GrassCull.shader CullInstance); else Transforms
        // bound as a stand-in, never read (the slot is the row).
        ReadBuffer(3) Ids
        {
            uint InstanceIds[];
        };

        // The view's deformation field (FrameGraphRefs.hpp DeformationFieldReads; System.Black and zero levels where
        // the view has none: every read answers "untouched").
        Uniform sampler DeformationFieldSampler;
        Uniform(4) texture2D DeformHeight0;
        Uniform(5) texture2D DeformHeight1;
        Uniform(6) texture2D DeformDetail0;
        Uniform(7) texture2D DeformDetail1;
        ReadBuffer(8) DeformationParams
        {
            ivec4 FieldWindows; // Deformation::DeformationFieldParamsGpu: xy level 0's minimum cell, zw level 1's
            vec4  FieldLevels;  // x, y: texel (cm) of level 0, 1; z, w: their resolution (0: no field)
        };

        Buffer(2) TlasRows
        {
            uint TlasRowWords[]; // 16 words per VkAccelerationStructureInstanceKHR
        };

        PushConstant PushConstants
        {
            vec4  BladeSize;     // a blade source's GrassBladeShapeData Size (Height cm, Stiffness, Curvature, cards)
            vec4  BladeBend;     // its Bend (CurvatureSpread, BendSpread, TrampledLength)
            vec4  TrampleWindow; // the field's trampled box: world X, Z minimum (xy), maximum (zw); min > max: none
            uvec4 PressLo;       // pressed BLAS k + 1's device address, low words (step k + 1 of PressSteps)
            uvec4 PressHi;       // and high words
            uint RowBase;      // the source's first row
            uint Rows;         // its reserved rows
            uint Record;       // the record stating the drawn count (instanceCount) and first instance
            uint IndexAndMask; // customIndex:24 | mask:8
            uint SbtAndFlags;  // sbtOffset:24 | flags:8
            uint AddressLo;    // the BLAS device address
            uint AddressHi;
            uint ThroughIds;   // 1: Ids holds each slot's row of Transforms
            uint ArgsWord;     // the source's first word of Arguments (a pooled producer's range)
            uint PressSteps;   // a grass blade source's pressed BLASes (0: not one, its rows are never pressed)
            uint PressSbtStride; // pressed BLAS k + 1's segment run = SbtAndFlags + (k + 1) * PressSbtStride: its
                                 // own records, so a hit on a pressed row reads the pressed blade's normals
        };

        #include <Mesh/Surface/GrassBlade.glslh>
        #include <Deformation/DeformationFieldTrample.glslh>

        // A pressed blade row: @p world the instance's transform is replaced by the fit of the pressed BLAS @p step
        // and its address returned through @p lo, @p hi. False: the row stands (outside the box, no press, or a
        // degenerate fit) and keeps the shared BLAS and its own transform.
        bool PressedRow( inout mat4 world, out uint lo, out uint hi, out uint sbt )
        {
            lo  = 0u;
            hi  = 0u;
            sbt = SbtAndFlags;
            const vec3 root = world[3].xyz;
            if ( PressSteps == 0u || root.x < TrampleWindow.x || root.z < TrampleWindow.y ||
                 root.x > TrampleWindow.z || root.z > TrampleWindow.w )
                return false;
            const vec3 trample = DeformationFieldTrampleFrom( DeformHeight0, DeformHeight1, DeformDetail0,
                                                              DeformDetail1, DeformationFieldSampler, FieldWindows,
                                                              FieldLevels, root );
            if ( trample.z <= 0.0 )
                return false;
            const uint steps = min( PressSteps, 4u );
            const uint step  = clamp( uint( round( trample.z * float( steps ) ) ), 1u, steps );
            // The instance's curve at its own press (no wind: the TLAS is static) and the pressed BLAS's in blade
            // space (BladeCurveAt of the ray tracer's identity instance, press step / steps along +Z).
            const BladeCurve here = BladeCurveAt( BladeSize, BladeBend, world, vec4( 0.0 ), vec4( 0.0 ), vec4( 0.0 ),
                                                  vec2( 0.0 ), trample );
            const BladeCurve blas = BladeCurveAt(
                 BladeSize, BladeBend, GrassBladeRayTracingModel(), vec4( 0.0 ), vec4( 0.0 ), vec4( 0.0 ),
                 vec2( 0.0 ), vec3( 0.0, 0.0, GrassBladeRayTracingPress( step, steps ) ) );
            if ( blas.Tip.z < 1e-4 || blas.Rise < 1e-4 )
                return false;
            world[1] = vec4( here.Up * ( here.Rise / blas.Rise ), 0.0 );
            world[2] = vec4( here.Tip / blas.Tip.z, 0.0 );
            lo       = PressLo[step - 1u];
            hi       = PressHi[step - 1u];
            sbt      = SbtAndFlags + step * PressSbtStride;
            return ( lo | hi ) != 0u;
        }

        void main()
        {
            uint i = gl_GlobalInvocationID.x;
            if ( i >= Rows )
                return;
            uint row    = ( RowBase + i ) * 16u;
            uint record = ArgsWord + 4u + Record * 5u;
            if ( i >= Arguments[record + 1u] )
            {
                for ( uint w = 0u; w < 16u; ++w )
                    TlasRowWords[row + w] = 0u;
                return;
            }
            uint slot  = Arguments[record + 4u] + i;
            uint at    = ThroughIds != 0u ? InstanceIds[slot] : slot;
            mat4 world = InstanceTransforms[at];
            uint addressLo = AddressLo;
            uint addressHi = AddressHi;
            mat4 pressed   = world;
            uint sbt       = SbtAndFlags;
            uint pressedLo, pressedHi, pressedSbt;
            if ( PressedRow( pressed, pressedLo, pressedHi, pressedSbt ) )
            {
                world     = pressed;
                addressLo = pressedLo;
                addressHi = pressedHi;
                sbt       = pressedSbt;
            }
            for ( uint r = 0u; r < 3u; ++r )
                for ( uint c = 0u; c < 4u; ++c )
                    TlasRowWords[row + r * 4u + c] = floatBitsToUint( world[c][r] ); // row-major 3x4
            TlasRowWords[row + 12u] = IndexAndMask;
            TlasRowWords[row + 13u] = sbt;
            TlasRowWords[row + 14u] = addressLo;
            TlasRowWords[row + 15u] = addressHi;
        }
    }
}
