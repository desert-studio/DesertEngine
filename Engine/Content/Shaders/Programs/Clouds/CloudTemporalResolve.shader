// DesertAsset {"Kind":"Shader","Guid":"f8d0506cdd2a8abd51f33eed80de242f","Versions":{"SHDR":1},"Dependencies":[]}
Shader "CloudTemporalResolve"
{
    Compute
    {
        // THE TEMPORAL RECONSTRUCTION — Unreal's VolumetricRenderTarget mode 0, reconstruct stage.
        //
        // CloudRaymarch.shader traces at a QUARTER of the framebuffer's size and jitters its rays so that
        // each quarter-res texel holds one of the four HALF-resolution pixels of its 2x2 block, a
        // different one every frame, walking {0, 2, 3, 1}. This pass turns that stream into a HALF-
        // resolution image, and CloudComposite.shader upsamples the result exactly as it upsampled the
        // half-res trace before this stage existed.
        //
        // For each half-res pixel there are three cases:
        //
        //   1. THIS FRAME OWNS IT. The pixel's sub-pixel index equals the frame's, so the quarter-res
        //      texel above it holds a ray cast through this very pixel. Its value is fetched exactly,
        //      with a texelFetch and no filtering — it is the only sample in the whole scheme that is not
        //      an estimate.
        //
        //   2. HISTORY IS USABLE. Walk the pixel's ray to the guide's CLOUD FRONT distance, project that
        //      world point with the previous frame's view-projection — Unreal's ClipToPrevClip, factored
        //      so the walk can happen in world space — and read the reconstruction of the frame before.
        //      This frame's sample is then blended into it, by kOwnedSampleWeight for case 1 and by the
        //      much smaller kNewSampleWeight otherwise; see those constants for why the two differ.
        //
        //   3. NO HISTORY TO READ — the first frame after the targets were allocated, or a reprojection
        //      that lands off screen or behind the previous eye. Take the bilinear reconstruction of the
        //      quarter-res trace, radiance and guide together, which is exactly what Unreal does in the
        //      same position (VolumetricRenderTarget.usf:500-507: "History is invalid so simply use this
        //      frame low resolution render with bilinear sampling"). A history that was READ and then
        //      REJECTED is not this case — see the validation list below.
        //
        // THE GUIDE IS LOADED, NEVER FILTERED, except in case 3. Unreal point-loads its depth guide for
        // EVERY pixel, both for the reprojection (VolumetricRenderTarget.usf:286) and as the frame's own
        // depth data (:391); the one bilinear read of it is :506, the lost-history branch above, where it
        // rides along with a bilinear radiance because there is nothing better to offer. Filtering it
        // anywhere else averages the distance of a cloud edge with the distance of the sky behind it and
        // produces a number that is neither — VolumetricCloudCommon.ush:89-92 is Epic's own note on the
        // harm, and CloudRaymarch.shader:74-75 is ours. Both of the guide's readers judge EDGES by it,
        // so an averaged distance makes the edge filter fire along every silhouette in the frame.
        //
        // WHY THE FRONT DISTANCE AND NOT A VELOCITY BUFFER. There is no velocity buffer, and clouds do
        // not want one: Unreal reprojects the whole layer as a single front surface, because a volume has
        // no single motion vector and the front is what the eye tracks. The guide's .x channel is that
        // surface.
        //
        // WHAT IS VALIDATED. Three rules, and each of the three is Unreal's — but Unreal has more than
        // three, so this list is a SUBSET and not a port:
        //   * the reprojected UV must land on screen — off-screen history is not history, it is the edge
        //     texel repeated, and this engine's samplers are REPEAT, so it would be the OPPOSITE edge.
        //     Unreal's bValidPreviousUVs, VolumetricRenderTarget.usf:298;
        //   * the scene distance must not have jumped between the history and this frame — that is a
        //     DISOCCLUSION, geometry that moved in front of or out from behind the cloud. Unreal splits
        //     the same test into its two directions at :416 and :423 and uses the same 2 km threshold
        //     (:406); ours is the symmetric form of the pair;
        //   * neither the history colour nor its guide may be NaN or Inf. One such texel, blended
        //     forward, poisons an ever-growing region of the screen for the rest of the session, which is
        //     the single most expensive failure mode a history buffer has. Unreal's :491.
        //
        // WHAT UNREAL VALIDATES AND THIS PASS DOES NOT, so that the list above is not read as complete:
        // a minimum reprojection distance (:299), the whole min/max-depth family (:402-427, :480), the
        // eight-neighbour DILATION toward the closest scene depth (:543-558) and the optional
        // neighbourhood colour box (:570+).
        //
        // ALL FOUR WERE BUILT AND MEASURED — task HV, CALIBRATION.md §HV — AND NONE IS ADOPTED. This
        // paragraph is a result rather than a plan, and the reasons differ from each other:
        //
        //   * TWO OF THE FOUR ARE NOT IN OUR CONFIGURATION OF UNREAL AT ALL. :398 and :515 split the
        //     reconstruct body on PERMUTATION_CLOUD_MIN_AND_MAX_DEPTH; the dilation and the colour box are
        //     in the #else, and mode 0 with compute — which is exactly what this pass is — takes the #if.
        //   * THE MIN/MAX IS SCENE DEPTH, NOT CLOUD DEPTH. This comment said "cloud" for two phases and it
        //     was wrong: VolumetricCloud.usf:594-606 fills it from SceneDepthMinAndMaxTexture, the opaque
        //     Z-buffer's range over the block one trace texel covers. That family's headline rule (:408,
        //     "removing cloud over trees") is also unreachable in Unreal itself — the march writes .y and
        //     .w from one expression, so the second half of :409 reduces to `abs(x) < 0`.
        //   * THE GATE at :402, the one piece of that family needing no new channel, was implemented and
        //     is NET NEGATIVE here: it moved 0.486 % of one motion frame and raised the error against
        //     ground truth from 6.7142 to 6.7189.
        //   * THE COLOUR BOX costs 0.024 ms of a 0.179 ms pass and changes up to 6.35 % of a shipped still
        //     frame, while improving one motion path of four and making another worse.
        //   * THE MINIMUM REPROJECTION DISTANCE is the one with a real artefact behind it: a climb into
        //     the deck leaves a blocky patch of stale history over the horizon band, and the check removes
        //     it — the error against ground truth falls a tenth at 2 km and a third at Unreal's suggested
        //     4 km. It is still not adopted, because it cannot be spent as a constant: 4 km damages a
        //     tenth of a plain zenith frame, the largest value that costs nothing here (2 km) sits just
        //     under THIS layer's 2.2 km base, and five of the nine shipped cloud types have bases below
        //     that. Unreal ships it off for the same trade (cvar default 0.0f,
        //     VolumetricRenderTarget.cpp:63). What would make it adoptable is a threshold DERIVED from
        //     the layer rather than a constant, which is one float in CloudResolveParams; that is a
        //     proposal in §HV, not a plan hidden here.
        //
        // The disocclusion test above is, on the same measurements, INERT over open sky — removing it
        // entirely leaves every motion path byte-identical — and ALIVE at opaque silhouettes, where it
        // fires on 1.27 % of a pan and improves the frame. That is why it stays and why nothing was built
        // on top of it.
        //
        // LINEAR HDR IN, LINEAR HDR OUT. Premultiplied radiance in .rgb, TRANSMITTANCE in .a, exactly as
        // the march produced it and the composite expects it. Blending a premultiplied pair componentwise
        // is legitimate: both channels are linear in the same integral.

        // Included for CLOUD_WORLD_UNITS_PER_KM alone. Written out as a literal here instead, it would be
        // the second copy of a conversion factor, and this programme has already paid twice for two places
        // that had to agree and did not.
        #include <Common/CloudGeometry.glslh>

        // The reconstruction's two outputs, both HALF resolution. rgba16f for the same reason the march's
        // targets are: radiance is pre-tonemap HDR and transmittance is in [0,1].
        layout(binding = 4, rgba16f) restrict writeonly uniform image2D u_ReconstructedScatter;

        // The reconstructed guide, which serves two readers: CloudComposite.shader upsamples through it,
        // and NEXT frame's run of this pass reads it back as the history guide to detect disocclusions.
        // It carries THIS frame's distances with no temporal blending at all — a distance is a property of
        // the geometry in front of the camera now, and mixing it with a distance from three frames ago
        // would produce a number describing nothing, which the composite would then treat as an edge.
        layout(binding = 5, rgba16f) restrict writeonly uniform image2D u_ReconstructedGuide;

        // This frame's QUARTER-resolution trace and its guide.
        Uniform(0) sampler2D u_CloudTrace;
        Uniform(1) sampler2D u_CloudTraceGuide;

        // The PREVIOUS frame's half-resolution reconstruction and its guide. On the first frame after the
        // targets are allocated these are bound to the engine's fallback texture rather than left unbound
        // — a declared sampler with no image is an INVALID descriptor set, not an unused one, and this
        // backend answers an invalid set by skipping the entire dispatch, which would lose the clouds with
        // nothing in the log. u_HistoryValid is what says the bytes mean anything.
        Uniform(2) sampler2D u_CloudHistory;
        Uniform(3) sampler2D u_CloudHistoryGuide;

        // The C++ side of this block is Graphic::CloudResolveParams (Engine/Graphic/Clouds/CloudPayload.hpp),
        // where a static_assert pins every offset. Raw std430 rather than the ReadBuffer(n) sugar, as
        // CloudParams.glslh does, because this header describes a structure.
        //
        // A BUFFER AND NOT A PUSH CONSTANT: two 4x4 matrices are already 128 bytes, which is the whole
        // size Vulkan guarantees for push constants, and several desktop drivers report exactly that
        // minimum. The camera and the frame state would not fit.
        layout(std430, binding = 6) readonly buffer CloudResolveBuffer
        {
            mat4  u_InverseViewProjection; // clip -> world, this frame
            mat4  u_PrevViewProjection;    // world -> clip, the previous frame
            vec3  u_CameraPosition;        // world units (centimetres)
            float u_HistoryValid;          // 1 when the history targets hold a real previous frame
            ivec2 u_SubPixelOffset;        // this frame's traced sub-pixel, {0,1} x {0,1}
        };

        LocalSize(8, 8, 1);

        // THE DISOCCLUSION THRESHOLD, kilometres. Unreal rejects a history sample whose scene depth has
        // moved by more than a couple of kilometres at cloud scale; below that the difference is the
        // guide's own quantisation and the camera's motion, above it something has come between the
        // camera and the layer. Absolute rather than relative, deliberately: the quantity is a SCENE
        // distance, and at sky distances a relative threshold would accept a mountain appearing in front
        // of a cloud thirty kilometres away.
        const float kDisocclusionKm = 2.0f;

        // HOW MUCH OF THIS FRAME REPLACES A VALIDATED HISTORY SAMPLE. Two weights, because the two cases
        // carry very different information, and both numbers were measured rather than assumed — the
        // sweep is in the task report.
        //
        //   OWNED. The quarter-res texel above this pixel holds a ray cast through this very pixel, so
        //   the sample is exact. It is still not taken whole: each trace carries one realisation of the
        //   march's start-offset dither, and replacing outright preserves that realisation intact, which
        //   is the speckle this work exists to remove. Blending successive realisations is the only place
        //   in the scheme where the dither is actually AVERAGED rather than merely held.
        //
        //   NOT OWNED. All that is available is a bilinear reconstruction of a grid that was traced
        //   through DIFFERENT sub-pixels, so its sampling position moves every frame; feeding much of it
        //   in puts that movement into every pixel of the screen. It is admitted only enough to stop a
        //   pixel drifting away from the truth between the frames that own it — at zero the image
        //   visibly ghosts (measured: 4.7x the frame-to-frame difference of the pass this replaces).
        //
        // RAISED FROM 0.25 AFTER MEASURING THE AXIS THE FIRST SWEEP DID NOT HAVE.
        //
        // That sweep chose 0.25 because it minimised sigma. Sigma cannot tell a cloud that lost its
        // noise from one that lost its edges — both shrink the variance of a neighbourhood — so
        // minimising it walks straight into blur, and the frames at 0.25 were visibly soft. Measuring
        // the gradient energy of the far-field band against a pre-temporal render, on the settled
        // frame, prices it:
        //
        //     owned   sigma    edge energy     vs pre-temporal
        //     0.25    6.253      3.4631          -10.9 %
        //     0.50    6.448      3.8683           -0.5 %
        //     0.75    6.558      4.0943           +5.3 %
        //     1.00    6.633      4.2560           +9.4 %
        //
        // 0.50 is where the detail merely comes BACK; 0.75 is where it ends up ahead of where the pass
        // found it, and 2 % more noise is a fair price for 5 % more edge on a subject made of edges.
        // It is not a compromise between the two either: the original sweep's own inter-frame |dY|
        // column puts 0.50 at 0.1199-0.1499 against 0.25's 0.1270 — the same temporal stability, from
        // twice the weight. The quarter weight was buying softness, not steadiness.
        //
        // The ceiling above this is structural rather than a number to turn: the march traces at a
        // QUARTER of the framebuffer and this pass reconstructs a half-resolution image from four such
        // frames. Wanting more than +5 % means either tracing at a higher resolution — which is the
        // cost the quarter-res trace was adopted to avoid — or a neighbourhood clamp on the history,
        // which is the standard way to hold detail at a higher weight and is not implemented yet.
        //
        // Setting kOwnedSampleWeight to 1 is the pure hand-over-the-new-sample scheme, and it still
        // improves on the pass this replaces; it just improves half as much. The numbers are in the
        // report and the constant is here so the choice can be re-measured rather than re-argued.
        const float kOwnedSampleWeight = 0.75f;
        const float kNewSampleWeight   = 0.06f;

        // Half a texel in, on both axes. This engine's samplers are REPEAT, so a uv inside [0,1] but
        // within half a texel of the border still gathers from the OPPOSITE edge of the image — a bright
        // fringe along one side that no amount of staring at the march explains.
        vec2 ClampToTexelCentres(vec2 uv, ivec2 size)
        {
            vec2 half_texel = 0.5f / vec2(size);
            return clamp(uv, half_texel, vec2(1.0f, 1.0f) - half_texel);
        }

        bool IsFinite(vec4 v)
        {
            return !any(isnan(v)) && !any(isinf(v));
        }

        void main()
        {
            ivec2 size  = imageSize(u_ReconstructedScatter);
            ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
            if (coord.x >= size.x || coord.y >= size.y)
                return;

            ivec2 traceSize = textureSize(u_CloudTrace, 0);

            // Which sub-pixel of its 2x2 block this half-res pixel is, and the quarter-res texel that
            // covers the block. `& 1` and `>> 1` rather than `% 2` and `/ 2`: the coordinates are
            // non-negative by construction and the bit forms have no sign-handling to get wrong.
            ivec2 subPixel   = coord & ivec2(1, 1);
            ivec2 traceCoord = coord >> ivec2(1, 1);

            bool owned = subPixel == u_SubPixelOffset;

            // THIS FRAME'S LOW-RESOLUTION ESTIMATE, with the jitter undone. A quarter-res texel does not
            // hold the value at its own centre — it holds the value at the half-res pixel
            // (2 * texel + offset + 0.5), which is where its ray was cast. Sampling the texture at this
            // pixel's position directly would therefore be wrong by half a half-res pixel in a direction
            // that CHANGES EVERY FRAME, and a reconstruction that wobbles with the jitter is precisely
            // the artefact this pass exists to remove. Inverting the placement instead costs one
            // subtraction: the continuous texel index whose sample lands on this pixel is
            // (halfPos - offset - 0.5) / 2.
            vec2 halfPos    = vec2(coord) + vec2(0.5f, 0.5f);
            vec2 traceTexel = (halfPos - vec2(u_SubPixelOffset) - vec2(0.5f, 0.5f)) * 0.5f;
            vec2 traceUv    = ClampToTexelCentres((traceTexel + vec2(0.5f, 0.5f)) / vec2(traceSize), traceSize);

            // Case 1 for the RADIANCE: the pixel this frame traced gets the exact texel, fetched rather
            // than filtered. The other three of the block have no sample of their own this frame, so the
            // bilinear reconstruction of the quarter-res grid is all that exists for them.
            vec4 traceScatter = owned ? texelFetch(u_CloudTrace, traceCoord, 0) : texture(u_CloudTrace, traceUv);

            // THE GUIDE IS LOADED FOR EVERY PIXEL, owned or not — Unreal's :286 and :391, and see the
            // header for why filtering it is harmful rather than merely approximate. The block's four
            // pixels therefore share one distance, which is exactly what its readers want: a distance that
            // agrees with its neighbours costs the composite nothing, while a bilinear blend of the two
            // sides of a silhouette agrees with neither and fires the edge path along the whole silhouette.
            vec4 traceGuide = texelFetch(u_CloudTraceGuide, traceCoord, 0);

            float newWeight = owned ? kOwnedSampleWeight : kNewSampleWeight;

            // Every rejection below leaves this value in place: a history that was read and found to
            // describe a different surface is answered with THIS frame's sample. That is Unreal's rule
            // too — a failed disocclusion test there sets bUseNewSample — and it is the reason there is no
            // search here: a neighbour that passes validation belongs to a different surface, and the
            // smear it produces follows the camera.
            vec4 resolved = traceScatter;

            // Whether there was a history to read AT ALL, which is a different question from whether it
            // was accepted. Only the first sends the pixel down case 3.
            bool historyRead = false;

            if (u_HistoryValid > 0.5f)
            {
                // The pixel's ray, from this frame's inverse view-projection. REVERSED-Z, like everything
                // else in this engine (Core/Projection.hpp): 1 is the near plane and 0 the far one.
                vec2 uv  = halfPos / vec2(size);
                vec2 ndc = vec2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

                vec4 nearH = u_InverseViewProjection * vec4(ndc.x, ndc.y, 1.0f, 1.0f);
                vec4 farH  = u_InverseViewProjection * vec4(ndc.x, ndc.y, 0.0f, 1.0f);
                vec3 nearP = nearH.xyz / max(nearH.w, 1e-9f);
                vec3 farP  = farH.xyz / max(farH.w, 1e-9f);
                vec3 rayDir = normalize(farP - nearP);

                // THE SINGLE FRONT SURFACE. The guide's .x is where this ray first met material, or the
                // end of its search when it met none; either way it is a real distance, so the point
                // below is always on the ray and never a sentinel projected into the previous frame.
                vec3 frontWorld = u_CameraPosition +
                                  rayDir * (traceGuide.x * CLOUD_WORLD_UNITS_PER_KM);

                vec4 prevClip = u_PrevViewProjection * vec4(frontWorld, 1.0f);

                // Behind the previous camera's eye. Dividing by a w at or below zero mirrors the point
                // through the origin and lands it somewhere plausible on screen, which is the worst
                // possible failure: a confident wrong answer rather than a rejected one.
                if (prevClip.w > 1e-6f)
                {
                    vec2 prevNdc = prevClip.xy / prevClip.w;
                    vec2 prevUv  = vec2(prevNdc.x * 0.5f + 0.5f, 0.5f - prevNdc.y * 0.5f);

                    bool onScreen = all(greaterThanEqual(prevUv, vec2(0.0f, 0.0f))) &&
                                    all(lessThanEqual(prevUv, vec2(1.0f, 1.0f)));

                    if (onScreen)
                    {
                        historyRead = true;

                        vec2 fetchUv = ClampToTexelCentres(prevUv, size);

                        vec4 history      = texture(u_CloudHistory, fetchUv);
                        vec4 historyGuide = texture(u_CloudHistoryGuide, fetchUv);

                        bool finite      = IsFinite(history) && IsFinite(historyGuide);
                        bool sameSurface = abs(historyGuide.y - traceGuide.y) <= kDisocclusionKm;

                        if (finite && sameSurface)
                            resolved = mix(history, traceScatter, newWeight);
                    }
                }
            }

            // CASE 3, and only case 3: there was no history to reproject from — the first frame after the
            // targets were allocated, or a point that leaves the screen or falls behind the previous eye.
            // Unreal answers it with a bilinear read of this frame's trace, guide and radiance together
            // (VolumetricRenderTarget.usf:500-507), and so does this: `resolved` already holds the
            // filtered radiance for a pixel this frame did not trace, and the guide is brought alongside
            // it so the two describe the same reconstruction. A pixel this frame DID trace keeps its
            // exact texel in both, which is strictly better than a filter and is what Unreal's
            // bUseNewSample branch does as well.
            if (!historyRead && !owned)
                traceGuide = texture(u_CloudTraceGuide, traceUv);

            // The guide always describes THIS frame, whichever branch the radiance came from: it is what
            // the composite measures edges with and what next frame's disocclusion test compares against,
            // and both questions are about the geometry in front of the camera now. It is never blended
            // with the history's own distances — mixing a distance from three frames ago with this one
            // produces a number describing nothing, which the composite would then read as an edge.
            imageStore(u_ReconstructedGuide, coord, traceGuide);

            // Transmittance is clamped both ways because it multiplies the scene behind the cloud: a
            // value above 1 brightens what is BEHIND it, which reads as a glowing rectangle and is very
            // hard to trace back to a filter. Radiance is clamped only against negatives, which no blend
            // of two non-negative values can produce but a poisoned history texel can.
            imageStore(u_ReconstructedScatter, coord,
                       vec4(max(resolved.rgb, vec3(0.0f)), clamp(resolved.a, 0.0f, 1.0f)));
        }
    }
}
