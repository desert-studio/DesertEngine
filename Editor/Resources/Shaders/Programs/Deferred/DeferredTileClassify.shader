// DesertAsset {"Kind":"Shader","Guid":"7c3e9a51d2b84f06a1e5c8d93b7f2e4a","Versions":{"SHDR":1},"Dependencies":[]}
Shader "DeferredTileClassify"
{
    // "Deferred: Tile Classify" (GPU-TILE-CLASSIFY; UE SubstrateTileClassification): one workgroup per 8x8 tile of
    // the G-buffer sorts the tile into the lists of Common/DeferredTiles.glslh - none (no geometry texel: the
    // sky), SIMPLE (every geometry texel DefaultLit) or COMPLEX (any other shading model) - and counts it into
    // its list's indirect draw. The tests are the lighting's own: a texel is geometry when its G-buffer normal is
    // not zero (DeferredLighting.shader's hasGeometry), its model is the shading word's (DesertShadingModelIndex,
    // read at the same texel by texelFetch).

    Compute
    {
        LocalSize(8, 8, 1);

        Uniform sampler PointClampSampler;
        Uniform(0) texture2D GBufferBTexture; // rgb = world normal (zero where nothing was drawn), a = roughness
        #define GBufferBTexture sampler2D(GBufferBTexture, PointClampSampler)
        Uniform(1) utexture2D GBufferShadingWordTexture;
        #define GBufferShadingWordTexture usampler2D(GBufferShadingWordTexture, PointClampSampler)
        Buffer(2) DeferredTilesUB
        {
        	uint deferredTiles[];
        };

        #include <Generated/ShadingModels.glslh>
        #include <Common/DeferredTiles.glslh>

        const uint kTileHasGeometry = 1u;
        const uint kTileIsComplex   = 2u;
        shared uint s_TileFlags;

        void main()
        {
        	if (gl_LocalInvocationIndex == 0u)
        		s_TileFlags = 0u;
        	barrier();

        	const uvec2 extent = uvec2(deferredTiles[kDeferredTileExtent], deferredTiles[kDeferredTileExtent + 1u]);
        	const uvec2 pixel  = gl_GlobalInvocationID.xy;
        	if (all(lessThan(pixel, extent)))
        	{
        		const vec3 normal = texelFetch(GBufferBTexture, ivec2(pixel), 0).rgb;
        		if (dot(normal, normal) > 0.001)
        		{
        			const int model = DesertShadingModelIndex(texelFetch(GBufferShadingWordTexture, ivec2(pixel), 0).rg);
        			const uint flags = model == SHADING_MODEL_INDEX_DEFAULT_LIT ? kTileHasGeometry
        			                                                            : (kTileHasGeometry | kTileIsComplex);
        			atomicOr(s_TileFlags, flags);
        		}
        	}
        	barrier();

        	if (gl_LocalInvocationIndex != 0u || s_TileFlags == 0u)
        		return;
        	const uint tileCount = deferredTiles[kDeferredTileExtent + 3u];
        	const uint word      = DeferredTilePack(gl_WorkGroupID.xy);
        	// Every listed tile is a workgroup of the geometry dispatch (SSAO), a complex one of the complex one too.
        	atomicAdd(deferredTiles[kDeferredTileGeometryDispatch], 1u);
        	if ((s_TileFlags & kTileIsComplex) != 0u)
        	{
        		atomicAdd(deferredTiles[kDeferredTileComplexDispatch], 1u);
        		const uint slot = atomicAdd(deferredTiles[kDeferredTileComplexArgs], kDeferredTileVertices) /
        		                  kDeferredTileVertices;
        		deferredTiles[kDeferredTileHeaderWords + tileCount + slot] = word;
        	}
        	else
        	{
        		const uint slot = atomicAdd(deferredTiles[kDeferredTileSimpleArgs], kDeferredTileVertices) /
        		                  kDeferredTileVertices;
        		deferredTiles[kDeferredTileHeaderWords + slot] = word;
        	}
        }
    }
}
