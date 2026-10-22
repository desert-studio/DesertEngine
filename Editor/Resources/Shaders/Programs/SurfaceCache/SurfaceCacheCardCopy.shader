// DesertAsset {"Kind":"Shader","Guid":"de9aada61774470181ec163efd173d00","Versions":{"SHDR":1},"Dependencies":[]}
// THE CARD COPY (UE LumenSceneCardCapture.cpp CopyCardsToSurfaceCache + LumenSurfaceCache.usf
// CopyCardCaptureLightingToAtlas): the frame's card captures - each surface's own view-pass G-buffer cell drawn
// through each card's orthographic camera into the capture atlas (SurfaceCacheRenderer) - moved page by page into the
// surface-cache layers. One 8 x 8 group per tile of a 128² page; z = the page copy. The capture's depth is
// reversed-Z of the card box (1 = the card plane), cleared to 0: a texel at 0 saw no surface (opacity 0).
// The Subsurface layer is the surface's shading model read back from the capture's shading word: the subsurface
// colour of its payload (DesertSubsurfaceColor; UE's card capture keeps GetMaterialSubsurfaceData for a subsurface
// model) and its subsurface opacity, 0 for a model that lets no light through.
Shader "SurfaceCacheCardCopy"
{
    Compute
    {
        LocalSize(8, 8, 1);

        Uniform sampler PointClampSampler;
        Uniform(0) texture2D CaptureAlbedoTexture;
        #define CaptureAlbedoTexture sampler2D(CaptureAlbedoTexture, PointClampSampler)
        Uniform(1) texture2D CaptureNormalTexture;
        #define CaptureNormalTexture sampler2D(CaptureNormalTexture, PointClampSampler)
        Uniform(2) texture2D CaptureEmissiveTexture;
        #define CaptureEmissiveTexture sampler2D(CaptureEmissiveTexture, PointClampSampler)
        Uniform(3) texture2D CaptureDepthTexture;
        #define CaptureDepthTexture sampler2D(CaptureDepthTexture, PointClampSampler)
        Uniform(10) utexture2D CaptureShadingWordTexture;
        #define CaptureShadingWordTexture usampler2D(CaptureShadingWordTexture, PointClampSampler)
        // Two uvec4 per page copy (SurfaceCache::CardPageCopy): (capture origin xy, card resolution zw),
        // (atlas origin xy, page size x | y << 16, card texel offset x | y << 16).
        ReadBuffer(4) SurfaceCardPageCopies
        {
            uvec4 Copies[];
        };
        layout(binding = 5, r16) restrict writeonly uniform image2D AtlasDepthTexture;
        layout(binding = 6, rgba8) restrict writeonly uniform image2D AtlasAlbedoTexture;
        layout(binding = 7, r8) restrict writeonly uniform image2D AtlasOpacityTexture;
        layout(binding = 8, rg8) restrict writeonly uniform image2D AtlasNormalTexture;
        layout(binding = 9, r11f_g11f_b10f) restrict writeonly uniform image2D AtlasEmissiveTexture;
        layout(binding = 11, rgba8) restrict writeonly uniform image2D AtlasSubsurfaceTexture;

        #include <Generated/ShadingModels.glslh>

        // Octahedral unit vector -> [-1, 1]² (UE UnitVectorToOctahedron).
        vec2 UnitVectorToOctahedron(vec3 n)
        {
            n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6);
            if (n.z < 0.0)
                n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
            return n.xy;
        }

        void main()
        {
            const uint  copyIndex = gl_WorkGroupID.z;
            const uvec4 capture   = Copies[copyIndex * 2u];
            const uvec4 physical  = Copies[copyIndex * 2u + 1u];
            const uvec2 size      = uvec2(physical.z & 0xFFFFu, physical.z >> 16u);
            const uvec2 texel     = gl_GlobalInvocationID.xy;
            if (any(greaterThanEqual(texel, size)))
                return;
            // Pages of one card overlap by their border texel (stride SURFACE_CACHE_VIRTUAL_PAGE_SIZE); the last
            // page's texels past the card's edge repeat the edge.
            const uvec2 offset    = uvec2(physical.w & 0xFFFFu, physical.w >> 16u);
            const uvec2 cardTexel = min(offset + texel, capture.zw - 1u);
            const ivec2 source    = ivec2(capture.xy + cardTexel);
            const ivec2 target    = ivec2(physical.xy + texel);

            const float depth   = texelFetch(CaptureDepthTexture, source, 0).r;
            const bool  covered = depth > 0.0; // reversed-Z, cleared to 0
            const vec3  normal  = texelFetch(CaptureNormalTexture, source, 0).xyz;
            imageStore(AtlasDepthTexture, target, vec4(covered ? 1.0 - depth : 1.0));
            imageStore(AtlasAlbedoTexture, target, vec4(texelFetch(CaptureAlbedoTexture, source, 0).rgb, 1.0));
            imageStore(AtlasOpacityTexture, target, vec4(covered ? 1.0 : 0.0));
            imageStore(AtlasNormalTexture, target,
                       vec4(covered ? UnitVectorToOctahedron(normal) * 0.5 + 0.5 : vec2(0.5), 0.0, 0.0));
            imageStore(AtlasEmissiveTexture, target, vec4(texelFetch(CaptureEmissiveTexture, source, 0).rgb, 0.0));
            vec4 subsurface = vec4(0.0);
            if (covered)
            {
                const uvec2         word    = texelFetch(CaptureShadingWordTexture, source, 0).rg;
                const int           model   = DesertShadingModelIndex(word);
                const DesertPayload payload = DesertUnpackPayload(word);
                subsurface = vec4(DesertSubsurfaceColor(model, payload),
                                  max(DesertSubsurfaceOpacity(model, payload), 0.0));
            }
            imageStore(AtlasSubsurfaceTexture, target, subsurface);
        }
    }
}
