// DesertAsset {"Kind":"Shader","Guid":"f51f870417d4453ba24b59e9c7a9a967","Versions":{"SHDR":1},"Dependencies":[]}
Shader "WaterFFTFinalize"
{
    Compute
    {
        // THE OCEAN'S CASCADES AS THE WATER MESH READS THEM (WATER-RIVERS-FFT): the two inverse-transformed packed
        // spectra (WaterFFT.shader, rows then columns) unpacked into one rgba16f atlas of 256 x 1536 texels
        // (Mesh/Surface/WaterMeshTiles.glslh WaterFFTTexture):
        //   rows [256 c, 256 c + 256),       cascade c: xyz = the displacement (lambda Dx, Dy, lambda Dz), cm;
        //                                               w = lambda dDx/dz (the Jacobian's cross term);
        //   rows [256 (3 + c), 256 (4 + c)), cascade c: xy = the height's slopes (dDy/dx, dDy/dz),
        //                                               zw = lambda dDx/dx, lambda dDz/dz.
        // The water sums the cascades and forms the normal and the Jacobian from the sums (Tessendorf 2001 sec. 3.4:
        // J = (1 + Jxx)(1 + Jzz) - Jxz^2), so the whitecaps come from where the summed sea folds.
        #include <Water/WaterFFT.glslh>

        layout(binding = 0, rgba32f) restrict readonly uniform image2DArray WaterFFTResultA;
        layout(binding = 1, rgba32f) restrict readonly uniform image2DArray WaterFFTResultB;
        layout(binding = 2, rgba16f) restrict writeonly uniform image2D WaterFFTWaves;

        Buffer(3) WaterFFTParamsBuffer
        {
            WaterFFTParams Params;
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

            imageStore(WaterFFTWaves, ivec2(id.x, id.y + id.z * WATER_FFT_SIZE),
                       vec4(lambda * a.x, a.y, lambda * a.z, lambda * a.w));
            imageStore(WaterFFTWaves, ivec2(id.x, id.y + (WATER_FFT_CASCADES + id.z) * WATER_FFT_SIZE),
                       vec4(b.x, b.y, lambda * b.z, lambda * b.w));
        }
    }
}
