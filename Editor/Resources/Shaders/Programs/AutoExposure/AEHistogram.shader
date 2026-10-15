// DesertAsset {"Kind":"Shader","Guid":"b0caa987ff4d7feb3a44bae8779e5731","Versions":{"SHDR":1},"Dependencies":[]}
Shader "AEHistogram"
{
    Compute
    {
        // Auto-exposure histogram build, ported from UE 5.8 Engine/Shaders/Private/PostProcessHistogram.usf:66-187
        // (MainCS) and PostProcessHistogramCommon.ush:56-65: one thread per scene texel takes the metered luminance
        // (UE's uniform 1/3 weights, floored at the histogram bottom), maps log2 of it into [0, 1] over the window,
        // and splits it LINEARLY between the two buckets that straddle that position. Bucket 0's lower share is
        // scaled by the black-bucket influence (UE default 0), so black never meters. Both shares are weighed by the
        // metering mask (UE AdaptationWeightTexture, PostProcessHistogramCommon.ush:201-204: red channel, bilinear at
        // the texel's screen UV; PostProcessSettings::AutoExposureMeterMask, System.White when none).
        // Adapted: 256 buckets (UE 64: finer, the same window), the weights in uint fixed point x u_WeightScale
        // (AutoExposureWeightScale) because float atomics are not portable, one texel per thread (UE gathers 2x2;
        // the same texels) and the mask read at each texel's centre (UE: once per quad, at its corner).
        //
        // TWO-LEVEL accumulation: threads accumulate into a SHARED per-workgroup histogram, then each of the 256
        // threads (16x16 = one thread per bucket) flushes one bucket to the global buffer. Per-texel atomics into
        // the storage buffer cost SECONDS per frame on MoltenVK.

        LocalSize(16, 16, 1);

        Uniform(0) sampler2D u_Scene;
        Uniform(2) sampler2D u_MeterMask;
        Buffer(1) Histogram
        {
            uint u_Bins[256];
        };

        PushConstant PushConstants
        {
            float u_MinLogLum;      // log2 of the darkest metered luminance (UE HistogramLogMin)
            float u_InvLogLumRange; // 1 / (log2 range) (UE HistogramScale)
            float u_WeightScale;    // fixed-point scale of one texel's weight (AutoExposureWeightScale)
            float u_BlackBucketInfluence; // UE r.EyeAdaptation.BlackHistogramBucketInfluence
        };

        shared uint s_Bins[256];

        // UE CalculateEyeAdaptationLuminance with r.AutoExposure.LuminanceMethod 0 (AutoExposureRules.hpp
        // AutoExposureSplit) and LuminanceMin = exp2(HistogramLogMin).
        float MeteredLuminance( vec3 c )
        {
            return max( dot( c, vec3( 1.0 / 3.0 ) ), exp2( u_MinLogLum ) );
        }

        void main()
        {
            const uint local = gl_LocalInvocationIndex; // 0..255: exactly one thread per bucket
            s_Bins[local]    = 0u;
            barrier();

            ivec2 size  = textureSize( u_Scene, 0 );
            ivec2 coord = ivec2( gl_GlobalInvocationID.xy );
            if ( coord.x < size.x && coord.y < size.y )
            {
                float logLuminance = log2( MeteredLuminance( texelFetch( u_Scene, coord, 0 ).rgb ) );
                // NaN fails every comparison: it meters as the bottom, as UE's max() floor does.
                float position = ( logLuminance - u_MinLogLum ) * u_InvLogLumRange;
                float fBucket  = ( position > 0.0 ) ? min( position, 1.0 ) * 255.0 : 0.0;

                uint  bucket0 = min( uint( fBucket ), 255u );
                uint  bucket1 = min( bucket0 + 1u, 255u );
                float weight1 = fract( fBucket );
                float weight0 = 1.0 - weight1;
                if ( bucket0 == 0u )
                    weight0 *= u_BlackBucketInfluence;
                const float screenWeight = textureLod( u_MeterMask, ( vec2( coord ) + 0.5 ) / vec2( size ), 0.0 ).x;
                weight0 *= screenWeight;
                weight1 *= screenWeight;

                uint weight0Int = uint( weight0 * u_WeightScale );
                uint weight1Int = uint( weight1 * u_WeightScale );
                if ( weight0Int > 0u )
                    atomicAdd( s_Bins[bucket0], weight0Int );
                if ( weight1Int > 0u )
                    atomicAdd( s_Bins[bucket1], weight1Int );
            }
            barrier();

            const uint count = s_Bins[local];
            if ( count > 0u )
                atomicAdd( u_Bins[local], count );
        }
    }
}
