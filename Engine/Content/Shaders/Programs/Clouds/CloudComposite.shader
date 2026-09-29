// DesertAsset {"Kind":"Shader","Guid":"98a7911d387d07d3b6bf5a431308dac0","Versions":{"SHDR":1},"Dependencies":[]}
Shader "CloudComposite"
{
    Fragment
    {
        // The cloud APPLY: a fullscreen quad in RenderPhase::Transparency at RenderPassOrder::FarField —
        // ABOVE the height fog (AtmosphericFog, -100 below this rung) and BELOW everything the phase
        // composites by default, so particles and translucency land over the clouds rather than under
        // them. FarField exists in RenderGraphSort.hpp for exactly this: "content at sky distance".
        //
        // The pipeline supplies the blend: source factor One, destination factor SrcAlpha, i.e.
        //
        //     scene = cloud.rgb + scene * cloud.a
        //
        // the premultiplied over-operator with .a carrying TRANSMITTANCE, not opacity. The march
        // (CloudRaymarch.shader) produced exactly that pair, so nothing is reconstructed here.
        //
        // OCCLUSION WAS ALREADY RESOLVED. The march cut every ray at the scene depth, so a cloud behind a
        // mountain contributed nothing to begin with. This pass therefore runs with the depth test off and
        // could not read the depth attachment anyway — it is bound by the render pass, and sampling a
        // bound attachment is a feedback loop.
        //
        // WHAT THIS PASS READS. Not the march's own output: the march traces at a QUARTER of the
        // framebuffer's size with a jittered ray, and CloudTemporalResolve.shader reconstructs a HALF-
        // resolution image from four such frames. This pass upsamples that half-resolution pair — scatter
        // and guide — from half to full, which is exactly what it did before the resolve stage existed.
        // Nothing here changed with mode 0; the images arrive at the same size, in the same formats, with
        // the same meaning in every channel.
        //
        // BILATERAL UPSAMPLE — Unreal's upsampling mode 4, adapted to a pass that cannot read the scene
        // depth. The source runs at half the framebuffer's size, so this pass reconstructs rather than
        // fetches, and a plain bilinear reconstruction has one known failure: across a silhouette it
        // blends a cloud texel with an empty-sky texel and spreads the edge over a full low-res texel,
        // which reads as a halo and as mushy shapes.
        //
        // The fix is a DEPTH GUIDE carried alongside the scatter (written by CloudRaymarch.shader and
        // reconstructed to half resolution by CloudTemporalResolve.shader): per low-res texel, the
        // distance to the front of the cloud and the distance the ray was cut at. Neighbours that
        // disagree with this pixel about either are down-weighted, so the reconstruction stops averaging
        // across the edge and starts choosing a side.
        //
        // WHERE THE REFERENCE COMES FROM. Unreal compares each neighbour against the pixel's OWN scene
        // depth. This pass has no such thing to compare with — the depth attachment is bound by the
        // render pass it draws into, and sampling a bound attachment is a feedback loop; that is the
        // whole reason the guide exists. The reference is therefore the BILINEAR BLEND of the four
        // guide values, which is the best estimate of the pixel's own distance that the guide supports.
        // It is not a self-cancelling choice: because the blend is dominated by whichever side of the
        // edge the pixel sits nearer to, the depth weights then pull the result the rest of the way, and
        // a transition that was linear across a texel becomes a near-step at the crossing. That steepening
        // is the entire effect, and it stays SUB-TEXEL — the crossing sits where the bilinear weights put
        // it, so the edge does not snap onto the low-res grid.
        //
        // BILINEAR IS STILL THE COMMON PATH, deliberately. A bilateral filter applied everywhere is worse
        // than bilinear in smooth regions, because rejecting neighbours quantizes the result onto the
        // low-res grid; the sky is mostly smooth regions. The four guide values are therefore tested for
        // coherence first, against Unreal's threshold of a tenth of the reference distance, and only a
        // genuine discontinuity pays for the weighted path.
        //
        // FOUR texelFetches PER IMAGE RATHER THAN textureGather. Gather is two fetches' worth of traffic
        // less, but it picks its own 2x2 footprint from the sampler's coordinate rounding, and this pass
        // needs the footprint AND the bilinear fractions AND the guide values for the same four texels to
        // come from one expression. Deriving matching fractions beside a gather is exactly the pair of
        // places that must agree and is never checked. The cost is eight fetches on a fullscreen pass,
        // against a march that took hundreds of samples per texel.
        //
        // NO TONEMAP, NO GAMMA, NO EXPOSURE: linear HDR onto linear HDR.

        In(0) vec2 v_TexCoord;

        Out(0) vec4 o_Color;

        Uniform(0) sampler2D u_CloudScatter;

        // The guide, same size as the scatter: .x = cloud front distance (km), .y = scene
        // distance (km). Point-fetched, never filtered — a filtered depth across a silhouette averages
        // near and far into a distance where nothing is, which is the artefact this pass exists to undo.
        Uniform(1) sampler2D u_CloudGuide;

        // Unreal's coherence threshold: a tenth of the reference distance. Relative rather than absolute
        // because a kilometre of disagreement is a silhouette at three kilometres and nothing at fifty.
        const float kCoherentFraction = 0.1;

        // Turns a distance disagreement in kilometres into a weight. The 1000 is what makes it bite: at
        // this scale a neighbour ten metres away still counts, and one a kilometre away does not.
        float GuideWeight(float deltaKm)
        {
            return 1.0 / (deltaKm * 1000.0 + 1.0);
        }

        void main()
        {
            ivec2 size = textureSize(u_CloudScatter, 0);

            // The half-texel shift is the bilinear rule itself: texel centres sit at (i + 0.5) / size, so
            // the four texels straddling this pixel start at floor(uv * size - 0.5). Without the shift the
            // whole reconstruction is offset by half a low-res texel and every edge lands in the wrong
            // place — a shift that looks like blur rather than like an offset, which is why it is written
            // out here instead of trusted to a filtering mode.
            vec2  texel = v_TexCoord * vec2(size) - vec2(0.5);
            vec2  frac  = texel - floor(texel);
            ivec2 base  = ivec2(floor(texel));

            // Clamped to the edge, which is what the previous bilinear fetch did through the sampler. The
            // scatter target is half the frame rounded UP, so the last row and column exist; the clamp is
            // for the outer half-texel, where the 2x2 footprint reaches past the image.
            ivec2 last = size - ivec2(1);

            ivec2 coords[4];
            coords[0] = clamp(base + ivec2(0, 0), ivec2(0), last);
            coords[1] = clamp(base + ivec2(1, 0), ivec2(0), last);
            coords[2] = clamp(base + ivec2(0, 1), ivec2(0), last);
            coords[3] = clamp(base + ivec2(1, 1), ivec2(0), last);

            float bilinear[4];
            bilinear[0] = (1.0 - frac.x) * (1.0 - frac.y);
            bilinear[1] = frac.x * (1.0 - frac.y);
            bilinear[2] = (1.0 - frac.x) * frac.y;
            bilinear[3] = frac.x * frac.y;

            vec4  scatter[4];
            float frontKm[4];
            float sceneKm[4];

            for (int i = 0; i < 4; ++i)
            {
                scatter[i]  = texelFetch(u_CloudScatter, coords[i], 0);
                vec2 guide  = texelFetch(u_CloudGuide, coords[i], 0).xy;
                frontKm[i]  = guide.x;
                sceneKm[i]  = guide.y;
            }

            float referenceFrontKm = 0.0;
            float referenceSceneKm = 0.0;
            for (int i = 0; i < 4; ++i)
            {
                referenceFrontKm += bilinear[i] * frontKm[i];
                referenceSceneKm += bilinear[i] * sceneKm[i];
            }

            float frontSpreadKm = max(max(frontKm[0], frontKm[1]), max(frontKm[2], frontKm[3])) -
                                  min(min(frontKm[0], frontKm[1]), min(frontKm[2], frontKm[3]));
            float sceneSpreadKm = max(max(sceneKm[0], sceneKm[1]), max(sceneKm[2], sceneKm[3])) -
                                  min(min(sceneKm[0], sceneKm[1]), min(sceneKm[2], sceneKm[3]));

            // BOTH channels gate the filter, and they catch different edges. The cloud front catches a
            // cloud against open sky, where every ray runs to the far plane and the scene channel says
            // nothing. The scene distance catches a cloud against GEOMETRY, where the four texels were
            // cut at wildly different places and their radiances are not comparable at all — a mountain
            // ridge against a cloud bank is that case, and averaging across it is how a ridge acquires a
            // bright fringe.
            // Not named `coherent`: that is a GLSL memory qualifier, and using it here is a syntax error
            // the shader parser does not catch — only the runtime compile does.
            bool guideAgrees = frontSpreadKm <= referenceFrontKm * kCoherentFraction &&
                               sceneSpreadKm <= referenceSceneKm * kCoherentFraction;

            vec4 cloud = vec4(0.0);

            if (guideAgrees)
            {
                for (int i = 0; i < 4; ++i)
                    cloud += bilinear[i] * scatter[i];
            }
            else
            {
                float weightSum = 0.0;
                for (int i = 0; i < 4; ++i)
                {
                    // The bilinear weight is kept as a FACTOR rather than replaced. Dropping it is what
                    // turns a bilateral upsample into a nearest-neighbour one: sub-texel position stops
                    // reaching the result and the edge snaps to the low-res grid, trading a soft edge for
                    // a staircase.
                    float weight = bilinear[i] * GuideWeight(abs(frontKm[i] - referenceFrontKm) +
                                                             abs(sceneKm[i] - referenceSceneKm));
                    cloud += weight * scatter[i];
                    weightSum += weight;
                }

                // The reference is a convex combination of the four distances, so at least one neighbour
                // always agrees with it and the sum is positive by construction. The guard is against a
                // NaN arriving from a source texel, not against the arithmetic: it costs one instruction
                // and turns a full-screen white flash into a single dark pixel.
                cloud /= max(weightSum, 1e-8);
            }

            // Radiance is clamped only against negatives, which filtering cannot produce but a NaN in one
            // source texel can propagate into. Transmittance is clamped both ways because it multiplies
            // the scene: a value above 1 brightens what is BEHIND the cloud, which reads as a glowing
            // rectangle and is very hard to trace back to a filter.
            o_Color = vec4(max(cloud.rgb, vec3(0.0)), clamp(cloud.a, 0.0, 1.0));
        }
    }

    Vertex
    {
        #include <Common/QuadPositions.glslh>
        #include <Common/QuadTextureCoords.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
            v_TexCoord  = QUAD_TEXTURE_COORDINATES[gl_VertexIndex];
            gl_Position = vec4(QUAD_POSITIONS[gl_VertexIndex], 0.0, 1.0);
        }
    }
}
