// DesertAsset {"Kind":"Shader","Guid":"a54619ed2aeaf2aa1df042cfd72d7b7c","Versions":{"SHDR":1},"Dependencies":[]}
Shader "AEAverage"
{
    Compute
    {
        // Auto-exposure resolve: one 256-invocation group, one invocation per histogram bin (UE
        // PostProcessEyeAdaptation's group-parallel histogram reduce). A shared-memory prefix scan gives every bin
        // its cumulative range, so the darkest/brightest percentile tails are discarded (outlier rejection) per bin
        // in parallel; a tree reduction weighted-averages the surviving bins into a log-luminance; invocation 0
        // temporally adapts toward it in log2 (UE ComputeEyeAdaptation, PostProcessHistogramCommon.ush:206-240:
        // linear SpeedUp / SpeedDown stops per second while farther than TransitionDistance stops, exponential
        // inside it) and writes the new 1x1 value that
        // tonemap turns into exposure; a second tree reduction averages the local exposure over the bins.

        LocalSize(256, 1, 1);

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
            float SpeedUp;            // UE AutoExposureSpeedUp: stops per second toward a brighter target
            float SpeedDown;          // UE AutoExposureSpeedDown: toward a darker one
            float ExponentialUpM;     // UE ExponentialUpM / DownM (PostProcessEyeAdaptation.cpp:710-711): the
            float ExponentialDownM;   // exponential's slope matched to the linear one at TransitionDistance
            float TransitionDistance; // UE r.EyeAdaptation.ExponentialTransitionDistance (1.5 stops)
            float MinLuma;
            float MaxLuma;
            float MinLogLuminance;
            float LogLuminanceRange;
            float LowPercent;  // discard this fraction of the darkest samples (UE AutoExposureLowPercent / 100)
            float HighPercent; // keep up to this cumulative fraction (UE AutoExposureHighPercent / 100)
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

        // UE ComputeEyeAdaptation in log2 luminance: linear while the target is farther than TransitionDistance
        // stops, exponential inside it; no clamp here (the target is clamped).
        float AdaptLog2( float logCurrent, float logTarget )
        {
            const float diff   = logTarget - logCurrent;
            const float speed  = diff > 0.0 ? SpeedUp : SpeedDown;
            const float m      = diff > 0.0 ? ExponentialUpM : ExponentialDownM;
            const float factor = 1.0 - exp2( -DeltaTime * speed );
            const float exponential = logCurrent + diff * factor * m;
            const float offset      = DeltaTime * speed;
            const float linear      = logCurrent < logTarget ? min( logTarget, logCurrent + offset )
                                                             : max( logTarget, logCurrent - offset );
            return abs( diff ) > TransitionDistance ? linear : exponential;
        }

        shared uint  s_Scan[256];     // inclusive prefix sum of the bin counts (exact integers)
        shared float s_SumA[256];     // tree-reduction lane A (weighted log-luminance, then local exposure)
        shared float s_SumB[256];     // tree-reduction lane B (weight)
        shared float s_Exposed;       // invocation 0's exposed luminance, broadcast to the local-exposure reduce

        // Sums s_SumA / s_SumB over the group into element 0 (every invocation calls it: uniform control flow).
        void ReduceSums( uint lane )
        {
            for ( uint stride = 128u; stride > 0u; stride >>= 1u )
            {
                if ( lane < stride )
                {
                    s_SumA[lane] += s_SumA[lane + stride];
                    s_SumB[lane] += s_SumB[lane + stride];
                }
                barrier();
            }
        }

        void main()
        {
            const uint  lane  = gl_LocalInvocationIndex;
            const uint  count = Bins[lane];
            const float binLogLum = MinLogLuminance + ( float( lane ) / 255.0 ) * LogLuminanceRange;

            // Hillis-Steele inclusive scan: after it s_Scan[lane] = sum of Bins[0..lane], s_Scan[255] = total.
            s_Scan[lane] = count;
            barrier();
            for ( uint offset = 1u; offset < 256u; offset <<= 1u )
            {
                const uint partial = lane >= offset ? s_Scan[lane - offset] : 0u;
                barrier();
                s_Scan[lane] += partial;
                barrier();
            }
            const uint total     = s_Scan[255];
            const uint inclusive = s_Scan[lane];

            // Keep samples in the cumulative range [loCount, hiCount]; partial bins are weighted by overlap.
            float weight = 0.0;
            if ( total > 0u )
            {
                const float loCount    = LowPercent * float( total );
                const float hiCount    = HighPercent * float( total );
                const float binStart   = float( inclusive - count );
                const float cumulative = float( inclusive );
                weight = max( min( cumulative, hiCount ) - max( binStart, loCount ), 0.0 );
            }
            s_SumA[lane] = binLogLum * weight;
            s_SumB[lane] = weight;
            barrier();
            ReduceSums( lane );

            if ( lane == 0u )
            {
                float avgLum;
                float measured = -1.0;
                if ( total == 0u )
                {
                    avgLum = MinLuma;
                }
                else
                {
                    const float wLogSum   = s_SumA[0];
                    const float wSum      = s_SumB[0];
                    const float avgLogLum = ( wSum > 0.0 ) ? ( wLogSum / wSum ) : MinLogLuminance;
                    measured              = exp2( avgLogLum );
                    avgLum                = clamp( measured, MinLuma, MaxLuma );
                }

                float prev = texture( PreviousLuminanceTexture, vec2( 0.5 ) ).r;
                if ( !( prev > 0.0 ) ) // first frame / NaN / uninitialised
                    prev = avgLum;

                const float adapted = exp2( AdaptLog2( log2( prev ), log2( avgLum ) ) );
                // UE's exposure compensation (AutoExposureBias + the curve at the adapted average's EV100; EV100 =
                // log2(L) + 3, Graphic::LuminanceToEV100): the exposure is computed from adapted / 2^compensation,
                // kept in .g and in the read-back; .r stays the smoothing state, so a bias change does not ramp.
                const float compensation =
                    CompensationBias + ExposureCompensationCurve( log2( max( adapted, 1e-10 ) ) + 3.0 );
                const float exposed = adapted * exp2( -compensation );
                imageStore( OutLuminanceTexture, ivec2( 0 ), vec4( adapted, exposed, 0.0, 1.0 ) );
                ReadbackData = vec4( exposed, 1.0, measured, float( total ) );
                s_Exposed    = exposed;
            }
            barrier();

            // UE's AverageLocalExposure: every bucket treated as its own base layer (detail 0), exposed by this
            // frame's adapted exposure, averaged by sample count. The condition is group-uniform (push constant +
            // the shared total), so the barriers inside stay in uniform control flow.
            float averageLocalExposure = 1.0;
            if ( LocalExposureContrast.w > 0.5 && total > 0u )
            {
                const float logExposure = log2( ExposureKey / max( s_Exposed, 1e-4 ) );
                float term = 0.0;
                if ( count > 0u )
                {
                    const float logLum = binLogLum + logExposure;
                    term = float( count ) * exp2( LocalExposureLogExposure( logLum, logLum, LocalExposureLogMiddleGrey,
                                                                            vec4( LocalExposureContrast.xy, 0.0, 0.0 ),
                                                                            LocalExposureThreshold ) );
                }
                s_SumA[lane] = term;
                s_SumB[lane] = 0.0;
                barrier();
                ReduceSums( lane );
                averageLocalExposure = s_SumA[0] / float( total );
            }
            if ( lane == 0u )
                ReadbackLocal = vec4( averageLocalExposure, 0.0, 0.0, 0.0 );
        }
    }
}
