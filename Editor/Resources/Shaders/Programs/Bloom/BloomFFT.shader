// DesertAsset {"Kind":"Shader","Guid":"f4e61a38fba740a8ae66d98489810658","Versions":{"SHDR":1},"Dependencies":[]}
Shader "BloomFFT"
{
    Compute
    {
        // Convolution bloom (UE PostProcessFFTBloom): ONE 1D FFT over every line of the spectrum image, in place. A
        // workgroup is a line (a row when Axis is 0, a column when 1); it transforms both of the line's groups (RG,
        // then B) — the layout is Common/BloomFFT.glslh's. Used for the frame's forward and inverse row passes and
        // for the kernel's two forward passes; the frame's column pass is BloomConvolve (forward, multiply, inverse
        // in one load).
        layout(binding = 0, rgba32f) uniform image2D Spectrum;

        PushConstant PushConstants
        {
            int   LineLength;  // N along Axis (a power of two, <= kBloomFFTMaxLength)
            int   Log2Length;  // log2(LineLength)
            int   Axis;        // 0: rows, 1: columns
            float Direction;   // -1 forward, +1 inverse
            int   Width;       // N.x: the B group's x offset
        };

        #include <Common/BloomFFT.glslh>

        LocalSize(256, 1, 1);

        void main()
        {
            uint line = gl_WorkGroupID.x;
            for ( uint group = 0u; group < 2u; ++group )
            {
                BloomFFTLoad( line, group, uint( Axis ), uint( LineLength ), uint( Log2Length ), uint( Width ) );
                BloomFFTButterflies( uint( LineLength ), Direction );
                BloomFFTStore( line, group, uint( Axis ), uint( LineLength ), uint( Width ) );
            }
        }
    }
}
