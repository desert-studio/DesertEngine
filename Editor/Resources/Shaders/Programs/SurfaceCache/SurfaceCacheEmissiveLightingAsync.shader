// DesertAsset {"Kind":"Shader","Guid":"350151144f034147b6caf58c01657037","Versions":{"SHDR":1},"Dependencies":[]}
// THE SURFACE CACHE'S EMISSIVE LIGHTING ON THE ASYNC COMPUTE QUEUE (GI-TEMPORAL; contract
// Common/EmissiveCardLighting.glslh): every texel of the lighting tiles listed this frame
// (Common/SurfaceCacheLightingTiles.glslh) takes the same estimate as the raster path
// (Common/SurfaceCacheEmissiveLightingTexel.glslh, the selection grid EmissiveGridUB at 47), one 8x8 group per
// tile, ahead of the direct lighting, and blends it into the EmissiveLighting layer as storage:
// stored = mix( loaded, estimate, weight ). Picked with the other
// async card passes (GlobalIllumination.SurfaceCacheAsyncRadiosity). The program is compiled per tracer.
Shader "SurfaceCacheEmissiveLightingAsync"
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
        #include <Common/SurfaceCacheEmissiveLightingTexel.glslh>
        #include <Common/SurfaceCacheLightingTiles.glslh>

        // The raster path's colour target (SurfaceCache::EmissiveLighting::kLayerName), read and written in place.
        layout(binding = 21, rgba16f) restrict uniform image2D AtlasEmissiveLightingImage;

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
            const vec4  estimate   = EmissiveLightingTexel(atlasTexel, page.card.x, page.cardUVRect,
                                                           page.physicalAtlasUVRect, page.card.z);
            const vec4  loaded     = imageLoad(AtlasEmissiveLightingImage, atlasTexel);
            imageStore(AtlasEmissiveLightingImage, atlasTexel,
                       vec4(mix(loaded.rgb, estimate.rgb, estimate.a), 0.0));
        }
    }
}
