// DesertAsset {"Kind":"Shader","Guid":"b0caa987ff4d7feb3a44bae8779e5731","Versions":{"SHDR":1},"Dependencies":[]}
Shader "AEHistogram"
{
    Compute
    {
        // Auto-exposure histogram build: one thread per scene texel computes log2 luminance, maps it into one of
        // 256 bins over [MinLogLuminance, MinLogLuminance + 1/InverseLogLuminanceRange], and increments that bin. The full-frame
        // histogram lets AEAverage meter with percentile clipping (robust to bright/dark outliers).
        //
        // TWO-LEVEL accumulation (the classic exposure-histogram pattern): threads first accumulate into a
        // SHARED per-workgroup histogram, then each of the 256 threads (16x16 = one thread per bin) flushes one
        // bin to the global buffer. That is ~256x fewer global atomics than the naive per-texel atomicAdd —
        // per-texel atomics into the storage buffer made this dispatch cost SECONDS per frame on MoltenVK.

        LocalSize(16, 16, 1);

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform sampler PointClampSampler;
        Uniform(0) texture2D SceneColorTexture;
        #define SceneColorTexture sampler2D(SceneColorTexture, PointClampSampler)
        Buffer(1) Histogram
        {
            uint Bins[256];
        };

        PushConstant PushConstants
        {
            float MinLogLuminance;      // log2 of the darkest metered luminance
            float InverseLogLuminanceRange; // 1 / (log2 range)
            float InversePreExposure; // 1 / ViewFrame::PreExposure: the scene colour is stored pre-exposed
        };

        shared uint s_Bins[256];

        float Luminance( vec3 c )
        {
            return dot( c, vec3( 0.2126, 0.7152, 0.0722 ) );
        }

        void main()
        {
            const uint local = gl_LocalInvocationIndex; // 0..255 — exactly one thread per bin
            s_Bins[local]    = 0u;
            barrier();

            ivec2 size  = textureSize( SceneColorTexture, 0 );
            ivec2 coord = ivec2( gl_GlobalInvocationID.xy );
            if ( coord.x < size.x && coord.y < size.y )
            {
                float l = Luminance( texelFetch( SceneColorTexture, coord, 0 ).rgb ) * InversePreExposure;

                uint bin;
                if ( l < 1e-5 )
                {
                    bin = 0u; // treat near-black as the bottom bin (avoids log2 of ~0)
                }
                else
                {
                    float t = clamp( ( log2( l ) - MinLogLuminance ) * InverseLogLuminanceRange, 0.0, 1.0 );
                    bin     = uint( t * 255.0 + 0.5 );
                }

                atomicAdd( s_Bins[bin], 1u );
            }
            barrier();

            const uint count = s_Bins[local];
            if ( count > 0u )
                atomicAdd( Bins[local], count );
        }
    }
}
