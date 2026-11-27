// DesertAsset {"Kind":"Shader","Guid":"8371cca7a8924d7ba1fee90fcb2c6071","Versions":{"SHDR":1},"Dependencies":[]}
Shader "BloomGaussianBlur"
{
    Compute
    {
        // Ported from UE 5.8 FilterPixelShader.usf / PostProcessWeightedSampleSum.cpp (AddGaussianBlurPass),
        // adapted: compute, the kernel in a buffer. One axis of one bloom stage's Gaussian: the bilinear-paired taps
        // BloomGaussian::Compute1DKernel built (offset in texels of this stage, normalised weight) along Direction.
        // The horizontal pass writes the blur; the vertical pass multiplies it by the stage's tint and adds the
        // previous (coarser) stage's result read at the same UV (UE USE_COMBINE_ADDITIVE).
        Uniform sampler LinearClampSampler;
        Uniform(0) texture2D SourceTexture;
        #define SourceTexture sampler2D(SourceTexture, LinearClampSampler)
        // The coarser stage's result (vertical pass), or System.Black with UseAdditive 0.
        Uniform(1) texture2D AdditiveTexture;
        #define AdditiveTexture sampler2D(AdditiveTexture, LinearClampSampler)
        layout(binding = 2, rgba16f) writeonly uniform image2D DestinationTexture;
        // BloomGaussian::Kernel::OffsetWeight: x = offset (texels), y = weight; SampleCount of them are used.
        Buffer(3) KernelBuffer
        {
            vec4 OffsetWeight[32];
        };

        PushConstant PushConstants
        {
            vec4 Tint;        // rgb: the stage's tint (vertical pass), 1 on the horizontal pass
            vec2 Direction;   // one texel of the source along the blurred axis, UV
            int  SampleCount; // taps in KernelBuffer (<= 32)
            int  UseAdditive; // 1: add AdditiveTexture
        };

        LocalSize(16, 16, 1);

        void main()
        {
            ivec2 dstSize  = imageSize( DestinationTexture );
            ivec2 dstCoord = ivec2( gl_GlobalInvocationID.xy );
            if ( dstCoord.x >= dstSize.x || dstCoord.y >= dstSize.y )
                return;

            vec2 uv  = ( vec2( dstCoord ) + 0.5 ) / vec2( dstSize );
            vec3 sum = vec3( 0.0 );
            for ( int i = 0; i < SampleCount; ++i )
            {
                vec4 tap = OffsetWeight[i];
                sum += textureLod( SourceTexture, uv + Direction * tap.x, 0.0 ).rgb * tap.y;
            }
            sum *= Tint.rgb;
            if ( UseAdditive == 1 )
                sum += textureLod( AdditiveTexture, uv, 0.0 ).rgb;
            imageStore( DestinationTexture, dstCoord, vec4( sum, 1.0 ) );
        }
    }
}
