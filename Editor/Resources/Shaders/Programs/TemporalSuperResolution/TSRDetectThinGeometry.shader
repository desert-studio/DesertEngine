// DesertAsset {"Kind":"Shader","Guid":"b9148a2dcc67417c98f0c2afcdf0d648","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TSRDetectThinGeometry"
{
    Compute
    {
        // TSR.DetectThinGeometry (the project's r.TSR.ThinGeometryDetection, Project::RendererSettings). Ported from
        // UE 5.8 Engine/Shaders/Private/TemporalSuperResolution/TSRDetectThinGeometry.usf (DetectThinGeometryCS
        // :323-600, FetchCommonInput :109-148, FetchRegularPassInput :149-210, HorizontalThinEdge3x3 /
        // VerticalThinEdge3x3 :212-290, EncodeThinGeometryWeight :292-308) and TSRThinGeometryCommon.ush
        // (EncodeCoverage), with the 3x3 operators of TSRConvolutionNetwork.ush (WeightedSum3x3 :592, Sum3x3,
        // WeightedAvg3x3, Min3x3, Max3x3). Adapted:
        //  - one 16x16 tile per 8x8 group, four pixels per lane (UE: LANE_COUNT lanes x SIMD_SIZE elements on a
        //    32x32 tile), the 3x3 neighbours read from shared memory; UE's tile overscan (r.TSR.ShadingRejection.
        //    TileOverscan 3), so a group writes its inner 10x10;
        //  - the current coverage is the base passes' (u_ThinGeometryCoverage, Common/ThinGeometryCoverage.glslh)
        //    instead of TSRMeasureCoverage.usf's slice of the anti-flickering luma array;
        //  - the reprojection offset is the low 4 bits of TSR.ReprojectionBoundary (TSR.DilateVelocity writes it
        //    with thin geometry on, UE's R8Output ThinGeometryTextureIndex slice), and "has an offset" is it being
        //    non-zero (UE: R8Output slice 0 bit 0x80, the same test in TSRDilateVelocity.usf :226);
        //  - no pixel animation: our velocity carries no UE "pixel animation" bit (DecimateMask 0x4), so
        //    bHasPixelAnimation is false for every pixel, as in UE for a material that declares none;
        //  - UE's intensity-line (MinKeepLineContrast) and translucency (RejectTranslucency) permutations are not
        //    ported: this is DIM_INTENSITY_LINE_DETECTION 0 and DIM_REJECT_TRANSLUCENCY_DETECTION 0.
        // TSR_THIN_VARYING_RANGE (UE DIM_VARYING_RANGE_DETECTION: ShadingRange VaryingRange with the flickering
        // heuristic on): regular materials get edge-only relaxation, foliage the dilated one. 0: UE ShadingRange
        // Foliage (binary coverage: only foliage counts).
        // Writes u_ThinGeometry (UE R8Output[ThinGeometryTextureIndex]: weight * 127 << 1 | cluster-hole bit) and
        // this frame's coverage history u_CoverageOut (UE HistoryCoverageOutput, EncodeCoverage with no flags).
        LocalSize(8, 8, 1);

        #include <Common/TSRCommon.glslh>

        Uniform(1) sampler2D u_SceneDepth;            // reversed-Z device depth, RenderExtent
        Uniform(2) sampler2D u_ThinGeometryCoverage;  // the base passes': 0 / 0.5 / 1
        Uniform(3) sampler2D u_ReprojectedCoverage;   // TSR.DecimateHistory: last frame's coverage history
        Uniform(4) usampler2D u_ReprojectionBoundary; // TSR.DilateVelocity: low 4 bits the reprojection offset
        layout(binding = 5, r32ui) restrict writeonly uniform uimage2D u_ThinGeometry;
        layout(binding = 6, r32f) restrict writeonly uniform image2D u_CoverageOut;

        #ifndef TSR_THIN_VARYING_RANGE
        #define TSR_THIN_VARYING_RANGE 0
        #endif

        const int kTileSize   = 16;
        const int kOverscan   = 3; // UE r.TSR.ShadingRejection.TileOverscan
        const int kTileOutput = kTileSize - 2 * kOverscan;
        const int kLanes      = 64;
        const int kE          = kTileSize * kTileSize / kLanes; // pixels per lane

        // UE TemporalSuperResolution.cpp CVar defaults: r.TSR.ThinGeometryDetection.ErrorMultiplier (200) and
        // .Coverage.MaxRelaxationWeight (0.037).
        const float kErrorMultiplier     = 200.0;
        const float kMaxRelaxationWeight = 0.037;

        shared float s_A[kTileSize * kTileSize];
        shared float s_B[kTileSize * kTileSize];
        shared float s_C[kTileSize * kTileSize];
        shared uint  s_Any;

        int Lane()
        {
            return int(gl_LocalInvocationIndex);
        }
        int ElementIndex(int e)
        {
            return Lane() + e * kLanes;
        }
        ivec2 ElementLocal(int e)
        {
            const int i = ElementIndex(e);
            return ivec2(i % kTileSize, i / kTileSize);
        }
        int TapIndex(int e, ivec2 o)
        {
            const ivec2 q = clamp(ElementLocal(e) + o, ivec2(0), ivec2(kTileSize - 1));
            return q.y * kTileSize + q.x;
        }

        // UE AnyThread: one value for the whole group (every lane calls it; the barriers are group-uniform).
        bool AnyInGroup(bool value)
        {
            barrier();
            if (Lane() == 0)
                s_Any = 0u;
            barrier();
            if (value)
                atomicOr(s_Any, 1u);
            barrier();
            return s_Any != 0u;
        }

        void Publish(float a[kE], float b[kE], float c[kE])
        {
            barrier();
            for (int e = 0; e < kE; ++e)
            {
                s_A[ElementIndex(e)] = a[e];
                s_B[ElementIndex(e)] = b[e];
                s_C[ElementIndex(e)] = c[e];
            }
            barrier();
        }

        // UE WeightedSum3x3 (separable: the vertical weights, then the horizontal ones), on all three channels.
        void WeightedSum3x3(inout float a[kE], inout float b[kE], inout float c[kE], vec3 weights)
        {
            Publish(a, b, c);
            for (int e = 0; e < kE; ++e)
            {
                vec3 sum = vec3(0.0);
                for (int y = -1; y <= 1; ++y)
                {
                    for (int x = -1; x <= 1; ++x)
                    {
                        const int t = TapIndex(e, ivec2(x, y));
                        sum += weights[x + 1] * weights[y + 1] * vec3(s_A[t], s_B[t], s_C[t]);
                    }
                }
                a[e] = sum.x;
                b[e] = sum.y;
                c[e] = sum.z;
            }
        }

        // UE Max3x3 (@p takeMax) / Min3x3, on all three channels.
        void Extremum3x3(inout float a[kE], inout float b[kE], inout float c[kE], bool takeMax)
        {
            Publish(a, b, c);
            for (int e = 0; e < kE; ++e)
            {
                vec3 extremum = vec3(a[e], b[e], c[e]);
                for (int y = -1; y <= 1; ++y)
                {
                    for (int x = -1; x <= 1; ++x)
                    {
                        const int  t   = TapIndex(e, ivec2(x, y));
                        const vec3 tap = vec3(s_A[t], s_B[t], s_C[t]);
                        extremum       = takeMax ? max(extremum, tap) : min(extremum, tap);
                    }
                }
                a[e] = extremum.x;
                b[e] = extremum.y;
                c[e] = extremum.z;
            }
        }

        // UE HorizontalThinEdge3x3 + VerticalThinEdge3x3 (EDGE_TYPE EDGE_HORIZONTAL_VERTICAL_DIAGNAL) of the
        // device depth against each pixel's own threshold.
        void ThinEdge3x3(float depth[kE], float threshold[kE], out float edge[kE])
        {
            float unused[kE];
            Publish(depth, threshold, depth);
            float potentialH[kE]; // centre above its top and bottom neighbours
            float potentialV[kE]; // centre above its left and right neighbours
            for (int e = 0; e < kE; ++e)
            {
                const float c = depth[e];
                const float t = threshold[e];
                potentialH[e] = (c - s_A[TapIndex(e, ivec2(0, -1))] > t ? 1.0 : 0.0) +
                                (c - s_A[TapIndex(e, ivec2(0, 1))] > t ? 1.0 : 0.0);
                potentialV[e] = (c - s_A[TapIndex(e, ivec2(-1, 0))] > t ? 1.0 : 0.0) +
                                (c - s_A[TapIndex(e, ivec2(1, 0))] > t ? 1.0 : 0.0);
                unused[e]     = 0.0;
            }
            Publish(potentialH, potentialV, unused);
            for (int e = 0; e < kE; ++e)
            {
                // The centre must be an edge itself; at most one side of the line may not be one.
                const float leftRight = (s_A[TapIndex(e, ivec2(-1, 0))] + s_A[TapIndex(e, ivec2(1, 0))]) *
                                        clamp(potentialH[e] - 1.0, 0.0, 1.0);
                const float topBottom = (s_B[TapIndex(e, ivec2(0, -1))] + s_B[TapIndex(e, ivec2(0, 1))]) *
                                        clamp(potentialV[e] - 1.0, 0.0, 1.0);
                edge[e] = clamp((leftRight - 2.0) * 0.5, 0.0, 1.0) + clamp((topBottom - 2.0) * 0.5, 0.0, 1.0);
            }
        }

        // UE InterleavedGradientNoise (Random.ush).
        float InterleavedGradientNoise(vec2 uv, float frame)
        {
            uv += frame * (vec2(47.0, 17.0) * 0.695);
            const vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
            return fract(magic.z * fract(dot(uv, magic.xy)));
        }

        void main()
        {
            const ivec2 origin = ivec2(gl_WorkGroupID.xy) * kTileOutput - kOverscan;
            const ivec2 size   = ivec2(u_RenderSize);

            ivec2 pixel[kE];
            float coverage[kE];
            uint  materialMask[kE];
            bool  anyCoverage = false;
            for (int e = 0; e < kE; ++e)
            {
                pixel[e] = clamp(origin + ElementLocal(e), ivec2(0), size - 1);
                // FetchCommonInput: the coverage counts only where the closest-depth dilation moved the pixel,
                // at the max of its own and the dilated neighbour's.
                const ivec2 offset    = DecodeReprojectionOffset(texelFetch(u_ReprojectionBoundary, pixel[e], 0).r);
                const bool  hasOffset = any(notEqual(offset, ivec2(0)));
                const float own       = texelFetch(u_ThinGeometryCoverage, pixel[e], 0).r;
                const float dilated   = texelFetch(u_ThinGeometryCoverage, TsrClampRender(pixel[e] + offset), 0).r;
                const float current   = hasOffset ? max(own, dilated) : 0.0;
#if TSR_THIN_VARYING_RANGE
                materialMask[e] = uint(current * 2.01);
                coverage[e]     = ceil(current);
#else
                materialMask[e] = 0u;
                coverage[e]     = floor(current); // UE ToBinaryCoverage
#endif
                anyCoverage = anyCoverage || coverage[e] > 0.0;
            }

            float edge[kE];
            float relaxation[kE];
            bool  clusterHole[kE];
            float history[kE];
            for (int e = 0; e < kE; ++e)
            {
                edge[e]        = 0.0;
                relaxation[e]  = 0.0;
                clusterHole[e] = false;
                history[e]     = 0.0;
            }

            if (AnyInGroup(anyCoverage)) // UE bRegularPass
            {
                float depth[kE];
                float threshold[kE];
                for (int e = 0; e < kE; ++e)
                {
                    depth[e] = texelFetch(u_SceneDepth, pixel[e], 0).r;
                    // The sky (depth 0) is no edge: its pixel error is unbounded.
                    const float error = depth[e] > 0.0 ? TsrPixelDeviceZError(depth[e]) : 0.0;
#if TSR_THIN_VARYING_RANGE
                    threshold[e] = error * max(kErrorMultiplier * 0.05, 1.0);
#else
                    threshold[e] = error * kErrorMultiplier;
#endif
                    history[e] = texelFetch(u_ReprojectedCoverage, pixel[e], 0).r;
                }
                ThinEdge3x3(depth, threshold, edge);

                // Student t test on the Bernoulli distribution of the 3x3 neighbours: history and current coverage
                // of the same distribution, or the history is rejected.
                const float newSampleWeight = 0.1;
                float       sumHistory[kE];
                float       sumCurrent[kE];
                float       sumVariance[kE];
                for (int e = 0; e < kE; ++e)
                {
                    sumHistory[e]  = history[e];
                    sumCurrent[e]  = coverage[e];
                    sumVariance[e] = history[e] * (1.0 - history[e]);
                }
                WeightedSum3x3(sumHistory, sumCurrent, sumVariance, vec3(1.0));
                float t[kE];
                bool  sameDistribution[kE];
                for (int e = 0; e < kE; ++e)
                {
                    const float average = sumHistory[e] - sumCurrent[e];
                    const float s       = sqrt(sumVariance[e] + 0.000001) *
                                    sqrt(newSampleWeight / (2.0 - newSampleWeight) * 72.0);
                    t[e]                = abs(average > 0.0 ? average / s : 0.0);
                    sameDistribution[e] = t[e] < 20.754; // 2.306 (p = 0.05) x 9
                }

                // Max5x5 of T and of the current coverage (two Max3x3).
                float maxT[kE];
                float maxCoverage[kE];
                float unused[kE];
                for (int e = 0; e < kE; ++e)
                {
                    maxT[e]        = t[e];
                    maxCoverage[e] = coverage[e];
                    unused[e]      = 0.0;
                }
                Extremum3x3(maxT, maxCoverage, unused, true);
                Extremum3x3(maxT, maxCoverage, unused, true);

                bool validRegion[kE];
                for (int e = 0; e < kE; ++e)
                {
                    validRegion[e]              = maxCoverage[e] > 0.0;
                    const bool notAbsoluteMatch = maxT[e] > 0.0;
                    const bool acceptHistory    = validRegion[e] && sameDistribution[e] && notAbsoluteMatch;
                    history[e]     = mix(history[e], coverage[e], acceptHistory ? newSampleWeight : 1.0);
                    clusterHole[e] = history[e] > 0.99 / 127.0;
                    // The relaxation weight of the thin-geometry cluster: largest around half coverage.
                    const float w = abs(history[e] - 0.5) + 0.02;
                    relaxation[e] = w * exp(w * -20.0) * (10.0 / 0.184);
                }

                // Min5x5 of the updated history (two Min3x3): relax only inside a thin-geometry history region.
                float minHistory[kE];
                for (int e = 0; e < kE; ++e)
                    minHistory[e] = history[e];
                Extremum3x3(minHistory, unused, unused, false);
                Extremum3x3(minHistory, unused, unused, false);
                for (int e = 0; e < kE; ++e)
                {
                    const bool validRelaxation = minHistory[e] > 0.0 && validRegion[e] && sameDistribution[e];
                    relaxation[e]              = validRelaxation ? relaxation[e] : 0.0;
                }

#if TSR_THIN_VARYING_RANGE
                float nonFoliageEdge[kE];
                float nonFoliageRelaxation[kE];
                bool  anyFoliage = false;
                for (int e = 0; e < kE; ++e)
                {
                    nonFoliageEdge[e]       = edge[e];
                    nonFoliageRelaxation[e] = relaxation[e];
                    anyFoliage              = anyFoliage || materialMask[e] == 2u;
                }
                if (AnyInGroup(anyFoliage))
#endif
                {
                    // Dilate the edge with (0.8, 1, 0.8) and diffuse the weight a little, capped by
                    // MaxRelaxationWeight.
                    WeightedSum3x3(edge, relaxation, unused, vec3(0.8, 1.0, 0.8));
                    for (int e = 0; e < kE; ++e)
                        relaxation[e] = clamp(relaxation[e] * kMaxRelaxationWeight, 0.0, 1.0);
                    WeightedSum3x3(relaxation, unused, unused, vec3(0.5, 1.0, 0.5));
                    for (int e = 0; e < kE; ++e)
                    {
                        relaxation[e] = clamp(relaxation[e], 0.0, 1.0);
#if TSR_THIN_VARYING_RANGE
                        // Regular materials keep the edge-only relaxation.
                        if (materialMask[e] == 1u)
                        {
                            edge[e]       = nonFoliageEdge[e];
                            relaxation[e] = nonFoliageRelaxation[e];
                        }
#endif
                    }
                }
                for (int e = 0; e < kE; ++e)
                    relaxation[e] = sameDistribution[e] ? relaxation[e] : 0.0;
            }
            else
            {
                // No thin-geometry indicator in the tile: the coverage history becomes the current coverage.
                for (int e = 0; e < kE; ++e)
                    history[e] = coverage[e];
            }

            for (int e = 0; e < kE; ++e)
            {
                const ivec2 local = ElementLocal(e);
                const ivec2 outPixel = origin + local;
                if (any(lessThan(local, ivec2(kOverscan))) || any(greaterThanEqual(local, ivec2(kTileSize - kOverscan))) ||
                    any(greaterThanEqual(outPixel, size)))
                    continue;
                // UE EncodeThinGeometryWeight: 7-bit weight | 1 bit (0 edge line, 1 cluster-hole region).
                float      weight = clusterHole[e] ? relaxation[e] : 0.0;
                const bool isEdge = edge[e] > 0.0;
                weight            = isEdge ? edge[e] : weight;
                const uint hole   = (!isEdge && clusterHole[e]) ? 1u : 0u;
                imageStore(u_ThinGeometry, outPixel, uvec4((uint(weight * 127.0) << 1u) | hole));
                // UE EncodeCoverage(Coverage, false, Dither): 8 bits with a dither.
                const float dither = InterleavedGradientNoise(vec2(outPixel), u_ThinGeometryFrameIndex);
                const float bits   = floor(clamp(clamp(history[e], 0.0, 1.0) * 255.0 + dither, 0.0, 255.0));
                imageStore(u_CoverageOut, outPixel, vec4(bits / 255.0));
            }
        }
    }
}
