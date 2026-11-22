// DesertAsset {"Kind":"Shader","Guid":"77bb0e20e4fb4d82804f14a83523b9da","Versions":{"SHDR":1},"Dependencies":[]}
Shader "BloomConvolutionPack"
{
    Compute
    {
        // Convolution bloom: the bloom input (the chain's mip 0 — the half-resolution scene, thresholded, Karis
        // averaged and locally exposed by BloomDownsample's first pass, so both methods start from the same image)
        // into the spectrum image's layout (Common/BloomFFT.glslh), resampled onto the Content rectangle at the
        // origin and ZERO everywhere else: the zero border is what keeps a bright edge's kernel from wrapping round
        // to the opposite side (UE BloomConvolutionBufferScale).
        Uniform sampler LinearClampSampler;
        Uniform(0) texture2D BloomInput;
        #define BloomInput sampler2D(BloomInput, LinearClampSampler)
        layout(binding = 1, rgba32f) writeonly uniform image2D Spectrum;

        PushConstant PushConstants
        {
            vec2 ContentSize; // the content rectangle, spectrum texels (<= N)
            int  Width;       // N.x
            int  Height;      // N.y
        };

        LocalSize(16, 16, 1);

        void main()
        {
            ivec2 p = ivec2( gl_GlobalInvocationID.xy );
            if ( p.x >= Width || p.y >= Height )
                return;
            vec3 c = vec3( 0.0 );
            if ( float( p.x ) < ContentSize.x && float( p.y ) < ContentSize.y )
                c = textureLod( BloomInput, ( vec2( p ) + 0.5 ) / ContentSize, 0.0 ).rgb;
            imageStore( Spectrum, p, vec4( c.r, 0.0, c.g, 0.0 ) );
            imageStore( Spectrum, ivec2( p.x + Width, p.y ), vec4( c.b, 0.0, 0.0, 0.0 ) );
        }
    }
}
