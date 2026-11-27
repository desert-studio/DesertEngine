// DesertAsset {"Kind":"Shader","Guid":"b71c37b879074c71ac8f02932b429462","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsVisibleHash"
{
    // "MegaLights: Visible Light Hash" (UE MegaLightsVisibleLightHash.usf): the lights whose samples were visible,
    // per tile of 8x8 sample pixels (Common/MegaLightsSample.glslh), keyed by this frame's rows. The next frame's
    // "MegaLights: Generate Samples" draws the others less often. Emissive triangles are not marked: their draws
    // come from the power tree, not from the light grid.

    Compute
    {
        Uniform sampler PointClampSampler;
        #include <Common/MegaLights.glslh>

        // "MegaLights: Trace Samples", sample-pixel sized, stacked layers
        layout(set = 0, binding = 10) uniform utexture2D SamplesTexture;
        #define SamplesTexture usampler2D(SamplesTexture, PointClampSampler)
        layout(std430, binding = 12) writeonly buffer VisibleLightsUB
        {
        	uvec4 visibleLights[]; // one Bloom filter per tile, row by row
        };

        LocalSize(8, 8, 1);

        void main()
        {
        	const ivec2 tile  = ivec2(gl_GlobalInvocationID.xy);
        	const ivec2 tiles = MegaLightsHashTiles();
        	if (tile.x >= tiles.x || tile.y >= tiles.y)
        		return;
        	const uint  samples = MegaLightsSamplesPerPixel();
        	const ivec2 first   = tile * MEGALIGHTS_HASH_TILE;
        	const ivec2 last    = min(first + MEGALIGHTS_HASH_TILE, MegaLightsSampleSize());
        	uvec4       hash    = uvec4(0u);
        	for (int y = first.y; y < last.y; ++y)
        		for (int x = first.x; x < last.x; ++x)
        			for (uint k = 0u; k < samples; ++k)
        			{
        				const MegaLightsSample s = MegaLightsUnpackSample(
        				     texelFetch(SamplesTexture, MegaLightsSampleTexel(ivec2(x, y), k), 0).rg);
        				if (s.Light >= 0 && s.Visibility > 0.0 && !MegaLightsLightIsEmissive(s.Light))
        					MegaLightsHashMark(hash, uint(s.Light));
        			}
        	visibleLights[tile.y * tiles.x + tile.x] = hash;
        }
    }
}
