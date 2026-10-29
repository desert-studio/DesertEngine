// DesertAsset {"Kind":"Shader","Guid":"7d6d35d06fe449379ca22316bedad253","Versions":{"SHDR":1},"Dependencies":[]}
// The "Surface Cache Atlas" view mode (DeferredDebugMode::SurfaceCacheAtlas; UE r.Lumen.Visualize "Surface Cache"
// atlas view): the captured layers of the whole atlas, a quadrant each - albedo top left, normal top right
// (octahedral, decoded, shown as 0.5 + 0.5 n), depth bottom left (card plane white, far side black, no surface
// dark red), opacity bottom right.
Shader "SurfaceCacheAtlasView"
{
    Vertex
    {
        #include <Common/FullscreenTriangle.glslh>
        Out(0) vec2 v_Ndc;
        void main()
        {
        	v_Ndc       = FullscreenTriangleNdc();
        	gl_Position = vec4(FullscreenTriangleNdc(), 0.0, 1.0);
        }
    }
    Fragment
    {
        In(0) vec2 v_Ndc;
        Uniform sampler PointClampSampler;
        Uniform(0) texture2D AtlasDepthTexture;
        #define AtlasDepthTexture sampler2D(AtlasDepthTexture, PointClampSampler)
        Uniform(1) texture2D AtlasAlbedoTexture;
        #define AtlasAlbedoTexture sampler2D(AtlasAlbedoTexture, PointClampSampler)
        Uniform(2) texture2D AtlasOpacityTexture;
        #define AtlasOpacityTexture sampler2D(AtlasOpacityTexture, PointClampSampler)
        Uniform(3) texture2D AtlasNormalTexture;
        #define AtlasNormalTexture sampler2D(AtlasNormalTexture, PointClampSampler)
        Out(0) vec4 oColor;

        vec3 OctahedronToUnitVector(vec2 o)
        {
            vec3 n = vec3(o, 1.0 - abs(o.x) - abs(o.y));
            const float t = max(-n.z, 0.0);
            n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
            return normalize(n);
        }

        void main()
        {
            // Top-down image coordinates (the engine's viewport keeps +Y up in NDC).
            const vec2 uv    = vec2(0.5 + 0.5 * v_Ndc.x, 0.5 - 0.5 * v_Ndc.y);
            const vec2 q     = floor(uv * 2.0);
            const vec2 local = fract(uv * 2.0);
            const ivec2 size = textureSize(AtlasAlbedoTexture, 0);
            const ivec2 texel = clamp(ivec2(local * vec2(size)), ivec2(0), size - 1);
            const float opacity = texelFetch(AtlasOpacityTexture, texel, 0).r;
            vec3 color;
            if (q.y < 0.5)
                color = q.x < 0.5 ? texelFetch(AtlasAlbedoTexture, texel, 0).rgb
                                  : (opacity > 0.5 ? OctahedronToUnitVector(texelFetch(AtlasNormalTexture, texel, 0).rg * 2.0 - 1.0) * 0.5 + 0.5
                                                   : vec3(0.0));
            else
                color = q.x < 0.5 ? (opacity > 0.5 ? vec3(1.0 - texelFetch(AtlasDepthTexture, texel, 0).r) : vec3(0.25, 0.0, 0.0))
                                  : vec3(opacity);
            // A thin grid of the 128² physical pages.
            const vec2 pageEdge = abs(fract(local * vec2(size) / 128.0) - 0.5);
            if (max(pageEdge.x, pageEdge.y) > 0.5 - 0.5 * float(size.x) / 128.0 / 512.0)
                color = mix(color, vec3(0.2, 0.2, 0.35), 0.5);
            oColor = vec4(color, 1.0);
        }
    }
}
