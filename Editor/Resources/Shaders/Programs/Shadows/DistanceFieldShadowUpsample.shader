// DesertAsset {"Kind":"Shader","Guid":"5a4b422f28b847fb804383b3dd25fb8e","Versions":{"SHDR":1},"Dependencies":[]}
// "SHADOWS: DISTANCE FIELD SUN", THE UPSAMPLE (UE DistanceFieldShadowing.usf's bilateral upsample of the half
// resolution trace, written from memory): every full-resolution pixel takes the four half-resolution texels around
// it, each weighted bilinearly and by how close its receiver's distance from the camera lies to the pixel's own - a
// tap on another surface (a trunk in front of the field behind it) gives nothing - into the screen mask the deferred
// direct lighting cross-fades with the cascades (Common/DistanceFieldShadow.glslh, the band). r = visibility.
Shader "DistanceFieldShadowUpsample"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/DistanceFieldShadow.glslh>
        #include <Common/ReconstructPosition.glslh>

        Uniform sampler PointClampSampler;

        ReadBuffer(0) DistanceFieldShadowParamsUB
        {
            DistanceFieldShadowParams ShadowParams;
        };
        Uniform(1) texture2D GBufferDepthTexture; // device depth, reversed-Z (0 = far: no surface)
        #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
        Uniform(2) texture2D DistanceFieldShadowHalfTexture; // r = visibility, g = receiver distance (m), 0 = sky
        #define DistanceFieldShadowHalfTexture sampler2D(DistanceFieldShadowHalfTexture, PointClampSampler)
        layout(binding = 3, rgba16f) writeonly uniform image2D DistanceFieldShadowTexture;

        // The relative distance difference at which a tap's weight halves.
        const float kDepthTolerance = 0.02;

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            const ivec2 size  = ivec2(ShadowParams.extent.xy);
            if (pixel.x >= size.x || pixel.y >= size.y)
                return;
            const float depth = texelFetch(GBufferDepthTexture, pixel, 0).r;
            if (depth <= 0.0)
            {
                imageStore(DistanceFieldShadowTexture, pixel, vec4(1.0));
                return;
            }
            const vec2  uv       = (vec2(pixel) + 0.5) / ShadowParams.extent.xy;
            const vec3  position = ReconstructWorldPosition(uv, depth, ShadowParams.invViewProjection);
            const float metres   = length(position - ShadowParams.camera.xyz) * 0.01;

            // The half-resolution texel centres around the pixel: texel h stands for full pixel 2h (the trace's).
            const vec2  halfPos = vec2(pixel) * 0.5;
            const ivec2 base    = ivec2(floor(halfPos));
            const vec2  f       = halfPos - vec2(base);
            const ivec2 last    = ivec2(ShadowParams.extent.zw) - 1;
            float sum    = 0.0;
            float weight = 0.0;
            for (int k = 0; k < 4; ++k)
            {
                const ivec2 o   = ivec2(k & 1, k >> 1);
                const ivec2 tap = clamp(base + o, ivec2(0), last);
                const vec2  s   = texelFetch(DistanceFieldShadowHalfTexture, tap, 0).rg;
                const float bilinear = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
                const float relative = abs(s.y - metres) / max(metres, 1.0e-3);
                const float w        = (bilinear + 1.0e-3) / (1.0 + relative / kDepthTolerance) * (s.y > 0.0 ? 1.0 : 1.0e-3);
                sum    += s.x * w;
                weight += w;
            }
            imageStore(DistanceFieldShadowTexture, pixel, vec4(weight > 0.0 ? sum / weight : 1.0));
        }
    }
}
