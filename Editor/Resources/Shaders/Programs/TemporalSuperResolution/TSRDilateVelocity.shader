// DesertAsset {"Kind":"Shader","Guid":"6dad0189e4dc4a0e966fd55ef41fdada","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TSRDilateVelocity"
{
    Compute
    {
        // TSR.DilateVelocity. Ported from UE 5.8 Engine/Shaders/Private/TemporalSuperResolution/TSRDilateVelocity.usf
        // (the closest depth of the 3x3 picks the velocity; with r.TSR.ReprojectionField :176-268 the reprojection
        // jacobian, ComputeReprojectionJacobian / ComputeDepthBilaterals / ComputeVelocityJacobian of
        // TSRDepthVelocityAnalysis.ush :332-548, and the depth-anti-aliased reprojection boundary :58-143), and
        // TSRClosestOccluder.ush ScatterClosestOccluder :101 (the closest depth scattered to where it was last frame,
        // InterlockedMax), adapted: one pixel per lane (no butterfly kernel), no motion-blur directions output, no
        // velocity-edge (decimate) mask, the scatter is atomicMax on a storage buffer over the previous position's
        // bilinear 2x2 footprint, the depth browse samples the depth bilinearly by hand (linear filtering of
        // DEPTH32F is not guaranteed on Vulkan / MoltenVK). A pixel nothing wrote velocity for (depth 0: the
        // reversed-Z far plane, the sky) is reprojected through the camera, as TemporalAA.shader does.
        //
        // u_ReprojectionField (r.TSR.ReprojectionField, a uniform as in UE): 1 writes the field TSR.UpdateHistory
        // reads — u_ReprojectionJacobian (UE EncodeReprojectionJacobian) and u_ReprojectionBoundary (the dilating
        // offset to the closest depth and the boundary, UE EncodeReprojectionBoundary); 0 writes both as 0.
        LocalSize(8, 8, 1);

        #include <Common/TSRCommon.glslh>

        Uniform(1) sampler2D u_SceneDepth; // reversed-Z device depth, RenderExtent
        Uniform(2) sampler2D u_Velocity;   // NDC current - previous, RenderExtent
        layout(binding = 3, rgba16f) restrict writeonly uniform image2D u_DilatedVelocity; // xy: NDC velocity
        layout(binding = 4, r32f) restrict writeonly uniform image2D u_ClosestDepth;       // device depth
        layout(std430, binding = 5) restrict buffer TSRPrevAtomicBuffer
        {
            uint u_PrevAtomic[];
        };
        layout(binding = 6, r32ui) restrict writeonly uniform uimage2D u_ReprojectionJacobian;
        layout(binding = 7, r32ui) restrict writeonly uniform uimage2D u_ReprojectionBoundary;

        float DepthAt(ivec2 p)
        {
            return texelFetch(u_SceneDepth, TsrClampRender(p), 0).r;
        }

        // The bilinear device depth at a render-pixel position, clamped to UE's bilinear viewport.
        float BilinearDepth(vec2 position)
        {
            const vec2  p    = clamp(position, vec2(0.5), u_RenderSize - 0.5) - 0.5;
            const ivec2 base = ivec2(floor(p));
            const vec2  f    = p - vec2(base);
            const float d00  = DepthAt(base);
            const float d10  = DepthAt(base + ivec2(1, 0));
            const float d01  = DepthAt(base + ivec2(0, 1));
            const float d11  = DepthAt(base + ivec2(1, 1));
            return mix(mix(d00, d10, f.x), mix(d01, d11, f.x), f.y);
        }
        #define TSR_BROWSE_SAMPLE(position) BilinearDepth(position)
        #include <Common/TSRSpatialAntiAliasing.glslh>

        // The NDC velocity of the render pixel @p p at device depth @p depth: its drawn velocity, or the camera's
        // reprojection of the direction where nothing drew (depth 0, the sky).
        vec2 NdcVelocityAt(ivec2 p, float depth)
        {
            if (depth > 0.0)
                return texelFetch(u_Velocity, TsrClampRender(p), 0).xy;
            const vec2 uv       = TsrSurfaceUv(p);
            const vec4 world    = u_InvViewProjection * vec4(ScreenUVToNdc(uv), depth, 1.0);
            const vec4 prevClip = u_PrevViewProjection * world;
            return ScreenUVToNdc(uv) - prevClip.xy / prevClip.w;
        }

        // The 3x3 neighbourhood, [y + 1][x + 1].
        float s_Depth[3][3];
        vec2  s_PixelVelocity[3][3];

        float DepthN(ivec2 o)
        {
            return s_Depth[o.y + 1][o.x + 1];
        }
        vec2 PixelVelocityN(ivec2 o)
        {
            return s_PixelVelocity[o.y + 1][o.x + 1];
        }

        // UE ComputeDepthBilaterals: how much each of the four neighbours along (directionE, directionS) lies on the
        // centre's surface; false when neither axis has any.
        bool ComputeDepthBilaterals(ivec2 directionE, ivec2 directionS, out vec4 weights)
        {
            const float weightOffset  = 1.5;
            const float pixelDistance = length(vec2(directionE));
            const float c             = DepthN(ivec2(0));
            const float e             = DepthN(directionE);
            const float w             = DepthN(-directionE);
            const float s             = DepthN(directionS);
            const float n             = DepthN(-directionS);

            const float variationEW = abs(c - (e + w) * 0.5);
            const float variationSN = abs(c - (s + n) * 0.5);
            const float diffE       = abs(e - c);
            const float diffW       = abs(w - c);
            const float diffS       = abs(s - c);
            const float diffN       = abs(n - c);
            const float minDerivativeE = min(abs(e - w), min(diffE, diffW));
            const float minDerivativeS = min(abs(s - n), min(diffS, diffN));

            const float pixelError = TsrPixelDeviceZError(c) * pixelDistance;
            const float derivativeE = min(max(minDerivativeE, pixelError), max(minDerivativeS, 8.0 * pixelError));
            const float derivativeS = min(max(minDerivativeS, pixelError), max(minDerivativeE, 8.0 * pixelError));

            float weightE = clamp(weightOffset - min(diffE, variationEW) / derivativeE, 0.0, 1.0);
            float weightW = clamp(weightOffset - min(diffW, variationEW) / derivativeE, 0.0, 1.0);
            float weightS = clamp(weightOffset - min(diffS, variationSN) / derivativeS, 0.0, 1.0);
            float weightN = clamp(weightOffset - min(diffN, variationSN) / derivativeS, 0.0, 1.0);

            const float totalEW = weightE + weightW;
            const float totalSN = weightS + weightN;
            const float invEW   = totalEW > 0.0 ? 1.0 / totalEW : 0.0;
            const float invSN   = totalSN > 0.0 ? 1.0 / totalSN : 0.0;
            weights             = vec4(weightE * invEW, weightW * invEW, weightS * invSN, weightN * invSN);
            return totalEW > 0.0 || totalSN > 0.0;
        }

        // UE ComputeVelocityJacobian.
        mat2 ComputeVelocityJacobian(ivec2 directionE, ivec2 directionS, vec4 weights)
        {
            const vec2 c = PixelVelocityN(ivec2(0));
            mat2       jacobian;
            jacobian[0] = (PixelVelocityN(directionE) - c) * weights.x + (c - PixelVelocityN(-directionE)) * weights.y;
            jacobian[1] = (PixelVelocityN(directionS) - c) * weights.z + (c - PixelVelocityN(-directionS)) * weights.w;
            return jacobian;
        }

        // UE ComputeReprojectionJacobian: cardinals first, the diagonals rotated back when the cardinals fail.
        mat2 ComputeReprojectionJacobian()
        {
            vec4 weights;
            if (ComputeDepthBilaterals(ivec2(1, 0), ivec2(0, 1), weights))
                return ComputeVelocityJacobian(ivec2(1, 0), ivec2(0, 1), weights);
            ComputeDepthBilaterals(ivec2(1, 1), ivec2(-1, 1), weights);
            const mat2 diagonal = ComputeVelocityJacobian(ivec2(1, 1), ivec2(-1, 1), weights);
            mat2       jacobian;
            jacobian[0] = diagonal[0] * 0.5 - diagonal[1] * 0.5;
            jacobian[1] = diagonal[0] * 0.5 + diagonal[1] * 0.5;
            return jacobian;
        }

        // UE TSRDilateVelocity.usf ComputeReprojectionBoundary :58: anti-alias the depth edge the dilation crosses
        // when the dilation changes the velocity enough, else dilate fully.
        vec2 ComputeBoundary(ivec2 pixel, vec2 ndcVelocity, vec2 dilatedNdcVelocity)
        {
            const int  kBrowsingIterations = 3;
            const vec2 screenPos           = ScreenUVToNdc((vec2(pixel) + 0.5) / u_RenderSize);
            const vec2 prevScreenPos       = screenPos - dilatedNdcVelocity;
            const bool offscreen           = any(notEqual(prevScreenPos, clamp(prevScreenPos, vec2(-1.0), vec2(1.0))));
            const vec2 differential        = u_RenderSize * (ndcVelocity - dilatedNdcVelocity);
            const bool enoughDifference    = dot(differential, differential) > u_ReprojectionFieldAntiAliasVelocityThreshold;
            if (!enoughDifference || offscreen)
                return kFullDilateBoundary;

            float noiseFiltering;
            ivec2 browseDirection;
            ivec2 edgeSide;
            float edgeInput;
            TsrFindBrowsingDirection(DepthN(ivec2(0)), DepthN(ivec2(0, -1)), DepthN(ivec2(0, 1)), DepthN(ivec2(1, 0)),
                                     DepthN(ivec2(-1, 0)), DepthN(ivec2(1, -1)), DepthN(ivec2(-1, -1)),
                                     DepthN(ivec2(1, 1)), DepthN(ivec2(-1, 1)), noiseFiltering, browseDirection,
                                     edgeSide, edgeInput);
            int  lengthP;
            int  lengthN;
            bool incrementP;
            bool incrementN;
            bool decrementP;
            bool decrementN;
            TsrBrowseNeighborhoodBilinear(kBrowsingIterations, DepthN(ivec2(0)), edgeInput, pixel, browseDirection,
                                          edgeSide, lengthP, lengthN, incrementP, incrementN, decrementP,
                                          decrementN);
            return TsrComputeReprojectionBoundary(kBrowsingIterations, edgeSide, lengthP, lengthN, incrementP,
                                                  incrementN, decrementP, decrementN);
        }

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            if (any(greaterThanEqual(pixel, ivec2(u_RenderSize))))
                return;

            float closest       = -1.0;
            ivec2 closestOffset = ivec2(0);
            for (int y = -1; y <= 1; ++y)
            {
                for (int x = -1; x <= 1; ++x)
                {
                    const ivec2 o     = ivec2(x, y);
                    const float depth = DepthAt(pixel + o);
                    s_Depth[y + 1][x + 1] = depth;
                    if (depth > closest) // reversed-Z: larger is nearer
                    {
                        closest       = depth;
                        closestOffset = o;
                    }
                }
            }
            const ivec2 closestPixel = TsrClampRender(pixel + closestOffset);

            // The closest surface at THIS pixel, in homogeneous world space, never divided: the sky at depth 0 is a
            // point at infinity, and its reprojection is still that of its direction.
            const vec2 uv       = TsrSurfaceUv(pixel);
            const vec4 world    = u_InvViewProjection * vec4(ScreenUVToNdc(uv), closest, 1.0);
            const vec4 prevClip = u_PrevViewProjection * world;

            vec2 velocity = texelFetch(u_Velocity, closestPixel, 0).xy;
            if (closest <= 0.0)
                velocity = ScreenUVToNdc(uv) - prevClip.xy / prevClip.w;

            imageStore(u_DilatedVelocity, pixel, vec4(velocity, 0.0, 0.0));
            imageStore(u_ClosestDepth, pixel, vec4(closest));

            uint encodedJacobian = 0u;
            uint encodedBoundary = 0u;
            if (u_ReprojectionField > 0.5)
            {
                for (int y = -1; y <= 1; ++y)
                    for (int x = -1; x <= 1; ++x)
                        s_PixelVelocity[y + 1][x + 1] =
                             TsrNdcToPixelVelocity(NdcVelocityAt(pixel + ivec2(x, y), s_Depth[y + 1][x + 1]));
                encodedJacobian = EncodeReprojectionJacobian(ComputeReprojectionJacobian());
                const vec2 centreVelocity = NdcVelocityAt(pixel, s_Depth[1][1]);
                encodedBoundary =
                     EncodeReprojectionBoundary(closestOffset, ComputeBoundary(pixel, centreVelocity, velocity));
            }
            imageStore(u_ReprojectionJacobian, pixel, uvec4(encodedJacobian));
            imageStore(u_ReprojectionBoundary, pixel, uvec4(encodedBoundary));

            if (closest <= 0.0 || prevClip.w <= 0.0)
                return;
            const float prevDepth = clamp(prevClip.z / prevClip.w, 0.0, 1.0);
            const vec2  prevPixel = (uv - velocity * vec2(0.5, -0.5)) * u_RenderSize - 0.5;
            const ivec2 base      = ivec2(floor(prevPixel));
            for (int i = 0; i < 4; ++i)
            {
                const ivec2 q = base + ivec2(i & 1, i >> 1);
                if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, ivec2(u_RenderSize))))
                    continue;
                // Positive floats order as their bit patterns: atomicMax on the bits keeps the nearest.
                atomicMax(u_PrevAtomic[q.y * int(u_RenderSize.x) + q.x], floatBitsToUint(prevDepth));
            }
        }
    }
}
