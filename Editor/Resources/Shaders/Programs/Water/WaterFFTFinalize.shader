// DesertAsset {"Kind":"Shader","Guid":"f51f870417d4453ba24b59e9c7a9a967","Versions":{"SHDR":1},"Dependencies":[]}
Shader "WaterFFTFinalize"
{
    Compute
    {
        // THE OCEAN'S CASCADES AS THE WATER READS THEM: the two inverse-transformed packed spectra (WaterFFT.shader,
        // rows then columns) unpacked into the first mip of the waves' array (WaterFFT.glslh WATER_WAVES_*). The
        // water sums the cascades and forms the normal from the sums (Tessendorf 2001 sec. 3.4).
        #include <Water/WaterFFT.glslh>

        layout(binding = 0, rgba32f) restrict readonly uniform image2DArray WaterFFTResultA;
        layout(binding = 1, rgba32f) restrict readonly uniform image2DArray WaterFFTResultB;
        // Read and written: the displacement the array holds becomes the previous frame's.
        layout(binding = 2, rgba16f) restrict uniform image2DArray WaterFFTWaves;

        Buffer(3) WaterFFTParamsBuffer
        {
            WaterFFTParams Params;
        };

        // THE CPU MIRROR's read-back (WATER-RIVERS-FFT pass 7; C++ twin Water::kFFTReadbackSize /
        // kFFTReadbackTexels, Engine/Water/FFTSurfaceMirror.hpp): every WATER_FFT_READBACK_STRIDE-th texel's
        // displacement, cascade-major then row-major over (z, x), extracted host-readable and read frames later.
        #define WATER_FFT_READBACK_SIZE 64
        #define WATER_FFT_READBACK_STRIDE (WATER_FFT_SIZE / WATER_FFT_READBACK_SIZE)
        Buffer(4) WaterFFTReadbackBuffer
        {
            vec4 WaterFFTReadback[WATER_FFT_CASCADES * WATER_FFT_READBACK_SIZE * WATER_FFT_READBACK_SIZE];
        };

        LocalSize(8, 8, 1);

        void main()
        {
            const ivec3 id = ivec3(gl_GlobalInvocationID);
            if (id.x >= WATER_FFT_SIZE || id.y >= WATER_FFT_SIZE || id.z >= WATER_FFT_CASCADES)
                return;

            // The spectrum's zero sits at texel N/2 on both axes: the transform carries (-1)^x (-1)^y.
            const float flip   = ((id.x + id.y) & 1) == 0 ? 1.0 : -1.0;
            const vec4  a      = imageLoad(WaterFFTResultA, id) * flip; // Dx, Dy, Dz, dDx/dz
            const vec4  b      = imageLoad(WaterFFTResultB, id) * flip; // dDy/dx, dDy/dz, dDx/dx, dDz/dz
            const float lambda = Params.Shape.x;

            const vec4  d  = vec4(lambda * a.x, a.y, lambda * a.z, lambda * a.w);
            const vec4  s  = vec4(b.x, b.y, lambda * b.z, lambda * b.w);
            const ivec3 at = ivec3(id.xy, WATER_WAVES_DISPLACEMENT + id.z);
            if (Params.Shape.w < 1.5)
                imageStore(WaterFFTWaves, ivec3(id.xy, WATER_WAVES_PREVIOUS + id.z),
                           Params.Shape.w > 0.5 ? imageLoad(WaterFFTWaves, at) : d);
            imageStore(WaterFFTWaves, at, d);
            imageStore(WaterFFTWaves, ivec3(id.xy, WATER_WAVES_SLOPES + id.z), s);
            imageStore(WaterFFTWaves, ivec3(id.xy, WATER_WAVES_MOMENTS + id.z), vec4(s.xy * s.xy, 0.0, 0.0));
            if ((id.x % WATER_FFT_READBACK_STRIDE) == 0 && (id.y % WATER_FFT_READBACK_STRIDE) == 0)
            {
                const int rx = id.x / WATER_FFT_READBACK_STRIDE;
                const int rz = id.y / WATER_FFT_READBACK_STRIDE;
                WaterFFTReadback[(id.z * WATER_FFT_READBACK_SIZE + rz) * WATER_FFT_READBACK_SIZE + rx] =
                    vec4(lambda * a.x, a.y, lambda * a.z, 0.0);
            }
        }
    }
}
