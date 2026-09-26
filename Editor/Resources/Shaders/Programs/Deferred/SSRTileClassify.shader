// DesertAsset {"Kind":"Shader","Guid":"2c5f973ed43a471a9e9f333a45966a84","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSRTileClassify"
{
    // SSR tile classification: one output texel per screen tile (Common/SSRTiles.glslh), 1 where any pixel of
    // the tile passes the trace's G-buffer gate (Common/SSRGate.glslh). The trace, resolve and composite then
    // draw only the marked tiles (and their neighbours), so SSR's cost follows the reflecting share of the
    // screen instead of the whole frame.
    //
    // One workgroup per tile, one thread per pixel, the tile's verdict reduced in shared memory: a fragment
    // per tile looping over its 64 texels ran at a sliver of the GPU's occupancy and cost as much as the trace.

    Compute
    {
        #include <Common/SSRGate.glslh>

        Uniform(0) sampler2D u_GBufferNormal; // rgb = world normal, a = roughness
        layout(binding = 1, rgba8) writeonly uniform image2D u_TileMask;

        LocalSize(8, 8, 1);

        shared uint s_TileReflects;

        void main()
        {
        	if (gl_LocalInvocationIndex == 0u)
        		s_TileReflects = 0u;
        	barrier();

        	// The tiled passes stretch the tiles over the target (grid = ceil(size / 8)), so a tile spans
        	// [floor(t * scale), ceil((t + 1) * scale)) texels: up to 9, hence up to 2x2 texels per thread.
        	ivec2 full  = textureSize(u_GBufferNormal, 0);
        	ivec2 grid  = imageSize(u_TileMask);
        	ivec2 tile  = ivec2(gl_WorkGroupID.xy);
        	vec2  scale = vec2(full) / vec2(grid);
        	ivec2 p0    = ivec2(floor(vec2(tile) * scale));
        	ivec2 p1    = min(ivec2(ceil(vec2(tile + 1) * scale)), full);

        	bool reflects = false;
        	for (int oy = 0; oy < 2; oy++)
        		for (int ox = 0; ox < 2; ox++)
        		{
        			ivec2 p = p0 + ivec2(gl_LocalInvocationID.xy) + ivec2(ox, oy) * 8;
        			if (p.x < p1.x && p.y < p1.y && SSRPixelCanReflect(texelFetch(u_GBufferNormal, p, 0)))
        				reflects = true;
        		}
        	if (reflects)
        		atomicOr(s_TileReflects, 1u);
        	barrier();

        	if (gl_LocalInvocationIndex == 0u)
        		imageStore(u_TileMask, tile, vec4(float(s_TileReflects)));
        }
    }
}
