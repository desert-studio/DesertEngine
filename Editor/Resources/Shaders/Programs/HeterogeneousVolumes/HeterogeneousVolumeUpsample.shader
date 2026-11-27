// DesertAsset {"Kind":"Shader","Guid":"c4e6a8b0d2f44a6c8e0b2d4f6a8c0e72","Versions":{"SHDR":1},"Dependencies":[]}
Shader "HeterogeneousVolumeUpsample"
{
    Compute
    {
        // THE HALF-RES MARCH TO FULL RESOLUTION, depth-aware (the clouds' CloudUpsample rule): each full-res pixel
        // takes the four half-res texels around it, bilinear weights times how well each texel's cut distance (the
        // guide) agrees with the pixel's OWN scene distance - so a smoke plume behind a fence post is not smeared
        // over the post. Outside the volumes' screen rectangle it writes "no medium" (0, 0, 0, 1) at once.
        //
        // Writes the full-res RGBA16F the composite (the clouds' CloudComposite program: scene = rgb x PreExposure
        // + scene x a) blends over the scene colour.

        #include <Common/VolumetricFog.glslh>

        layout(binding = 0, rgba16f) restrict writeonly uniform image2D UpsampledTexture;

        Uniform sampler PointClampSampler;
        Uniform(2) texture2D MarchTexture;
        #define MarchTexture sampler2D(MarchTexture, PointClampSampler)
        Uniform(3) texture2D MarchGuideTexture;
        #define MarchGuideTexture sampler2D(MarchGuideTexture, PointClampSampler)
        Uniform(4) texture2D SceneDepthTexture;
        #define SceneDepthTexture sampler2D(SceneDepthTexture, PointClampSampler)

        #include <Generated/Fog/HeterogeneousVolume.glslh>
        ReadBuffer(17) HeterogeneousVolumeMarchParams { DESERT_HeterogeneousVolumeMarchGpu_MEMBERS };

        LocalSize(8, 8, 1);

        void main()
        {
            uvec2 pixel = gl_GlobalInvocationID.xy;
            if (any(greaterThanEqual(vec2(pixel), HvTarget.zw)))
                return;
            vec2 uv = (vec2(pixel) + 0.5) / HvTarget.zw;
            // One half-res texel of margin around the rectangle, so the bilinear footprint at its edge is complete.
            vec2 margin = 2.0 / HvTarget.xy;
            if (any(lessThan(uv, HvRect.xy - margin)) || any(greaterThan(uv, HvRect.zw + margin)))
            {
                imageStore(UpsampledTexture, ivec2(pixel), vec4(0.0, 0.0, 0.0, 1.0));
                return;
            }

            float device = texelFetch(SceneDepthTexture, ivec2(pixel), 0).r;
            float own    = 1e9;
            vec2  ndc    = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
            if (device > 0.0)
            {
                vec4 h = VfInvViewProjection * vec4(ndc, device, 1.0);
                own    = length(h.xyz / h.w - VfCamera.xyz);
            }

            ivec2 halfSize = ivec2(HvTarget.xy);
            vec2  coord    = uv * HvTarget.xy - 0.5;
            ivec2 base     = ivec2(floor(coord));
            vec2  f        = coord - vec2(base);
            vec4  sum      = vec4(0.0);
            float weights  = 0.0;
            for (int i = 0; i < 4; ++i)
            {
                ivec2 o     = ivec2(i & 1, i >> 1);
                ivec2 texel = clamp(base + o, ivec2(0), halfSize - 1);
                float bil   = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
                float guide = texelFetch(MarchGuideTexture, texel, 0).r;
                float rel   = abs(guide - own) / max(min(guide, own), 1.0);
                float w     = bil * (1.0 / (1e-3 + rel * rel * 100.0)) + 1e-6;
                sum     += w * texelFetch(MarchTexture, texel, 0);
                weights += w;
            }
            vec4 result = sum / max(weights, 1e-6);
            imageStore(UpsampledTexture, ivec2(pixel),
                       vec4(max(result.rgb, vec3(0.0)), clamp(result.a, 0.0, 1.0)));
        }
    }
}
