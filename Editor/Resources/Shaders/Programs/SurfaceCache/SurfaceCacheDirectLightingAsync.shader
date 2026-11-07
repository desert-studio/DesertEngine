// DesertAsset {"Kind":"Shader","Guid":"8d41c7e25b9f4a06b3e1d0f72a5c9e84","Versions":{"SHDR":1},"Dependencies":[]}
// THE SURFACE CACHE'S DIRECT LIGHTING ON THE ASYNC COMPUTE QUEUE (GPU-GI-CRITPATH step 2; UE
// LumenSceneDirectLighting's card-tile compute pass, on async compute with the rest of Lumen's scene lighting):
// every texel of the lighting tiles listed this frame (Common/SurfaceCacheLightingTiles.glslh) lit by the same per-texel
// work as the raster path (Common/SurfaceCacheDirectLightingTexel.glslh), one 8x8 group per tile, the two layers
// the raster path draws as colour targets written as storage. Picked by Scalability
// GlobalIllumination.SurfaceCacheAsyncRadiosity with SurfaceCacheRadiosityTraceAsync and
// SurfaceCacheIndirectLightingAsync; 0 keeps the raster pass on the graphics queue for an A/B.
// The program is compiled per tracer and local shadow kind (SurfaceCacheRenderer DirectLightingVariant).
Shader "SurfaceCacheDirectLightingAsync"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/SurfaceCache.glslh>
        ReadBuffer(0) SurfaceCardPageBuffer
        {
            SurfaceCardPage surfaceCardPages[];
        };
        ReadBuffer(1) SurfaceLightingPages
        {
            uint lightingPages[];
        };
        #include <Common/SurfaceCacheDirectLightingTexel.glslh>
        #define RADIOSITY_OPACITY AtlasOpacityTexture
        #include <Common/SurfaceCacheRadiosity.glslh> // RadiosityPageTexels: the page's texels
        #define SURFACE_CACHE_TILES_BINDING 60
        #include <Common/SurfaceCacheLightingTiles.glslh>

        // The raster path's two colour targets (SurfaceCache::Layer::DirectLighting, FinalLighting).
        layout(binding = 21, r11f_g11f_b10f) restrict writeonly uniform image2D AtlasDirectLightingImage;
        layout(binding = 22, r11f_g11f_b10f) restrict writeonly uniform image2D AtlasFinalLightingImage;

        void main()
        {
            uint  slot;
            ivec2 tileOrigin;
            if (!SurfaceCacheLightingTile(slot, tileOrigin))
                return;
            const SurfaceCardPage page  = surfaceCardPages[lightingPages[slot]];
            const ivec4           rect  = RadiosityPageTexels(page.physicalAtlasUVRect,
                                                              vec2(textureSize(AtlasOpacityTexture, 0)));
            const ivec2           local = tileOrigin + ivec2(gl_LocalInvocationID.xy);
            if (any(greaterThanEqual(local, rect.zw)))
                return;
            const ivec2 atlasTexel = rect.xy + local;
            vec4        direct;
            vec4        lit;
            DirectLightingTexel(atlasTexel, page.card.x, page.cardUVRect, page.physicalAtlasUVRect, page.card.z,
                                direct, lit);
            imageStore(AtlasDirectLightingImage, atlasTexel, direct);
            imageStore(AtlasFinalLightingImage, atlasTexel, lit);
        }
    }
}
