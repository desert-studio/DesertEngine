// DesertAsset {"Kind":"Shader","Guid":"c5a46a23f83d4b2a8d7288b2a97fdc10","Versions":{"SHDR":1},"Dependencies":[]}
// THE SURFACE CACHE'S INDIRECT LIGHTING ON THE ASYNC COMPUTE QUEUE (GPU-GI-CRITPATH; UE LumenRadiosity.usf
// LumenRadiosityIntegrateCS + the combine of LumenSceneLighting.usf, on async compute with the rest of Lumen's scene
// lighting): every atlas texel of the pages relit this frame integrates its card's radiosity probes and recombines
// FinalLighting - the same per-texel work as the raster path (Common/SurfaceCacheIndirectLightingTexel.glslh), one
// 8x8 group per listed lighting tile (Common/SurfaceCacheLightingTiles.glslh), written as storage images.
// Picked by Scalability GlobalIllumination.SurfaceCacheAsyncRadiosity with SurfaceCacheRadiosityTraceAsync.shader.
Shader "SurfaceCacheIndirectLightingAsync"
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
        #include <Common/SurfaceCacheIndirectLightingTexel.glslh>
        #define SURFACE_CACHE_TILES_BINDING 60
        #include <Common/SurfaceCacheLightingTiles.glslh>

        // The raster path's two colour targets (SurfaceCache::Layer::IndirectLighting, FinalLighting).
        layout(binding = 16, r11f_g11f_b10f) restrict writeonly uniform image2D AtlasIndirectLightingImage;
        layout(binding = 17, r11f_g11f_b10f) restrict writeonly uniform image2D AtlasFinalLightingImage;

        void main()
        {
            uint  slot;
            ivec2 tileOrigin;
            if ( !SurfaceCacheLightingTile( slot, tileOrigin ) )
                return;
            const SurfaceCardPage page  = surfaceCardPages[lightingPages[slot]];
            const ivec4           rect  = RadiosityPageTexels( page.physicalAtlasUVRect,
                                                               vec2( textureSize( AtlasOpacityTexture, 0 ) ) );
            const ivec2           local = tileOrigin + ivec2( gl_LocalInvocationID.xy );
            if ( any( greaterThanEqual( local, rect.zw ) ) )
                return;
            const ivec2 atlasTexel = rect.xy + local;
            vec4        indirect;
            vec4        lit;
            IndirectLightingTexel( atlasTexel, page.card.x, page.cardUVRect, page.physicalAtlasUVRect, indirect,
                                   lit );
            imageStore( AtlasIndirectLightingImage, atlasTexel, indirect );
            imageStore( AtlasFinalLightingImage, atlasTexel, lit );
        }
    }
}
