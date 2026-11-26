// DesertAsset {"Kind":"Shader","Guid":"1ae6f54688bd44699dc5f8c085aafca6","Versions":{"SHDR":1},"Dependencies":[]}
Shader "BloomConvolve"
{
    Compute
    {
        // Convolution bloom (UE PostProcessFFTBloom's "convolve with kernel" pass): one workgroup per COLUMN of the
        // row-transformed frame spectrum. It finishes the forward 2D FFT down the column, multiplies by the kernel's
        // 2D spectrum, and runs the inverse FFT back down the column — one load and one store for three steps. The
        // inverse row pass (BloomFFT, Axis 0, Direction +1) then returns the image.
        //
        // KERNEL NORMALISATION: the kernel's DC bin (KernelSpectrum texel (0, 0) and (N.x, 0)) is the sum of each of
        // its channels. The product is divided by ONE scalar, the luminance of those three sums, so the bloom
        // carries the energy of its source and the kernel's tint survives (a per-channel division would grey it).
        layout(binding = 0, rgba32f) uniform image2D Spectrum;
        layout(binding = 1, rgba32f) readonly uniform image2D KernelSpectrum;

        PushConstant PushConstants
        {
            int LineLength; // N.y
            int Log2Length; // log2(N.y)
            int Width;      // N.x
        };

        #include <Common/BloomFFT.glslh>

        LocalSize(256, 1, 1);

        void main()
        {
            uint  column = gl_WorkGroupID.x;
            uint  n      = uint( LineLength );
            uint  logN   = uint( Log2Length );
            uint  width  = uint( Width );

            vec4  dcRG   = imageLoad( KernelSpectrum, ivec2( 0, 0 ) );
            vec4  dcB    = imageLoad( KernelSpectrum, ivec2( int( width ), 0 ) );
            float sum    = dot( vec3( dcRG.x, dcRG.z, dcB.x ), vec3( 0.2126, 0.7152, 0.0722 ) );
            float norm   = sum > 1e-20 ? 1.0 / sum : 0.0;

            for ( uint group = 0u; group < 2u; ++group )
            {
                BloomFFTLoad( column, group, 1u, n, logN, width );
                BloomFFTButterflies( n, -1.0 );
                for ( uint i = gl_LocalInvocationID.x; i < n; i += kBloomFFTThreads )
                {
                    vec4 k          = imageLoad( KernelSpectrum, BloomFFTTexel( column, i, group, 1u, width ) );
                    BloomFFTLine[i] = BloomFFTMulPairs( BloomFFTLine[i], k ) * norm;
                }
                barrier();
                BloomFFTBitReverse( n, logN );
                BloomFFTButterflies( n, 1.0 );
                BloomFFTStore( column, group, 1u, n, width );
            }
        }
    }
}
