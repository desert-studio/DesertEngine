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
            uint Bins[256];
        };
        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform sampler PointClampSampler;
        Uniform(1) texture2D PreviousLuminanceTexture;             // 1x1 previous adapted luminance
        #define PreviousLuminanceTexture sampler2D(PreviousLuminanceTexture, PointClampSampler)
        layout(binding = 2, rgba32f) writeonly uniform image2D OutLuminanceTexture; // 1x1 new adapted luminance
        // The same value for the CPU (UE FViewInfo::UpdatePreExposure's eye-adaptation readback): a per-frame
        // host-readable copy, read FramesInFlight frames later (AutoExposureRenderer::ReadBackAdaptedLuminance).
        // y = 1 marks a written value; z = this frame's measured luminance before the clamp and the adaptation
        // (-1 = empty histogram), w = the histogram's sample count — the read-back's trace says WHY x is what it is.
        // ReadbackLocal.x = AverageLocalExposure (UE PostProcessEyeAdaptation.usf:202-248; 1 at identity).
        Buffer(4) Readback
        {
            vec4 ReadbackData;
            vec4 ReadbackLocal;
        };

        PushConstant PushConstants
        {
            float DeltaTime;
            float AdaptationSpeed;
            float MinLuma;
            float MaxLuma;
            float MinLogLuminance;
            float LogLuminanceRange;
            float LowPercent;  // discard this fraction of the darkest samples
            float HighPercent; // keep up to this cumulative fraction (discard the brightest tail above it)
            vec4  LocalExposureContrast;  // highlight, shadow, detail (0: a bucket is its own base), enabled
            vec4  LocalExposureThreshold; // hi thr, sh thr, hi strength, sh strength
            float LocalExposureLogMiddleGrey;
            float ExposureKey;
            float CompensationBias; // UE AutoExposureBias (EV)
            float CompensationKeys; // the Exposure Compensation Curve's key count (0 = no curve)
            vec4  CurveEV100;       // its keys' EV100, ascending
            vec4  CurveBias;        // its bias (EV) at each key
        };

        #include <Common/LocalExposure.glslh>

        // UE's Exposure Compensation Curve at @p ev100, piecewise linear over the keys, flat past the ends - the
        // GPU twin of Graphic::ExposureCompensationCurveBias (AutoExposureRules.hpp).
        float ExposureCompensationCurve( float ev100 )
        {
            int count = int( CompensationKeys + 0.5 );
            if ( count <= 0 )
                return 0.0;
            count = min( count, 4 );
            if ( ev100 <= CurveEV100[0] )
                return CurveBias[0];
            for ( int i = 1; i < count; ++i )
                if ( ev100 < CurveEV100[i] )
                {
                    float t = ( ev100 - CurveEV100[i - 1] ) / max( CurveEV100[i] - CurveEV100[i - 1], 1e-6 );
                    return mix( CurveBias[i - 1], CurveBias[i], t );
                }
            return CurveBias[count - 1];
        }

        void main()
        {
            uint total = 0u;
            for ( int i = 0; i < 256; ++i )
                total += Bins[i];

            float avgLum;
            float measured = -1.0;
            if ( total == 0u )
            {
                avgLum = MinLuma;
            }
            else
            {
                // Keep samples in the cumulative range [loCount, hiCount]; partial bins are weighted by overlap.
                float loCount    = LowPercent * float( total );
                float hiCount    = HighPercent * float( total );
                float cumulative = 0.0;
                float wLogSum    = 0.0;
                float wSum       = 0.0;

                for ( int i = 0; i < 256; ++i )
                {
                    float c        = float( Bins[i] );
                    float binStart = cumulative;
                    cumulative += c;
                    float w = max( min( cumulative, hiCount ) - max( binStart, loCount ), 0.0 );
                    if ( w > 0.0 )
                    {
                        float logLum = MinLogLuminance + ( float( i ) / 255.0 ) * LogLuminanceRange;
                        wLogSum += logLum * w;
                        wSum += w;
                    }
                }

                float avgLogLum = ( wSum > 0.0 ) ? ( wLogSum / wSum ) : MinLogLuminance;
                measured        = exp2( avgLogLum );
                avgLum          = clamp( measured, MinLuma, MaxLuma );
            }

            float prev = texture( PreviousLuminanceTexture, vec2( 0.5 ) ).r;
            if ( !( prev > 0.0 ) ) // first frame / NaN / uninitialised
                prev = avgLum;

            float adapted = mix( prev, avgLum, 1.0 - exp( -DeltaTime * AdaptationSpeed ) );
            // UE's exposure compensation (AutoExposureBias + the curve at the adapted average's EV100; EV100 =
            // log2(L) + 3, Graphic::LuminanceToEV100): the exposure is computed from adapted / 2^compensation,
            // kept in .g and in the read-back; .r stays the smoothing state, so a bias change does not ramp.
            float compensation =
                CompensationBias + ExposureCompensationCurve( log2( max( adapted, 1e-10 ) ) + 3.0 );
            float exposed      = adapted * exp2( -compensation );
            imageStore( OutLuminanceTexture, ivec2( 0 ), vec4( adapted, exposed, 0.0, 1.0 ) );
            ReadbackData = vec4( exposed, 1.0, measured, float( total ) );

            // UE's AverageLocalExposure: every bucket treated as its own base layer (detail 0), exposed by this
            // frame's adapted exposure, averaged by sample count.
            float averageLocalExposure = 1.0;
            if ( LocalExposureContrast.w > 0.5 && total > 0u )
            {
                const float logExposure = log2( ExposureKey / max( exposed, 1e-4 ) );
                float sum = 0.0;
                for ( int i = 0; i < 256; ++i )
                {
                    const float c = float( Bins[i] );
                    if ( c <= 0.0 )
                        continue;
                    const float logLum = MinLogLuminance + ( float( i ) / 255.0 ) * LogLuminanceRange + logExposure;
                    sum += c * exp2( LocalExposureLogExposure( logLum, logLum, LocalExposureLogMiddleGrey,
                                                               vec4( LocalExposureContrast.xy, 0.0, 0.0 ),
                                                               LocalExposureThreshold ) );
                }
                averageLocalExposure = sum / float( total );
            }
            ReadbackLocal = vec4( averageLocalExposure, 0.0, 0.0, 0.0 );
        }
    }
}
