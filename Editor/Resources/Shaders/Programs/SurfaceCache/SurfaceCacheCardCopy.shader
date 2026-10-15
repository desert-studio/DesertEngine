// DesertAsset {"Kind":"Shader","Guid":"de9aada61774470181ec163efd173d00","Versions":{"SHDR":1},"Dependencies":[]}
// THE CARD COPY (UE LumenSceneCardCapture.cpp CopyCardsToSurfaceCache + LumenSurfaceCache.usf
// CopyCardCaptureLightingToAtlas): the frame's card captures - the G-buffer light-view permutation drawn through
// each card's orthographic camera into the capture atlas (SurfaceCacheRenderer) - moved page by page into the
// surface-cache layers. One 8 x 8 group per tile of a 128² page; z = the page copy. The capture's depth is
// standard-Z of the card box (0 = the card plane), cleared to 1: a texel at 1 saw no surface (opacity 0).
Shader "SurfaceCacheCardCopy"
{
    Compute
    {
        LocalSize(8, 8, 1);

        Uniform(0) sampler2D u_CaptureAlbedo;
        Uniform(1) sampler2D u_CaptureNormal;
        Uniform(2) sampler2D u_CaptureEmissive;
        Uniform(3) sampler2D u_CaptureDepth;
        // Two uvec4 per page copy (SurfaceCache::CardPageCopy): (capture origin xy, card resolution zw),
        // (atlas origin xy, page size x | y << 16, card texel offset x | y << 16).
        ReadBuffer(4) SurfaceCardPageCopies
        {
            uvec4 u_Copies[];
        };
        layout(binding = 5, r16) restrict writeonly uniform image2D u_AtlasDepth;
        layout(binding = 6, rgba8) restrict writeonly uniform image2D u_AtlasAlbedo;
        layout(binding = 7, r8) restrict writeonly uniform image2D u_AtlasOpacity;
        layout(binding = 8, rg8) restrict writeonly uniform image2D u_AtlasNormal;
        layout(binding = 9, r11f_g11f_b10f) restrict writeonly uniform image2D u_AtlasEmissive;

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
            const uvec4 capture   = u_Copies[copyIndex * 2u];
            const uvec4 physical  = u_Copies[copyIndex * 2u + 1u];
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

            const float depth   = texelFetch(u_CaptureDepth, source, 0).r;
            const bool  covered = depth < 1.0;
            const vec3  normal  = texelFetch(u_CaptureNormal, source, 0).xyz;
            imageStore(u_AtlasDepth, target, vec4(covered ? depth : 1.0));
            imageStore(u_AtlasAlbedo, target, vec4(texelFetch(u_CaptureAlbedo, source, 0).rgb, 1.0));
            imageStore(u_AtlasOpacity, target, vec4(covered ? 1.0 : 0.0));
            imageStore(u_AtlasNormal, target,
                       vec4(covered ? UnitVectorToOctahedron(normal) * 0.5 + 0.5 : vec2(0.5), 0.0, 0.0));
            imageStore(u_AtlasEmissive, target, vec4(texelFetch(u_CaptureEmissive, source, 0).rgb, 0.0));
        }
    }
}
