// DesertAsset {"Kind":"Shader","Guid":"5db9fc616bff4753b59e271605149886","Versions":{"SHDR":1},"Dependencies":[]}
Shader "BloomConvolutionUnpack"
{
    Compute
    {
        // Convolution bloom: the convolved spectrum image (after the inverse row pass: real parts are the result)
        // back into the bloom setup texture it was packed from (BloomRenderer: the convolution's input and output). The content
        // rectangle is resampled to the chain's size (a bilinear of four real texels) and divided by N.x * N.y, the
        // unnormalised inverse FFT's gain, then scaled by the view's bloom strength (the composite adds it as is).
        layout(binding = 0, rgba32f) readonly uniform image2D Spectrum;
        layout(binding = 1, rgba16f) writeonly uniform image2D BloomOutput;

        PushConstant PushConstants
        {
            vec2  ContentSize;  // the content rectangle, spectrum texels
            int   Width;        // N.x
            float InverseCount; // 1 / (N.x * N.y)
            float Intensity;    // BloomIntensity x BloomConvolutionIntensity (UE: the FFT's own strength)
        };

        LocalSize(16, 16, 1);

        vec3 Fetch( ivec2 q )
        {
            q        = clamp( q, ivec2( 0 ), ivec2( ContentSize ) - 1 );
            vec4 rg  = imageLoad( Spectrum, q );
            vec4 b   = imageLoad( Spectrum, ivec2( q.x + Width, q.y ) );
            return vec3( rg.x, rg.z, b.x );
        }

        void main()
        {
            ivec2 outSize = imageSize( BloomOutput );
            ivec2 p       = ivec2( gl_GlobalInvocationID.xy );
            if ( p.x >= outSize.x || p.y >= outSize.y )
                return;
            vec2  s  = ( vec2( p ) + 0.5 ) * ContentSize / vec2( outSize ) - 0.5;
            ivec2 i  = ivec2( floor( s ) );
            vec2  f  = s - vec2( i );
            vec3  c  = mix( mix( Fetch( i ), Fetch( i + ivec2( 1, 0 ) ), f.x ),
                            mix( Fetch( i + ivec2( 0, 1 ) ), Fetch( i + ivec2( 1, 1 ) ), f.x ), f.y );
            // A ringing lobe of the transform is not light: clamp the negatives the band limit leaves.
            imageStore( BloomOutput, p, vec4( max( c * ( InverseCount * Intensity ), vec3( 0.0 ) ), 1.0 ) );
        }
    }
}
