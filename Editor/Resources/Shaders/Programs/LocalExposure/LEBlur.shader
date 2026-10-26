// DesertAsset {"Kind":"Shader","Guid":"61dfa85e05a04bc99a9a13fbfe09d761","Versions":{"SHDR":1},"Dependencies":[]}
Shader "LEBlur"
{
    Compute
    {
        // LOCAL EXPOSURE BLUR — one direction of the separable Gaussian over the 1/32 log luminance (UE
        // AddGaussianBlurPass, PostProcessLocalExposure.cpp:292-303 -> PostProcessWeightedSampleSum.cpp). UE's
        // weights exp(-16.7 (x/R)^2) (:94-115), normalised; mirrored addressing (UE UseMirrorAddressMode) by
        // integer texelFetch, so no sampler state; the radius is NOT clamped to 31 taps (CONTRACT.md §6 item 4).

        LocalSize(8, 8, 1);

        Uniform(0) texture2D InputTexture;
        Uniform sampler PointClampSampler;
        #define InputTexture sampler2D(InputTexture, PointClampSampler)
        layout(binding = 1, r32f) writeonly uniform image2D OutputTexture;

        PushConstant PushConstants
        {
            ivec2 Direction; // (1, 0) horizontal, (0, 1) vertical
            float Radius;    // R in texels (LocalExposureBlurRadius)
            float Pad0;
        };

        int Mirror( int i, int size )
        {
            const int period = 2 * size;
            int m = i % period;
            if ( m < 0 )
                m += period;
            return m < size ? m : period - 1 - m;
        }

        void main()
        {
            const ivec2 size  = textureSize( InputTexture, 0 );
            const ivec2 coord = ivec2( gl_GlobalInvocationID.xy );
            if ( coord.x >= size.x || coord.y >= size.y )
                return;
            const int axisSize = Direction.x != 0 ? size.x : size.y;
            const int taps     = int( ceil( Radius ) );
            float sum    = 0.0;
            float weight = 0.0;
            for ( int x = -taps; x <= taps; ++x )
            {
                const float u = Radius > 0.0 ? float( x ) / Radius : 0.0;
                const float w = exp( -16.7 * u * u );
                ivec2 p = coord + Direction * x;
                if ( Direction.x != 0 )
                    p.x = Mirror( p.x, axisSize );
                else
                    p.y = Mirror( p.y, axisSize );
                sum += texelFetch( InputTexture, p, 0 ).r * w;
                weight += w;
            }
            imageStore( OutputTexture, coord, vec4( sum / max( weight, 1.0e-6 ), 0.0, 0.0, 0.0 ) );
        }
    }
}
