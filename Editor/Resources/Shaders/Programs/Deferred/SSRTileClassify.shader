// DesertAsset {"Kind":"Shader","Guid":"2c5f973ed43a471a9e9f333a45966a84","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSRTileClassify"
{
    // SSR tile classification: one output texel per screen tile (Common/SSRTiles.glslh), 1 where any pixel of
    // the tile passes the trace's G-buffer gate (Common/SSRGate.glslh). The trace, resolve and composite then
    // draw only the marked tiles (and their neighbours), so SSR's cost follows the reflecting share of the
    // screen instead of the whole frame.

    Vertex
    {
        #include <Common/QuadPositions.glslh>
        #include <Common/QuadTextureCoords.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
        	v_TexCoord  = QUAD_TEXTURE_COORDINATES[gl_VertexIndex];
        	gl_Position = vec4(QUAD_POSITIONS[gl_VertexIndex], 0.0, 1.0);
        }
    }

    Fragment
    {
        #include <Common/SSRGate.glslh>

        In(0) vec2 v_TexCoord;

        Uniform(1) sampler2D u_GBufferNormal; // rgb = world normal, a = roughness

        Out(0) vec4 oColor;

        Uniform(0) SSRTileClassifyUB
        {
        	vec4 u_Grid; // xy = tile grid size, zw unused
        };

        void main()
        {
        	// Texture coordinates, not gl_FragCoord: the tiled passes place tiles by uv (SSRTiles.glslh).
        	vec2  grid  = u_Grid.xy;
        	ivec2 full  = textureSize(u_GBufferNormal, 0);
        	ivec2 tile  = ivec2(v_TexCoord * grid);
        	vec2  scale = vec2(full) / grid; // texels per (stretched) tile

        	// Every texel the tile's quad can cover: floor of the left edge to ceil of the right edge.
        	ivec2 p0 = ivec2(floor(vec2(tile) * scale));
        	ivec2 p1 = min(ivec2(ceil(vec2(tile + 1) * scale)), full);
        	for (int y = p0.y; y < p1.y; y++)
        		for (int x = p0.x; x < p1.x; x++)
        			if (SSRPixelCanReflect(texelFetch(u_GBufferNormal, ivec2(x, y), 0)))
        			{
        				oColor = vec4(1.0);
        				return;
        			}
        	oColor = vec4(0.0);
        }
    }
}
