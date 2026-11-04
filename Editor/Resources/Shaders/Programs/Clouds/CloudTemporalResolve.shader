// DesertAsset {"Kind":"Shader","Guid":"f8d0506cdd2a8abd51f33eed80de242f","Versions":{"SHDR":1},"Dependencies":[]}
Shader "CloudTemporalResolve"
{
    Compute
    {
        // THE TEMPORAL RECONSTRUCTION — Unreal's VolumetricRenderTarget mode 0, reconstruct stage.
        //
        // CloudRaymarch.shader traces at a QUARTER of the framebuffer's size and jitters its rays so that
        // each quarter-res texel holds one of the four HALF-resolution pixels of its 2x2 block, walking
        // {0, 2, 3, 1}. It traces ONE CHECKER PHASE of the quarter grid per frame (CLOUD-MARCH-COST-3,
        // Graphic::CloudTraceCheckerPhase): the texels whose (x + y) & 1 is TraceCheckerPhase hold a sample,
        // the other half is unwritten transient memory and is never read. The phase alternates every frame and
        // the sub-pixel every second one, so each half-res pixel is traced once every eight frames
        // (Graphic::kCloudTraceWalkFrames). This pass turns that stream into a HALF-resolution image, and
        // CloudUpsample.shader upsamples the result exactly as it upsampled the half-res trace before this
        // stage existed.
        //
        // For each half-res pixel there are three cases:
        //
        //   1. THIS FRAME OWNS IT. The pixel's sub-pixel index equals the frame's and its block's texel is on
        //      this frame's checker phase, so the quarter-res texel above it holds a ray cast through this
        //      very pixel. Its value is fetched exactly,
        //      with a texelFetch and no filtering — it is the only sample in the whole scheme that is not
        //      an estimate.
        //
        //   2. HISTORY IS USABLE. Walk the pixel's ray to the guide's CLOUD FRONT distance, project that
        //      world point with the previous frame's view-projection — Unreal's ClipToPrevClip, factored
        //      so the walk can happen in world space — and read the reconstruction of the frame before.
        //      This frame's sample is then blended into it, by kOwnedSampleWeight for case 1 and by the
        //      much smaller kNewSampleWeight otherwise, each lowered toward its still-history weight as the
        //      reprojection's displacement falls to zero; see those constants for why the four differ.
        //
        //   3. NO HISTORY TO READ — the first frame after the targets were allocated, or a reprojection
        //      that lands off screen or behind the previous eye. Take the bilinear reconstruction of the
        //      quarter-res trace, radiance and guide together, which is exactly what Unreal does in the
        //      same position (VolumetricRenderTarget.usf:500-507: "History is invalid so simply use this
        //      frame low resolution render with bilinear sampling"). A history that was READ and then
        //      REJECTED is not this case — see the validation list below.
        //
        // THE GUIDE IS LOADED, NEVER FILTERED, except in case 3. (A block of the other checker phase has no
        // texel this frame; it loads the nearest front of its four written edge neighbours, see main.)
        // Unreal point-loads its depth guide for
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
        layout(binding = 4, rgba16f) restrict writeonly uniform image2D ReconstructedScatterTexture;

        // The reconstructed guide, which serves two readers: CloudUpsample.shader upsamples through it,
        // and NEXT frame's run of this pass reads it back as the history guide to detect disocclusions.
        // It carries THIS frame's distances with no temporal blending at all — a distance is a property of
        // the geometry in front of the camera now, and mixing it with a distance from three frames ago
        // would produce a number describing nothing, which the composite would then treat as an edge.
        layout(binding = 5, rgba16f) restrict writeonly uniform image2D ReconstructedGuideTexture;

        // This frame's QUARTER-resolution trace and its guide.
        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(8) sampler LinearClampSampler;
        Uniform(9) sampler GlobalFilterSampler;
        Uniform(0) texture2D CloudTraceTexture;
        #define CloudTraceTexture sampler2D(CloudTraceTexture, LinearClampSampler)
        Uniform(1) texture2D CloudTraceGuideTexture;
        #define CloudTraceGuideTexture sampler2D(CloudTraceGuideTexture, LinearClampSampler)

        // The PREVIOUS frame's half-resolution reconstruction and its guide. On the first frame after the
        // targets are allocated these are bound to the engine's fallback texture rather than left unbound
        // — a declared sampler with no image is an INVALID descriptor set, not an unused one, and this
        // backend answers an invalid set by skipping the entire dispatch, which would lose the clouds with
        // nothing in the log. HistoryValid is what says the bytes mean anything.
        Uniform(2) texture2D CloudHistoryTexture;
        #define CloudHistoryTexture sampler2D(CloudHistoryTexture, GlobalFilterSampler)
        Uniform(3) texture2D CloudHistoryGuideTexture;
        #define CloudHistoryGuideTexture sampler2D(CloudHistoryGuideTexture, GlobalFilterSampler)

        // THIS PIXEL'S OWN SCENE DEPTH — the same single-sample image the march cuts its rays with, read
        // at the same texel the march would read for this half-res pixel. It is what makes the guide's .y
        // a property of THIS pixel. Before it, .y was copied from whichever sub-pixel of the 2x2 block the
        // frame traced, so on a block straddling a silhouette (a cube against the sky, the ground against
        // the horizon) it flipped between the two surfaces on the {0, 2, 3, 1} cycle: the disocclusion
        // test then rejected a good history every frame the traced sub-pixel sat on the other surface,
        // and the pixel took that surface's radiance instead — the four-frame flicker along every
        // silhouette (CLOUD-HORIZON: 5 % of the pixels around a cube changed by more than 4/255 per
        // frame under a STATIC camera). Unreal's reconstruction compares against the pixel's own opaque
        // depth for the same reason (VolumetricRenderTarget.usf, SceneDepthMinAndMax / the mode-4
        // compose reading SceneDepth).
        Uniform(7) texture2D SceneDepthTexture;
        #define SceneDepthTexture sampler2D(SceneDepthTexture, GlobalFilterSampler)
        #include <Common/CloudSceneDepth.glslh>
        // FOLIAGE-CLOUD-FLICKER: "this pixel's own scene depth" is now the FARTHEST surface of the full-res
        // block under it, the depth the march cuts at (Common/CloudSceneDepth.glslh). A single texel of the block
        // reproduced the flip described above one level down: under the TAA jitter a grass blade covers that
        // texel on some frames and not on others, so .y swung between the blade and the sky and the
        // disocclusion test below rejected a good history at every blade edge, every frame.

        // The block layout this pass shares with CloudUpsample.shader (Common/CloudResolveParams.glslh).
        #include <Common/CloudResolveParams.glslh>

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
        //
        // THESE TWO ARE THE WEIGHTS OF A MOVING HISTORY — one reprojected by kMovingHistoryPixels or more.
        // Both costs they trade against are costs of RESAMPLING: a history read between texel centres is a
        // bilinear blur of itself, compounded every frame it is kept, and a not-owned pixel drifts from the
        // truth between the frames that own it. A history that did not move has neither — it is read at its
        // own texel centre and is exactly the pixel it was — so it takes the still weights below, and the
        // reprojection's own displacement blends between the two (UE's TAA does the same with its current
        // frame weight: TemporalAA.shader, kCurrentWeight raised toward kMovingCurrentWeight by the motion).
        //
        // LOWERED FROM 0.75 ONCE THE MOVING HISTORY WAS READ THROUGH A CATMULL-ROM (CloudHistoryCatmullRom).
        // The sweep above was taken on a still camera; 0.75 on a moving one is the bilinear blur's price, and
        // it hands the trace's start-offset dither to the screen every fourth frame — the sky, and everything
        // drawn against it, flickered while the camera turned (owner, 11-03). The cubic read keeps a
        // resampled history as sharp as a still one, so the moving owned weight only has to stop drift, and
        // twice the still weight does that.
        const float kOwnedSampleWeight = 0.3f;
        const float kNewSampleWeight   = 0.06f;

        // THE WEIGHT OF AN OWNED SAMPLE ON A STILL HISTORY: the same time constant UE's TAA averages a still
        // pixel over, 1 - (1 - 0.04)^4 — r.TemporalAA.CurrentFrameWeight (TemporalAA.shader kCurrentWeight)
        // compounded over the four frames between two traces of one pixel (Graphic::kCloudTraceSubPixelCount).
        //
        // WHY THIS PASS AND NOT THE TAA. The TAA cannot average this layer's noise: its history is clipped to
        // the 3x3 full-resolution neighbourhood's variance, and that neighbourhood is the bilinear upsample of
        // ONE or two half-res pixels — spatially smooth, so its box is tight, while the pixel it upsamples
        // jumps every fourth frame by the trace's dither. The clip then throws the TAA history away and the
        // jump reaches the screen, in the sky and in every edge in front of it ("и не только листва, все что
        // на фоне облаков", SKY-FLICKER-2). Unreal hands the new sample over whole (weight 1,
        // VolumetricRenderTarget.usf:454-458) because TSR, not its legacy TAA, integrates behind it.
        //
        // The march's start offset is low-discrepancy per pixel over its traces (CloudRaymarch.shader,
        // CloudMarchStartOffset), so this average converges on the step's mean — the shape and brightness of
        // a single trace's expectation, with the dither's swing gone — rather than on one realization.
        const float kStillOwnedSampleWeight = 0.15f;

        // A not-owned pixel on a still history keeps the history whole — Unreal's ReprojRGBA
        // (VolumetricRenderTarget.usf:461-487). The bilinear estimate it would otherwise take in is a blur of
        // the quarter-res trace whose footprint walks with the sub-pixel cycle: at a still camera it adds that
        // walk and that blur to every pixel and nothing a still history needs.
        const float kStillNewSampleWeight = 0.0f;

        // The reprojection displacement, in half-res pixels, at and above which a history takes the moving
        // weights in full. One pixel: below it the history is read inside its own texel's footprint.
        const float kMovingHistoryPixels = 1.0f;

        // WHERE THE WIND CARRIED A CLOUD POINT FROM since the history's frame, as a displacement in planet-centred
        // kilometres from @p planetKm (the point now). The field is sampled at (x, altitude, z) minus the wind
        // offset (Common/CloudField.glslh, CloudSampleProceduralField), so a feature at field coordinate f now
        // was at f - WindStepKm then: x and z shift by the step, the altitude |P| by its y, and the planet's
        // y follows from the new radius. Written as the DIFFERENCE of y^2 rather than as the new point, because
        // planet-centred y is ~6400 km and float32 holds it to half a metre — rebuilding the point would move
        // a still history by that much with no wind at all, while this is exactly zero at a zero step.
        vec3 CloudWindStepBackKm(vec3 planetKm, vec3 windStepKm)
        {
            float radiusKm = length(planetKm);
            float dySq     = windStepKm.x * (2.0f * planetKm.x - windStepKm.x) +
                         windStepKm.z * (2.0f * planetKm.z - windStepKm.z) -
                         windStepKm.y * (2.0f * radiusKm - windStepKm.y);
            float dy       = dySq / (planetKm.y + sqrt(max(planetKm.y * planetKm.y + dySq, 0.0f)));
            return vec3(-windStepKm.x, dy, -windStepKm.z);
        }

        // Half a texel in, on both axes. This engine's samplers are REPEAT, so a uv inside [0,1] but
        // within half a texel of the border still gathers from the OPPOSITE edge of the image — a bright
        // fringe along one side that no amount of staring at the march explains.
        vec2 ClampToTexelCentres(vec2 uv, ivec2 size)
        {
            vec2 half_texel = 0.5f / vec2(size);
            return clamp(uv, half_texel, vec2(1.0f, 1.0f) - half_texel);
        }

        // A MOVING HISTORY IS READ THROUGH A CATMULL-ROM, NOT A BILINEAR (UE's TemporalAA.usf history fetch,
        // AA_BICUBIC; the five-tap form of Jimenez's Filmic SMAA). A bilinear read between texel centres is a
        // blur of the history, and kept every frame it compounds: that blur was the whole reason a moving
        // history took the owned sample at 0.75, and 0.75 hands the trace's start-offset dither to the screen
        // every fourth frame - the flicker of the sky and of everything in front of it while the camera
        // turns (owner, 11-03: "когда крутишь там мерцают облака"). The cubic keeps a resampled history sharp,
        // so the moving weights below can stay near the still ones. Its overshoot is clamped to the 2x2
        // texels the bilinear would have blended, so it never invents a value the history did not hold.
        vec4 CloudHistoryCatmullRom(vec2 uv, ivec2 size)
        {
            vec2 texSize  = vec2(size);
            vec2 position = uv * texSize;
            vec2 centre1  = floor(position - 0.5f) + 0.5f;
            vec2 f        = position - centre1;
            vec2 w0       = f * (-0.5f + f * (1.0f - 0.5f * f));
            vec2 w1       = 1.0f + f * f * (-2.5f + 1.5f * f);
            vec2 w2       = f * (0.5f + f * (2.0f - 1.5f * f));
            vec2 w3       = f * f * (-0.5f + 0.5f * f);
            vec2 w12      = w1 + w2;
            vec2 uv0      = (centre1 - 1.0f) / texSize;
            vec2 uv12     = (centre1 + w2 / w12) / texSize;
            vec2 uv3      = (centre1 + 2.0f) / texSize;

            float wTop    = w12.x * w0.y;
            float wLeft   = w0.x * w12.y;
            float wMid    = w12.x * w12.y;
            float wRight  = w3.x * w12.y;
            float wBottom = w12.x * w3.y;

            vec4 sum = texture(CloudHistoryTexture, ClampToTexelCentres(vec2(uv12.x, uv0.y), size)) * wTop;
            sum += texture(CloudHistoryTexture, ClampToTexelCentres(vec2(uv0.x, uv12.y), size)) * wLeft;
            sum += texture(CloudHistoryTexture, ClampToTexelCentres(uv12, size)) * wMid;
            sum += texture(CloudHistoryTexture, ClampToTexelCentres(vec2(uv3.x, uv12.y), size)) * wRight;
            sum += texture(CloudHistoryTexture, ClampToTexelCentres(vec2(uv12.x, uv3.y), size)) * wBottom;
            vec4 cubic = sum / (wTop + wLeft + wMid + wRight + wBottom);

            ivec2 base  = clamp(ivec2(centre1 - 0.5f), ivec2(0), size - ivec2(1));
            ivec2 other = min(base + ivec2(1), size - ivec2(1));
            vec4  t00   = texelFetch(CloudHistoryTexture, base, 0);
            vec4  t10   = texelFetch(CloudHistoryTexture, ivec2(other.x, base.y), 0);
            vec4  t01   = texelFetch(CloudHistoryTexture, ivec2(base.x, other.y), 0);
            vec4  t11   = texelFetch(CloudHistoryTexture, other, 0);
            vec4  lo    = min(min(t00, t10), min(t01, t11));
            vec4  hi    = max(max(t00, t10), max(t01, t11));
            return clamp(cubic, lo, hi);
        }

        bool IsFinite(vec4 v)
        {
            return !any(isnan(v)) && !any(isinf(v));
        }

        // A NOT-OWNED PIXEL'S ESTIMATE: the bilinear reconstruction of the quarter-res trace at @p uv, radiance
        // and guide together, over the texels that hold a SAMPLE. A texel whose footprint was occluded in front
        // of the layer (Common/CloudSceneDepth.glslh: guide .z = 1) holds "clear" only because nothing was
        // marched for it; a plain bilinear would blend that clear into every sky pixel beside a crown, a roof or
        // a fence, by a weight that walks with the trace's sub-pixel cycle. Its weight goes to the other texels
        // instead. With no such texel in the footprint this IS the plain lattice blend below; with nothing but
        // such texels it is that plain blend too, and @p sampled is false.
        //
        // OVER THIS FRAME'S CHECKER PHASE ONLY (CLOUD-MARCH-COST-3, Graphic::CloudTraceCheckerPhase): the march wrote
        // the texels whose (x + y) & 1 is TraceCheckerPhase and left the others as undefined transient memory. The
        // written texels form a quincunx lattice, which is a regular unit grid in the rotated coordinates
        // u = (x + y) / 2, v = (x - y) / 2; the estimate is the BILINEAR interpolation on that grid (tent weights in
        // u and v, which sum to one over the lattice), so it is exact on a written texel and the plain linear blend
        // of its four edge neighbours on an unwritten one. Its support is |dx| + |dy| < 2, which the 4x4 texels
        // around the footprint cover; the other phase's eight are skipped without a fetch. A texel past the grid's
        // edge is skipped and the weights renormalised, where the plain bilinear clamped to the edge texel.
        bool CloudTraceTexelHoldsSample(ivec2 p)
        {
            return ((p.x + p.y) & 1) == (TraceCheckerPhase & 1);
        }

        void TraceFiltered(vec2 uv, ivec2 traceSize, out vec4 scatter, out vec2 guide, out bool sampled)
        {
            vec2  texel = uv * vec2(traceSize) - vec2(0.5f, 0.5f);
            ivec2 base  = ivec2(floor(texel));
            ivec2 last  = traceSize - ivec2(1, 1);

            vec4  sampledScatter = vec4(0.0f);
            vec2  sampledGuide   = vec2(0.0f);
            float sampledWeight  = 0.0f;
            // The same lattice blend over every written texel, occluded ones included: the answer when the
            // footprint holds nothing but occluded texels (it is then the "clear" they hold, as the plain bilinear
            // of them was).
            vec4  plainScatter = vec4(0.0f);
            vec2  plainGuide   = vec2(0.0f);
            float plainWeight  = 0.0f;
            for (int i = 0; i < 16; ++i)
            {
                ivec2 p = base + ivec2((i & 3) - 1, (i >> 2) - 1);
                if (any(lessThan(p, ivec2(0, 0))) || any(greaterThan(p, last)) || !CloudTraceTexelHoldsSample(p))
                    continue;

                vec2  d = vec2(p) - texel;
                float w = max(1.0f - abs(d.x + d.y) * 0.5f, 0.0f) * max(1.0f - abs(d.x - d.y) * 0.5f, 0.0f);
                if (w <= 0.0f)
                    continue;

                vec4 g = texelFetch(CloudTraceGuideTexture, p, 0);
                vec4 s = texelFetch(CloudTraceTexture, p, 0);
                plainScatter += w * s;
                plainGuide += w * g.xy;
                plainWeight += w;
                if (g.z >= 0.5f)
                    continue;
                sampledScatter += w * s;
                sampledGuide += w * g.xy;
                sampledWeight += w;
            }

            sampled = sampledWeight > 1e-4f;
            if (sampled)
            {
                scatter = sampledScatter / sampledWeight;
                guide   = sampledGuide / sampledWeight;
            }
            else if (plainWeight > 1e-4f)
            {
                scatter = plainScatter / plainWeight;
                guide   = plainGuide / plainWeight;
            }
            else
            {
                // Unreachable inside the grid (a written texel is always within |dx| + |dy| <= 1 of a texel
                // centre), kept so a degenerate one-texel grid reads "clear" rather than a division by zero.
                scatter = vec4(0.0f, 0.0f, 0.0f, 1.0f);
                guide   = vec2(0.0f, 0.0f);
            }
        }

        void main()
        {
            ivec2 size  = imageSize(ReconstructedScatterTexture);
            ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
            if (coord.x >= size.x || coord.y >= size.y)
                return;

            ivec2 traceSize = textureSize(CloudTraceTexture, 0);

            // Which sub-pixel of its 2x2 block this half-res pixel is, and the quarter-res texel that
            // covers the block. `& 1` and `>> 1` rather than `% 2` and `/ 2`: the coordinates are
            // non-negative by construction and the bit forms have no sign-handling to get wrong.
            ivec2 subPixel   = coord & ivec2(1, 1);
            ivec2 traceCoord = coord >> ivec2(1, 1);

            // THE BLOCK HOLDS A SAMPLE ONLY ON THIS FRAME'S CHECKER PHASE (CloudTraceTexelHoldsSample): on the other
            // phase all four of its pixels are not-owned this frame and take the lattice estimate below.
            bool blockTraced = CloudTraceTexelHoldsSample(traceCoord);
            bool owned       = blockTraced && subPixel == SubPixelOffset;

            // THIS FRAME'S LOW-RESOLUTION ESTIMATE, with the jitter undone. A quarter-res texel does not
            // hold the value at its own centre — it holds the value at the half-res pixel
            // (2 * texel + offset + 0.5), which is where its ray was cast. Sampling the texture at this
            // pixel's position directly would therefore be wrong by half a half-res pixel in a direction
            // that CHANGES EVERY FRAME, and a reconstruction that wobbles with the jitter is precisely
            // the artefact this pass exists to remove. Inverting the placement instead costs one
            // subtraction: the continuous texel index whose sample lands on this pixel is
            // (halfPos - offset - 0.5) / 2.
            vec2 halfPos    = vec2(coord) + vec2(0.5f, 0.5f);
            vec2 traceTexel = (halfPos - vec2(SubPixelOffset) - vec2(0.5f, 0.5f)) * 0.5f;
            vec2 traceUv    = ClampToTexelCentres((traceTexel + vec2(0.5f, 0.5f)) / vec2(traceSize), traceSize);

            // Case 1 for the RADIANCE: the pixel this frame traced gets the exact texel, fetched rather
            // than filtered. The other three of the block, and all four of a block of the other checker phase,
            // have no sample of their own this frame, so the lattice reconstruction of this frame's written
            // texels (TraceFiltered) is all that exists for them.
            //
            // THE GUIDE IS LOADED FOR EVERY PIXEL, owned or not — Unreal's :286 and :391, and see the
            // header for why filtering it is harmful rather than merely approximate (and blockFrontKm below
            // for a block with no texel this frame). The block's four pixels therefore share one distance,
            // which is exactly what its readers want: a distance that agrees with its neighbours costs the
            // composite nothing, while a bilinear blend of the two
            // sides of a silhouette agrees with neither and fires the edge path along the whole silhouette.
            // Its .z says whether the texel holds a sample at all (Common/CloudSceneDepth.glslh).
            vec4 traceScatter;
            vec2 traceGuideHere; // the guide over the same footprint as traceScatter (.x front, .y cut)
            bool traceSampled;   // whether traceScatter comes from a texel that holds a sample
            if (owned)
            {
                vec4 ownGuide  = texelFetch(CloudTraceGuideTexture, traceCoord, 0);
                traceScatter   = texelFetch(CloudTraceTexture, traceCoord, 0);
                traceGuideHere = ownGuide.xy;
                traceSampled   = ownGuide.z < 0.5f;
            }
            else
            {
                TraceFiltered(traceUv, traceSize, traceScatter, traceGuideHere, traceSampled);
            }

            // The block's front (.x of its texel's guide), shared by its four pixels as above. A block of the
            // other checker phase has no texel of its own this frame. Its four EDGE neighbours are the nearest
            // written texels, all at the same distance, and the block takes the NEAREST FRONT of those that hold
            // a sample (of all four when none does): a point-loaded distance a ray actually traced, as the header
            // requires outside case 3, and not their blend, which at a silhouette lies on neither surface and is
            // what the reprojection below would then walk to. Nearest because the front is what the reprojection
            // tracks, and at a silhouette the nearer surface is the one whose edge must not trail (the closest-
            // depth rule of temporal reprojection). The four pixels still share one distance.
            float blockFrontKm = 0.0f;
            if (blockTraced)
            {
                blockFrontKm = texelFetch(CloudTraceGuideTexture, traceCoord, 0).x;
            }
            else
            {
                bool  anySampled   = false;
                bool  anyWritten   = false;
                float sampledMinKm = 0.0f;
                float writtenMinKm = 0.0f;
                for (int i = 0; i < 4; ++i)
                {
                    int   side = (i & 1) * 2 - 1;
                    ivec2 p    = traceCoord + ((i & 2) == 0 ? ivec2(side, 0) : ivec2(0, side));
                    if (any(lessThan(p, ivec2(0, 0))) || any(greaterThan(p, traceSize - ivec2(1, 1))))
                        continue;

                    vec4 g       = texelFetch(CloudTraceGuideTexture, p, 0);
                    writtenMinKm = anyWritten ? min(writtenMinKm, g.x) : g.x;
                    anyWritten   = true;
                    if (g.z < 0.5f)
                    {
                        sampledMinKm = anySampled ? min(sampledMinKm, g.x) : g.x;
                        anySampled   = true;
                    }
                }
                // No written neighbour only on a degenerate one-texel grid, where TraceFiltered reads "clear".
                blockFrontKm = anySampled ? sampledMinKm : (anyWritten ? writtenMinKm : 0.0f);
            }

            // The pixel's ray, from this frame's inverse view-projection. REVERSED-Z, like everything
            // else in this engine (Core/Projection.hpp): 1 is the near plane and 0 the far one.
            vec2 uv  = halfPos / vec2(size);
            vec2 ndc = vec2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

            vec4 nearH = InverseViewProjection * vec4(ndc.x, ndc.y, 1.0f, 1.0f);
            vec4 farH  = InverseViewProjection * vec4(ndc.x, ndc.y, 0.0f, 1.0f);
            vec3 nearP = nearH.xyz / max(nearH.w, 1e-9f);
            vec3 farP  = farH.xyz / max(farH.w, 1e-9f);
            vec3 rayDir = normalize(farP - nearP);

            // THIS PIXEL'S SCENE DISTANCE, by the march's own expression (CloudRaymarch.shader, the
            // `sceneKm` it writes into its guide's .y): the far plane where nothing was drawn, the opaque
            // surface where something was. Same depth rule too — the farthest surface of the full-res block,
            // Common/CloudSceneDepth.glslh — so on the pixel this frame owns the two agree exactly.
            float deviceDepth = CloudFootprintFarthestDepth(coord, size);
            float farKm       = length(farP - CameraPosition) * (1.0f / CLOUD_WORLD_UNITS_PER_KM);
            float geomKm      = farKm;
            if (deviceDepth > 0.0f)
            {
                vec4 geomH = InverseViewProjection * vec4(ndc.x, ndc.y, deviceDepth, 1.0f);
                vec3 geomP = geomH.xyz / max(geomH.w, 1e-9f);
                geomKm     = length(geomP - CameraPosition) * (1.0f / CLOUD_WORLD_UNITS_PER_KM);
            }

            // AND THE MARCH'S CLASSIFICATION OF THE RAY, by its own expression and the payload it was given
            // (CloudShell, CloudReach): a footprint wholly in front of the layer is OCCLUDED, its scene distance
            // is the far plane, and this frame has no sample of it (Common/CloudSceneDepth.glslh). On the pixel
            // the frame traced the two decisions are the same computation on the same bits.
            CloudLayer layer    = CloudMakeLayer(CloudShell.x, CloudShell.y, CloudShell.z);
            vec2       segment  = CloudMarchSegment(layer, CloudPlanetOriginKm(CameraPosition, layer.PlanetRadiusKm),
                                                    rayDir, CloudReach.x, CloudShell.w);
            bool       occluded = CloudFootprintOccluded(deviceDepth, geomKm, segment,
                                                         CloudMarchHasWork(segment, CloudReach.y));
            float ownSceneKm = occluded ? farKm : geomKm;

            // WHETHER THIS FRAME'S SAMPLE DESCRIBES THIS PIXEL'S SURFACE. The owned pixel's does by
            // construction. The other three take the BILINEAR reconstruction of the trace, so they are
            // judged by the guide filtered over the same footprint: a footprint that reaches across a
            // silhouette averages the near and far distances into one that matches neither, and its
            // radiance is that same mixture — blending it in is the leak that crawled along every edge.
            // A texel that holds no sample (occluded in front of the layer) describes nothing.
            bool newSampleHere = traceSampled &&
                                 (owned || abs(traceGuideHere.y - ownSceneKm) <= kDisocclusionKm);

            // Every rejection below leaves this value in place: a history that was read and found to
            // describe a different surface is answered with THIS frame's sample. That is Unreal's rule
            // too — a failed disocclusion test there sets bUseNewSample — and it is the reason there is no
            // search here: a neighbour that passes validation belongs to a different surface, and the
            // smear it produces follows the camera.
            //
            // EXCEPT ON AN OCCLUDED PIXEL WITH NO SAMPLE OF ITS OWN: there this frame has nothing to say, and the
            // value held is "no cloud", which its guide below marks as the occluder's (SKY-FLICKER).
            bool frameEstimate = newSampleHere || !occluded;
            vec4 resolved      = frameEstimate ? traceScatter : vec4(0.0f, 0.0f, 0.0f, 1.0f);

            // Whether `resolved` estimates the cloud at all: this frame's sample or an accepted history.
            bool estimate = frameEstimate;

            // Whether there was a history to read AT ALL, which is a different question from whether it
            // was accepted. Only the first sends the pixel down case 3.
            bool historyRead = false;

            // The front distance the output guide carries. It follows the RADIANCE: a pixel that keeps its
            // history because this frame's sample belongs to another surface keeps that history's front, and
            // so does a not-owned pixel whose radiance is mostly that history (below).
            float frontKm = blockFrontKm;

            if (HistoryValid > 0.5f)
            {
                // THE SINGLE FRONT SURFACE. The guide's .x is the ray's transmittance-weighted mean distance
                // (Unreal's tAP, CloudRaymarch.shader), or the end of its search when it met no cloud; either
                // way it is a real distance, so the point below is always on the ray and never a sentinel
                // projected into the previous frame.
                //
                // AND WHERE THAT POINT WAS WHEN THE HISTORY WAS TRACED. Unreal reprojects by the camera alone,
                // so a still camera reads a still history while the wind carries the cloud past it: at the
                // still weight the edge lags ~0.4 s and trails. The point is moved back by the wind's step
                // since the history's frame (CloudResolveParams.WindStepKm, zero without wind), so the history
                // is read where this piece of cloud was rather than where the air now is.
                vec3 frontPlanetKm = CloudPlanetOriginKm(CameraPosition, layer.PlanetRadiusKm) + rayDir * blockFrontKm;
                vec3 frontWorld    = CameraPosition + rayDir * (blockFrontKm * CLOUD_WORLD_UNITS_PER_KM) +
                                  CloudWindStepBackKm(frontPlanetKm, WindStepKm) * CLOUD_WORLD_UNITS_PER_KM;

                vec4 prevClip = PreviousViewProjection * vec4(frontWorld, 1.0f);

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

                        float moving       = clamp(length((prevUv - uv) * vec2(size)) / kMovingHistoryPixels,
                                                   0.0f, 1.0f);
                        vec4  history      = moving > 0.0f ? CloudHistoryCatmullRom(fetchUv, size)
                                                           : texture(CloudHistoryTexture, fetchUv);
                        vec4 historyGuide = texture(CloudHistoryGuideTexture, fetchUv);

                        bool finite      = IsFinite(history) && IsFinite(historyGuide);
                        // Against THIS pixel's distance, not the traced sub-pixel's — see SceneDepthTexture.
                        bool sameSurface = abs(historyGuide.y - ownSceneKm) <= kDisocclusionKm;

                        if (finite && sameSurface)
                        {
                            estimate = true;
                            if (newSampleHere)
                            {
                                // How far the history moved to get here, in half-res pixels: 0 at a still
                                // camera, where the history is read at its own texel centre and is exact.
                                float newWeight = owned ? mix(kStillOwnedSampleWeight, kOwnedSampleWeight, moving)
                                                        : mix(kStillNewSampleWeight, kNewSampleWeight, moving);
                                resolved        = mix(history, traceScatter, newWeight);

                                // The front follows the radiance: a not-owned pixel's is the history's at any
                                // motion (Unreal's ReprojDepthData, VolumetricRenderTarget.usf:465), because at
                                // most kNewSampleWeight of it is this frame's. The traced texel's front belongs
                                // to whichever sub-pixel the walk is on and to this trace's dither, so carrying
                                // it put a new distance under every pixel of the block every frame, and the
                                // composite's edge filter (CloudUpsample.shader) re-weighed them every frame.
                                if (!owned)
                                    frontKm = historyGuide.x;
                            }
                            else
                            {
                                resolved = history;
                                frontKm  = historyGuide.x;
                            }
                        }
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
                frontKm = traceGuideHere.x;

            // The guide always describes THIS frame, whichever branch the radiance came from: it is what
            // the composite measures edges with and what next frame's disocclusion test compares against,
            // and both questions are about the geometry in front of the camera now. It is never blended
            // with the history's own distances — mixing a distance from three frames ago with this one
            // produces a number describing nothing, which the composite would then read as an edge.
            // .y is this pixel's own scene distance on every branch, which is what keeps it from flipping
            // with the jitter cycle; .x is the front of whichever sample the radiance came from.
            //
            // A pixel that holds NO ESTIMATE (occluded, no sample, no history) holds "no cloud" and says so the
            // way an occluded texel always did: both channels at its occluder. Its own pixels then take no cloud
            // from it (front at the surface) and a sky pixel beside it weighs it out (cut far from the sky);
            // next frame's resolve rejects it as history, since its .y is not the far plane this pixel reports.
            vec2 heldGuide = estimate ? vec2(frontKm, ownSceneKm) : vec2(geomKm, geomKm);
            imageStore(ReconstructedGuideTexture, coord, vec4(heldGuide, 0.0f, 0.0f));

            // Transmittance is clamped both ways because it multiplies the scene behind the cloud: a
            // value above 1 brightens what is BEHIND it, which reads as a glowing rectangle and is very
            // hard to trace back to a filter. Radiance is clamped only against negatives, which no blend
            // of two non-negative values can produce but a poisoned history texel can.
            imageStore(ReconstructedScatterTexture, coord,
                       vec4(max(resolved.rgb, vec3(0.0f)), clamp(resolved.a, 0.0f, 1.0f)));
        }
    }
}
