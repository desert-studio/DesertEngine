// DesertAsset {"Kind":"Shader","Guid":"f51f870417d4453ba24b59e9c7a9a967","Versions":{"SHDR":1},"Dependencies":[]}
Shader "WaterFFTFinalize"
{
    Compute
    {
        // THE OCEAN'S CASCADES AS THE WATER MESH READS THEM (WATER-RIVERS-FFT): the two inverse-transformed packed
        // spectra (WaterFFT.shader, rows then columns) unpacked into one 256 x 2816 half of a persistent rgba16f
        // atlas of 256 x 5632 texels (Mesh/Surface/WaterMeshTiles.glslh WaterFFTTexture); the rows below are
        // relative to that half (Params.Shape.z):
        //   rows [256 c, 256 c + 256),       cascade c: xyz = the displacement (lambda Dx, Dy, lambda Dz), cm;
        //                                               w = lambda dDx/dz (the Jacobian's cross term);
        //   rows [256 (3 + c), 256 (4 + c)), cascade c: xy = the height's slopes (dDy/dx, dDy/dz),
        //                                               zw = lambda dDx/dx, lambda dDz/dz.
        //   rows [256 * 6, 256 * 7),         THE FOAM HISTORY: xyz = cascade 0..2's compression max(1 - J_c, 0),
        //                                               kept as the max of now and the previous half's decayed
        //                                               by Params.Foam.x (the accumulate-and-decay foam buffer of
        //                                               AAA FFT oceans; written by the z = 0 invocation alone, so
        //                                               no two invocations store one texel); w unused;
        //   rows [256 * 7, 256 * 11),        the surf foam (WaterSurfFoam.shader; not written here).
        // The water sums the cascades and forms the normal from the sums (Tessendorf 2001 sec. 3.4); its whitecaps
        // read the history band, so the foam trails behind the crest that folded.
        #include <Water/WaterFFT.glslh>

        layout(binding = 0, rgba32f) restrict readonly uniform image2DArray WaterFFTResultA;
        layout(binding = 1, rgba32f) restrict readonly uniform image2DArray WaterFFTResultB;
        // Read and written: the previous half's foam band is read, this half's bands written (disjoint texels).
        layout(binding = 2, rgba16f) restrict uniform image2D WaterFFTWaves;

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

            // This frame's half of the persistent two-frame atlas (Params.Shape.z = its first band, 0 or 6): the
            // other half keeps the previous frame for the surface's velocity (WaterMeshTiles.glslh).
            const int base = int(Params.Shape.z + 0.5);
            imageStore(WaterFFTWaves, ivec2(id.x, id.y + (base + id.z) * WATER_FFT_SIZE),
                       vec4(lambda * a.x, a.y, lambda * a.z, lambda * a.w));
            if ((id.x % WATER_FFT_READBACK_STRIDE) == 0 && (id.y % WATER_FFT_READBACK_STRIDE) == 0)
            {
                const int rx = id.x / WATER_FFT_READBACK_STRIDE;
                const int rz = id.y / WATER_FFT_READBACK_STRIDE;
                WaterFFTReadback[(id.z * WATER_FFT_READBACK_SIZE + rz) * WATER_FFT_READBACK_SIZE + rx] =
                    vec4(lambda * a.x, a.y, lambda * a.z, 0.0);
            }
            imageStore(WaterFFTWaves, ivec2(id.x, id.y + (base + WATER_FFT_CASCADES + id.z) * WATER_FFT_SIZE),
                       vec4(b.x, b.y, lambda * b.z, lambda * b.w));

            // THE FOAM HISTORY, one texel for the three cascades: each cascade's own Jacobian
            // J_c = (1 + lambda dDx/dx)(1 + lambda dDz/dz) - (lambda dDx/dz)^2 over its own (periodic) patch.
            if (id.z != 0)
                return;
            vec3 compression = vec3(0.0);
            for (int c = 0; c < WATER_FFT_CASCADES; ++c)
            {
                const vec4  ac = imageLoad(WaterFFTResultA, ivec3(id.xy, c)) * flip;
                const vec4  bc = imageLoad(WaterFFTResultB, ivec3(id.xy, c)) * flip;
                const float jc = (1.0 + lambda * bc.z) * (1.0 + lambda * bc.w) - lambda * ac.w * lambda * ac.w;
                compression[c] = max(1.0 - jc, 0.0);
            }
            vec3 kept = vec3(0.0);
            if (Params.Foam.y > 0.5)
            {
                const int previous = int(Params.Foam.y - 0.5);
                kept = imageLoad(WaterFFTWaves, ivec2(id.x, id.y + (previous + WATER_FFT_FOAM) * WATER_FFT_SIZE)).xyz *
                       Params.Foam.x;
            }
            imageStore(WaterFFTWaves, ivec2(id.x, id.y + (base + WATER_FFT_FOAM) * WATER_FFT_SIZE),
                       vec4(max(kept, compression), 0.0));
        }
    }
}
