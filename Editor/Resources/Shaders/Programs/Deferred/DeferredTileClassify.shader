// DesertAsset {"Kind":"Shader","Guid":"7c3e9a51d2b84f06a1e5c8d93b7f2e4a","Versions":{"SHDR":1},"Dependencies":[]}
Shader "DeferredTileClassify"
{
    // "Deferred: Tile Classify" (GPU-TILE-CLASSIFY; UE SubstrateTileClassification): one workgroup per 8x8 tile of
    // the G-buffer sorts the tile into the lists of Common/DeferredTiles.glslh - none (no geometry texel: the
    // sky), SIMPLE (every geometry texel DefaultLit) or COMPLEX (any other shading model) - and counts it into
    // its list's indirect draw. The tests are the lighting's own: a texel is geometry when its G-buffer normal is
    // not zero (DeferredLighting.shader's hasGeometry), its model is the shading word's (DesertShadingModelIndex,
    // read at the same texel by texelFetch).
    // FPS-DIRECT-TILES (UE ClusteredDeferredShading's per-shading-model tile types): the tile's SET of models (a
    // bitmask of model indices) also sorts every COMPLEX tile for the direct lighting: into the list of its one
    // model m (every geometry texel model m, and bit m in the header's kDeferredTileModelMask), drawn with the model
    // folded, or else into the MIXED list, drawn with the dispatch.

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

        // Bit m: a geometry texel of the tile is shading model m (DesertShadingModelIndex, 0..15); 0: the sky only.
        shared uint s_TileModels;

        void main()
        {
        	if (gl_LocalInvocationIndex == 0u)
        		s_TileModels = 0u;
        	barrier();

        	const uvec2 extent = uvec2(deferredTiles[kDeferredTileExtent], deferredTiles[kDeferredTileExtent + 1u]);
        	const uvec2 pixel  = gl_GlobalInvocationID.xy;
        	if (all(lessThan(pixel, extent)))
        	{
        		const vec3 normal = texelFetch(GBufferBTexture, ivec2(pixel), 0).rgb;
        		if (dot(normal, normal) > 0.001)
        		{
        			const int model = DesertShadingModelIndex(texelFetch(GBufferShadingWordTexture, ivec2(pixel), 0).rg);
        			atomicOr(s_TileModels, 1u << uint(model));
        		}
        	}
        	barrier();

        	const uint models = s_TileModels;
        	if (gl_LocalInvocationIndex != 0u || models == 0u)
        		return;
        	const uint tileCount = deferredTiles[kDeferredTileExtent + 3u];
        	const uint word      = DeferredTilePack(gl_WorkGroupID.xy);
        	// Every listed tile is a workgroup of the geometry dispatch (SSAO), a complex one of the complex one too.
        	atomicAdd(deferredTiles[kDeferredTileGeometryDispatch], 1u);
        	if (models != (1u << uint(SHADING_MODEL_INDEX_DEFAULT_LIT)))
        	{
        		atomicAdd(deferredTiles[kDeferredTileComplexDispatch], 1u);
        		const uint slot = atomicAdd(deferredTiles[kDeferredTileComplexArgs], kDeferredTileVertices) /
        		                  kDeferredTileVertices;
        		deferredTiles[kDeferredTileHeaderWords + tileCount + slot] = word;
        		// The direct lighting's split of the same tile: the list of its one model, or the mixed list.
        		const uint model = uint(findLSB(models));
        		if (bitCount(models) == 1 && (deferredTiles[kDeferredTileModelMask] & models) != 0u)
        		{
        			const uint modelSlot =
        			     atomicAdd(deferredTiles[kDeferredTileModelArgs + 4u * model], kDeferredTileVertices) /
        			     kDeferredTileVertices;
        			deferredTiles[kDeferredTileHeaderWords + (3u + model) * tileCount + modelSlot] = word;
        		}
        		else
        		{
        			const uint mixedSlot =
        			     atomicAdd(deferredTiles[kDeferredTileMixedArgs], kDeferredTileVertices) / kDeferredTileVertices;
        			deferredTiles[kDeferredTileHeaderWords + 2u * tileCount + mixedSlot] = word;
        		}
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
