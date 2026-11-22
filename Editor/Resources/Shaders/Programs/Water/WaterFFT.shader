// DesertAsset {"Kind":"Shader","Guid":"a1d79d40346f4f74935c78335fbf1817","Versions":{"SHDR":1},"Dependencies":[]}
Shader "WaterFFT"
{
    Compute
    {
        // ONE PASS OF THE OCEAN'S 2D INVERSE FFT (WATER-RIVERS-FFT): every row (Params.Shape.y = 0) or every
        // column (1) of every cascade of the two packed spectra, transformed in one workgroup of 256 invocations
        // each - a radix-2 decimation-in-time Cooley-Tukey over shared memory (bit-reversed load, log2(256) = 8
        // ping-ponged butterfly stages, e^(+i...) twiddles of the inverse transform, no 1/N: Tessendorf's sum).
        // Run twice per frame (rows, then columns; Graphic::System::WaterRenderer::AddWavePasses), source and
        // destination distinct so no line reads another's output. The zero wave number sits at texel N/2: the
        // (-1)^(x + y) that shift leaves on the result is taken out by WaterFFTFinalize.
        #include <Water/WaterFFT.glslh>

        layout(binding = 0, rgba32f) restrict readonly uniform image2DArray WaterFFTSourceA;
        layout(binding = 1, rgba32f) restrict readonly uniform image2DArray WaterFFTSourceB;
        layout(binding = 2, rgba32f) restrict writeonly uniform image2DArray WaterFFTTargetA;
        layout(binding = 3, rgba32f) restrict writeonly uniform image2DArray WaterFFTTargetB;

        Buffer(4) WaterFFTParamsBuffer
        {
            WaterFFTParams Params;
        };

        LocalSize(256, 1, 1);

        // Two ping-pong lines of both packed spectra: 2 x 2 x 256 x 16 B = 16 KiB of shared memory.
        shared vec4 sA[2][WATER_FFT_SIZE];
        shared vec4 sB[2][WATER_FFT_SIZE];

        // Both complex numbers of a texel times one twiddle.
        vec4 MulPair(vec2 w, vec4 z)
        {
            return vec4(WaterFFTMul(w, z.xy), WaterFFTMul(w, z.zw));
        }

        void main()
        {
            const int   t       = int(gl_LocalInvocationID.x);
            const int   line    = int(gl_WorkGroupID.y);
            const int   cascade = int(gl_WorkGroupID.z);
            const bool  columns = Params.Shape.y > 0.5;
            const ivec3 texel   = columns ? ivec3(line, t, cascade) : ivec3(t, line, cascade);

            // Decimation in time reads its input in bit-reversed order.
            const int reversed = int(bitfieldReverse(uint(t)) >> uint(32 - WATER_FFT_LOG2));
            sA[0][reversed]    = imageLoad(WaterFFTSourceA, texel);
            sB[0][reversed]    = imageLoad(WaterFFTSourceB, texel);
            barrier();

            int src = 0;
            for (int stage = 0; stage < WATER_FFT_LOG2; ++stage)
            {
                const int   halfSize   = 1 << stage;
                const int   j      = t & (2 * halfSize - 1); // position inside this stage's butterfly group
                const bool  upper  = j >= halfSize;
                const int   even   = upper ? t - halfSize : t;
                const int   odd    = even + halfSize;
                const float angle  = WATER_FFT_TWO_PI * float(upper ? j - halfSize : j) / float(2 * halfSize);
                const vec2  w      = vec2(cos(angle), sin(angle)); // inverse transform: e^(+i angle)
                const float flip   = upper ? -1.0 : 1.0;
                sA[1 - src][t]     = sA[src][even] + flip * MulPair(w, sA[src][odd]);
                sB[1 - src][t]     = sB[src][even] + flip * MulPair(w, sB[src][odd]);
                src                = 1 - src;
                barrier();
            }

            imageStore(WaterFFTTargetA, texel, sA[src][t]);
            imageStore(WaterFFTTargetB, texel, sB[src][t]);
        }
    }
}
