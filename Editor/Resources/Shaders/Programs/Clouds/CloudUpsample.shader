// DesertAsset {"Kind":"Shader","Guid":"09a50469d74c49d7ba27609e75e06845","Versions":{"SHDR":1},"Dependencies":[]}
Shader "CloudUpsample"
{
    Compute
    {
        // THE FULL-RESOLUTION COMPOSE — Unreal's ComposeVolumetricRTOverScene (VolumetricRenderTarget.usf) with
        // its depth-aware upsample (r.VolumetricRenderTarget.UpsamplingMode 4), as a compute pass because it
        // needs the one input the raster composite cannot have: THIS PIXEL'S OWN SCENE DEPTH. CloudComposite
        // draws into the scene target with the depth attached, and sampling a bound attachment is a feedback
        // loop; here, ahead of the translucency phase, the depth is an ordinary sampled image (the same
        // SceneDepth.Compute the march and the resolve read).
        //
        // WHAT IT REPLACES AND WHY (FOLIAGE-CLOUD-FLICKER). The composite used to upsample the half-resolution
        // reconstruction against the BILINEAR BLEND of the four guide distances, its best guess at the pixel's
        // distance. At a grass blade or a leaf against the sky, that guess is a mixture of the blade and the sky
        // and says nothing about which of the two THIS pixel is. Which side the bilateral weights then chose
        // followed the half-res guide, and the guide's scene channel followed one jittered full-res texel per
        // block: a blade pixel received the sky's cloud on one frame and none on the next, the sky between two
        // blades lost its cloud and got it back, and TAA was handed a different answer at every foliage edge
        // every frame — the "foliage and grass flicker against the clouds".
        //
        // THE RULE, per full-resolution pixel at scene distance d (km, the far plane for the sky):
        //   * a half-res texel whose CLOUD FRONT lies at or behind d has no cloud in front of this pixel's
        //     surface — the surface occludes all of it. It contributes "clear" (no radiance, transmittance 1)
        //     and it AGREES with the pixel: that answer is exact here. This is the cloud kept behind opaque by
        //     the cloud front, and it is what makes a blade in front of a cloud take no cloud on any frame,
        //     whatever the half-res guide around it did;
        //   * any other texel contributes its scatter, weighted by how well the distance its ray was cut at
        //     (guide .y, the farthest surface of its block, Common/CloudSceneDepth.glslh) agrees with d, and by
        //     how well its front agrees with the neighbourhood's.
        // Over open sky both rules reduce to the composite's previous filter: every d is the far plane, every
        // .y is the far plane, and only the cloud-front term is left.
        //
        // BILINEAR STAYS THE COMMON PATH, as before: a filter that rejects neighbours everywhere quantizes the
        // result onto the half-res grid. The four texels are tested for coherence first against Unreal's
        // threshold of a tenth of the reference distance, and only a genuine discontinuity pays for the
        // weighted path. The weighted path keeps the bilinear weight as a FACTOR, so the edge stays sub-texel.
        //
        // LINEAR HDR, ABSOLUTE: premultiplied radiance in .rgb, transmittance in .a, exactly as the resolve
        // wrote them. The view's PreExposure is applied by CloudComposite, which only copies this image onto the
        // scene with the over-operator.

        // Included for CLOUD_WORLD_UNITS_PER_KM alone (see CloudTemporalResolve.shader).
        #include <Common/CloudGeometry.glslh>

        // The output: one texel per framebuffer pixel, read 1:1 by CloudComposite.shader.
        layout(binding = 3, rgba16f) restrict writeonly uniform image2D UpsampledCloudTexture;

        // Shared sampler states (UE's shared samplers), as in the resolve.
        Uniform(8) sampler LinearClampSampler;
        Uniform(9) sampler GlobalFilterSampler;

        // This frame's HALF-resolution reconstruction (CloudTemporalResolve.shader): the scatter and its guide
        // (.x cloud front distance, .y the distance the ray was cut at, kilometres). Point-fetched only — a
        // filtered distance across a silhouette is a distance where nothing is.
        Uniform(0) texture2D CloudReconstructionTexture;
        #define CloudReconstructionTexture sampler2D(CloudReconstructionTexture, LinearClampSampler)
        Uniform(1) texture2D CloudReconstructionGuideTexture;
        #define CloudReconstructionGuideTexture sampler2D(CloudReconstructionGuideTexture, LinearClampSampler)

        // The single-sample scene depth, full resolution: this pixel's own surface.
        Uniform(2) texture2D SceneDepthTexture;
        #define SceneDepthTexture sampler2D(SceneDepthTexture, GlobalFilterSampler)

        // InverseViewProjection (the JITTERED inverse, matching the depth it un-projects) and CameraPosition.
        #include <Common/CloudResolveParams.glslh>

        LocalSize(8, 8, 1);

        // Unreal's coherence threshold: a tenth of the reference distance. Relative, because a kilometre of
        // disagreement is a silhouette at three kilometres and nothing at fifty.
        const float kCoherentFraction = 0.1f;

        // A distance disagreement in kilometres as a weight: a neighbour ten metres away still counts, one a
        // kilometre away does not.
        float GuideWeight(float deltaKm)
        {
            return 1.0f / (deltaKm * 1000.0f + 1.0f);
        }

        void main()
        {
            ivec2 size  = imageSize(UpsampledCloudTexture);
            ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
            if (coord.x >= size.x || coord.y >= size.y)
                return;

            // THIS PIXEL'S SCENE DISTANCE, by the march's expression: the far plane where nothing was drawn,
            // the opaque surface where something was. REVERSED-Z: 0 is the far plane and the sky.
            vec2 uv  = (vec2(coord) + vec2(0.5f, 0.5f)) / vec2(size);
            vec2 ndc = vec2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

            vec4 farH = InverseViewProjection * vec4(ndc.x, ndc.y, 0.0f, 1.0f);
            vec3 farP = farH.xyz / max(farH.w, 1e-9f);
            float pixelKm = length(farP - CameraPosition) * (1.0f / CLOUD_WORLD_UNITS_PER_KM);

            ivec2 depthSize   = textureSize(SceneDepthTexture, 0);
            float deviceDepth = texelFetch(SceneDepthTexture, clamp(coord, ivec2(0, 0), depthSize - ivec2(1, 1)), 0).r;
            if (deviceDepth > 0.0f)
            {
                vec4 geomH = InverseViewProjection * vec4(ndc.x, ndc.y, deviceDepth, 1.0f);
                vec3 geomP = geomH.xyz / max(geomH.w, 1e-9f);
                pixelKm = length(geomP - CameraPosition) * (1.0f / CLOUD_WORLD_UNITS_PER_KM);
            }

            // The 2x2 half-res footprint and its bilinear fractions, from one expression (the half-texel shift
            // is the bilinear rule: texel centres sit at (i + 0.5) / halfSize).
            ivec2 halfSize = textureSize(CloudReconstructionTexture, 0);
            vec2  texel    = uv * vec2(halfSize) - vec2(0.5f, 0.5f);
            vec2  frac     = texel - floor(texel);
            ivec2 base     = ivec2(floor(texel));
            ivec2 last     = halfSize - ivec2(1, 1);

            ivec2 coords[4];
            coords[0] = clamp(base + ivec2(0, 0), ivec2(0, 0), last);
            coords[1] = clamp(base + ivec2(1, 0), ivec2(0, 0), last);
            coords[2] = clamp(base + ivec2(0, 1), ivec2(0, 0), last);
            coords[3] = clamp(base + ivec2(1, 1), ivec2(0, 0), last);

            float bilinear[4];
            bilinear[0] = (1.0f - frac.x) * (1.0f - frac.y);
            bilinear[1] = frac.x * (1.0f - frac.y);
            bilinear[2] = (1.0f - frac.x) * frac.y;
            bilinear[3] = frac.x * frac.y;

            vec4  scatter[4];
            float frontKm[4]; // the cloud front, clamped at this pixel's surface
            float cutDeltaKm[4]; // how far the texel's cut distance is from this pixel's surface

            for (int i = 0; i < 4; ++i)
            {
                vec4 s     = texelFetch(CloudReconstructionTexture, coords[i], 0);
                vec2 guide = texelFetch(CloudReconstructionGuideTexture, coords[i], 0).xy;
                if (guide.x >= pixelKm)
                {
                    // The whole of this texel's cloud lies behind this pixel's surface.
                    scatter[i]    = vec4(0.0f, 0.0f, 0.0f, 1.0f);
                    frontKm[i]    = pixelKm;
                    cutDeltaKm[i] = 0.0f;
                }
                else
                {
                    scatter[i]    = s;
                    frontKm[i]    = guide.x;
                    cutDeltaKm[i] = abs(guide.y - pixelKm);
                }
            }

            float referenceFrontKm = 0.0f;
            for (int i = 0; i < 4; ++i)
                referenceFrontKm += bilinear[i] * frontKm[i];

            float frontSpreadKm = max(max(frontKm[0], frontKm[1]), max(frontKm[2], frontKm[3])) -
                                  min(min(frontKm[0], frontKm[1]), min(frontKm[2], frontKm[3]));
            float cutSpreadKm   = max(max(cutDeltaKm[0], cutDeltaKm[1]), max(cutDeltaKm[2], cutDeltaKm[3]));

            // BOTH terms gate the filter, as in the composite this replaces: the cloud front catches a cloud
            // edge against open sky, the cut distance catches a cloud against geometry.
            // Not named `coherent`: that is a GLSL memory qualifier.
            bool guideAgrees = frontSpreadKm <= referenceFrontKm * kCoherentFraction &&
                               cutSpreadKm <= pixelKm * kCoherentFraction;

            vec4 cloud = vec4(0.0f);
            if (guideAgrees)
            {
                for (int i = 0; i < 4; ++i)
                    cloud += bilinear[i] * scatter[i];
            }
            else
            {
                float weightSum = 0.0f;
                for (int i = 0; i < 4; ++i)
                {
                    float weight = bilinear[i] * GuideWeight(abs(frontKm[i] - referenceFrontKm) + cutDeltaKm[i]);
                    cloud += weight * scatter[i];
                    weightSum += weight;
                }
                // Positive by construction for finite inputs (some bilinear weight is positive and every
                // GuideWeight is); the guard turns a NaN arriving from a source texel into one dark pixel
                // rather than a white flash.
                cloud /= max(weightSum, 1e-8f);
            }

            // Radiance clamped against negatives (a NaN can bring one), transmittance both ways: above 1 it
            // would brighten what is behind the cloud.
            imageStore(UpsampledCloudTexture, coord, vec4(max(cloud.rgb, vec3(0.0f)), clamp(cloud.a, 0.0f, 1.0f)));
        }
    }
}
