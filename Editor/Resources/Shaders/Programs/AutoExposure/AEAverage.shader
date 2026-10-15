// DesertAsset {"Kind":"Shader","Guid":"a54619ed2aeaf2aa1df042cfd72d7b7c","Versions":{"SHDR":1},"Dependencies":[]}
Shader "AEAverage"
{
    Compute
    {
        // Auto-exposure resolve, ported from UE 5.8 Engine/Shaders/Private/PostProcessHistogramCommon.ush:149-260
        // (ComputeAverageLuminanceWithoutOutlier, ComputeEyeAdaptation, ExponentialAdaption, LinearAdaption) and
        // PostProcessEyeAdaptation.usf:168-197 (EyeAdaptationCommon), adapted: the 1x1 history holds the adapted
        // AVERAGE LUMINANCE (UE holds its reciprocal, the exposure scale, with the compensation folded in; the
        // tonemap applies 2^Bias x 0.18 / L, the same number); a 256-bin uint histogram.
        //
        // A single invocation walks the histogram, discards the darkest / brightest percentile tails, averages the
        // rest in log2, clamps that into the view's EV100 range, then moves the history toward it IN EV at the up or
        // down speed: linearly while further than StartDistance, exponentially once near.

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
            float u_SpeedUp;           // EV per second toward a brighter target
            float u_SpeedDown;         // EV per second toward a darker target
            float u_ExponentialUpM;
            float u_ExponentialDownM;
            float u_StartDistance;     // EV
            float u_MinAverageLuminance;
            float u_MaxAverageLuminance;
            float u_MinLogLum;
            float u_LogLumRange;
            float u_LowPct;  // discard this fraction of the darkest samples
            float u_HighPct; // keep up to this cumulative fraction (discard the brightest tail above it)
            float u_ForceTarget; // 1 = land on the target (scene replaced, camera cut)
        };

        float AverageLuminanceWithoutOutliers()
        {
            // UE's special case: equal tails meter nothing and expose for 1.
            if ( abs( u_LowPct - u_HighPct ) < 0.0001 )
                return 1.0;

            float total = 0.0;
            for ( int i = 0; i < 256; ++i )
                total += float( u_Bins[i] );

            float minFractionSum = u_LowPct * total;
            float maxFractionSum = u_HighPct * total;
            vec2  sumWithoutOutliers = vec2( 0.0 );
            for ( int i = 0; i < 256; ++i )
            {
                float localValue = float( u_Bins[i] );

                // Remove the outliers at the lower end...
                float sub = min( localValue, minFractionSum );
                localValue -= sub;
                minFractionSum -= sub;
                maxFractionSum -= sub;

                // ...and at the upper end.
                localValue = min( localValue, maxFractionSum );
                maxFractionSum -= localValue;

                float logLuminanceAtBucket = u_MinLogLum + ( float( i ) / 255.0 ) * u_LogLumRange;
                sumWithoutOutliers += vec2( logLuminanceAtBucket, 1.0 ) * localValue;
            }

            if ( sumWithoutOutliers.y == 0.0 )
                return u_MinAverageLuminance / 0.18;
            return exp2( sumWithoutOutliers.x / sumWithoutOutliers.y );
        }

        float ExponentialAdaption( float current, float target, float frameTime, float adaptionSpeed, float m )
        {
            float factor = 1.0 - exp2( -frameTime * adaptionSpeed );
            return current + ( target - current ) * factor * m;
        }

        float LinearAdaption( float current, float target, float frameTime, float adaptionSpeed )
        {
            float offset = frameTime * adaptionSpeed;
            return ( current < target ) ? min( target, current + offset ) : max( target, current - offset );
        }

        void main()
        {
            float target = clamp( AverageLuminanceWithoutOutliers(), u_MinAverageLuminance, u_MaxAverageLuminance );

            float previous = texture( u_PrevLum, vec2( 0.5 ) ).r;
            if ( !( previous > 0.0 ) ) // first frame / NaN / uninitialised
                previous = target;

            float logTarget = log2( target );
            float logOld    = log2( previous );
            float logDiff   = logTarget - logOld;

            float speed = ( logDiff > 0.0 ) ? u_SpeedUp : u_SpeedDown;
            float m     = ( logDiff > 0.0 ) ? u_ExponentialUpM : u_ExponentialDownM;

            float logAdapted = ( abs( logDiff ) > u_StartDistance )
                                   ? LinearAdaption( logOld, logTarget, u_Dt, speed )
                                   : ExponentialAdaption( logOld, logTarget, u_Dt, speed, m );

            // The step itself is unclamped (UE's note on ComputeEyeAdaptation: leaving one volume's range for
            // another's ramps instead of jumping); EyeAdaptationCommon then clamps the smoothed value to the range.
            float adapted = clamp( mix( exp2( logAdapted ), target, u_ForceTarget ), u_MinAverageLuminance,
                                   u_MaxAverageLuminance );
            imageStore( u_OutLum, ivec2( 0 ), vec4( adapted, 0.0, 0.0, 1.0 ) );
        }
    }
}
