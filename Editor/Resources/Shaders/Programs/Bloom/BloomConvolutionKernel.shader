// DesertAsset {"Kind":"Shader","Guid":"5482561a2ac442e7af5491ed8c62e462","Versions":{"SHDR":1},"Dependencies":[]}
Shader "BloomConvolutionKernel"
{
    Compute
    {
        // Convolution bloom: the kernel image (the post-process volume's BloomConvolutionKernel) placed into the
        // spectrum layout (Common/BloomFFT.glslh) with its CENTRE AT TEXEL (0, 0) and the rest wrapped round the
        // edges, so convolving with it does not shift the image. Its larger side spans KernelSize spectrum texels
        // (BloomConvolutionScale x the content's minor axis, UE BloomConvolutionSize), aspect kept. Runs only when the
        // kernel, its scale or the spectrum size changes; two BloomFFT passes then turn it into its spectrum.
        Uniform sampler LinearClampSampler;
        Uniform(0) texture2D KernelImage;
        #define KernelImage sampler2D(KernelImage, LinearClampSampler)
        layout(binding = 1, rgba32f) writeonly uniform image2D Spectrum;

        PushConstant PushConstants
        {
            vec2  KernelSize; // the kernel image's footprint, spectrum texels (x, y)
            float KernelLod;  // the mip whose texels match a spectrum texel (log2 of the minification)
            int   Width;      // N.x
            int   Height;     // N.y
        };

        LocalSize(16, 16, 1);

        void main()
        {
            ivec2 p = ivec2( gl_GlobalInvocationID.xy );
            if ( p.x >= Width || p.y >= Height )
                return;
            // Signed offset from the kernel centre, wrapped: texel N - 1 is offset -1.
            vec2 d  = vec2( p.x < Width / 2 ? p.x : p.x - Width, p.y < Height / 2 ? p.y : p.y - Height );
            vec2 uv = vec2( 0.5 ) + d / max( KernelSize, vec2( 1.0 ) );
            vec3 k  = vec3( 0.0 );
            if ( all( greaterThanEqual( uv, vec2( 0.0 ) ) ) && all( lessThanEqual( uv, vec2( 1.0 ) ) ) )
                k = max( textureLod( KernelImage, uv, KernelLod ).rgb, vec3( 0.0 ) );
            imageStore( Spectrum, p, vec4( k.r, 0.0, k.g, 0.0 ) );
            imageStore( Spectrum, ivec2( p.x + Width, p.y ), vec4( k.b, 0.0, 0.0, 0.0 ) );
        }
    }
}
