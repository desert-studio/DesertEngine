// DesertAsset {"Kind":"Shader","Guid":"a54619ed2aeaf2aa1df042cfd72d7b7c","Versions":{"SHDR":1},"Dependencies":[]}
Shader "AEAverage"
{
    Compute
    {
        // Auto-exposure resolve: a single invocation walks the 256-bin histogram, discards the darkest/brightest
        // percentile tails (outlier rejection), weighted-averages the surviving bins into a log-luminance, then
        // temporally adapts toward it (read the previous 1x1 luminance, lerp) and writes the new 1x1 value that
        // tonemap turns into exposure.

        LocalSize(1, 1, 1);

        Buffer(0) Histogram
        {
            uint u_Bins[256];
        };
        Uniform(1) sampler2D u_PrevLum;             // 1x1 previous adapted luminance
        layout(binding = 2, rgba32f) writeonly uniform image2D u_OutLum; // 1x1 new adapted luminance

        PushConstant PushConstants
        {
            float u_Dt;
            float u_AdaptSpeed;
            float u_MinLuma;
            float u_MaxLuma;
            float u_MinLogLum;
            float u_LogLumRange;
            float u_LowPct;  // discard this fraction of the darkest samples
            float u_HighPct; // keep up to this cumulative fraction (discard the brightest tail above it)
        };

        void main()
        {
            uint total = 0u;
            for ( int i = 0; i < 256; ++i )
                total += u_Bins[i];

            float avgLum;
            if ( total == 0u )
            {
                avgLum = u_MinLuma;
            }
            else
            {
                // Keep samples in the cumulative range [loCount, hiCount]; partial bins are weighted by overlap.
                float loCount    = u_LowPct * float( total );
                float hiCount    = u_HighPct * float( total );
                float cumulative = 0.0;
                float wLogSum    = 0.0;
                float wSum       = 0.0;

                for ( int i = 0; i < 256; ++i )
                {
                    float c        = float( u_Bins[i] );
                    float binStart = cumulative;
                    cumulative += c;
                    float w = max( min( cumulative, hiCount ) - max( binStart, loCount ), 0.0 );
                    if ( w > 0.0 )
                    {
                        float logLum = u_MinLogLum + ( float( i ) / 255.0 ) * u_LogLumRange;
                        wLogSum += logLum * w;
                        wSum += w;
                    }
                }

                float avgLogLum = ( wSum > 0.0 ) ? ( wLogSum / wSum ) : u_MinLogLum;
                avgLum          = clamp( exp2( avgLogLum ), u_MinLuma, u_MaxLuma );
            }

            float prev = texture( u_PrevLum, vec2( 0.5 ) ).r;
            if ( !( prev > 0.0 ) ) // first frame / NaN / uninitialised
                prev = avgLum;

            float adapted = mix( prev, avgLum, 1.0 - exp( -u_Dt * u_AdaptSpeed ) );
            imageStore( u_OutLum, ivec2( 0 ), vec4( adapted, 0.0, 0.0, 1.0 ) );
        }
    }
}
