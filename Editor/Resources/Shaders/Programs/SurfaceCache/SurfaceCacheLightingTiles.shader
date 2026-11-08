// DesertAsset {"Kind":"Shader","Guid":"3e9b1f0a6c2d4e58a1f7b2c94d0e6a13","Versions":{"SHDR":1},"Dependencies":[]}
// THE SURFACE CACHE'S LIGHTING TILE LIST (GPU-GI-CRITPATH step 2; UE LumenSceneDirectLighting
// SpliceCardPagesIntoTilesCS + its indirect args): one 8x8 group per candidate tile of every page relit this frame
// (x, y = the tile in its page, z = the page's slot in SurfaceLightingPages); a tile inside its page with at least
// one captured texel (opacity >= 0.5) is appended to the list Common/SurfaceCacheLightingTiles.glslh describes,
// and the consumers' 2D dispatch grows to hold it. Runs on the async compute queue ahead of the Direct Lighting, the
// Radiosity Trace and the Indirect Lighting (Scalability GlobalIllumination.SurfaceCacheAsyncRadiosity).
Shader "SurfaceCacheLightingTiles"
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
        Uniform sampler PointClampSampler;
        Uniform(2) texture2D AtlasOpacityTexture;
        #define AtlasOpacityTexture sampler2D(AtlasOpacityTexture, PointClampSampler)
        #define RADIOSITY_OPACITY AtlasOpacityTexture
        #include <Common/SurfaceCacheRadiosity.glslh> // RadiosityPageTexels: the page's texels
        #define SURFACE_CACHE_TILES_WRITE
        #include <Common/SurfaceCacheLightingTiles.glslh>

        shared uint sCaptured;

        void main()
        {
            if (gl_LocalInvocationIndex == 0u)
                sCaptured = 0u;
            barrier();
            const uint            slot  = gl_WorkGroupID.z;
            const SurfaceCardPage page  = surfaceCardPages[lightingPages[slot]];
            const ivec4           rect  = RadiosityPageTexels(page.physicalAtlasUVRect,
                                                              vec2(textureSize(AtlasOpacityTexture, 0)));
            const ivec2           local = ivec2(gl_GlobalInvocationID.xy);
            if (all(lessThan(local, rect.zw)) && texelFetch(AtlasOpacityTexture, rect.xy + local, 0).r >= 0.5)
                sCaptured = 1u;
            barrier();
            if (gl_LocalInvocationIndex != 0u || sCaptured == 0u)
                return;
            const uint index     = atomicAdd(lightingTileArgs[3], 1u);
            lightingTiles[index] = SurfaceCacheTileWord(slot, gl_WorkGroupID.xy);
            atomicMax(lightingTileArgs[0], min(index + 1u, SURFACE_CACHE_TILE_GRID_WIDTH));
            atomicMax(lightingTileArgs[1], index / SURFACE_CACHE_TILE_GRID_WIDTH + 1u);
        }
    }
}
