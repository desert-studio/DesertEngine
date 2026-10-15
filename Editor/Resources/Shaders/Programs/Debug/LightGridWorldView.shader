// DesertAsset {"Kind":"Shader","Guid":"21f5d2879a1e4e0bad53004ff8030bdf","Versions":{"SHDR":1},"Dependencies":[]}
Shader "LightGridWorldView"
{
    // The "Light Grid (World)" view mode (DeferredDebugMode::LightGridWorld): for the surface under each pixel, the
    // number of point and spot lights its cell of the WORLD light grid lists (Mesh/LightGrid.glslh, the radiance
    // cache's clipmap cells — what a GI ray hitting that surface would be lit by), heat-mapped like Light
    // Complexity (8 lights = full red). Cell edges are drawn darker, so the clipmap's cells (finer near the camera)
    // read as a lattice on the geometry; magenta = a cell lit by every light (the pool ran out); black = no
    // geometry or past the coarsest clipmap. Drawn over the post input by "Debug: LightGridWorld".

    Vertex
    {
        #include <Common/FullscreenTriangle.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
        	v_TexCoord  = FullscreenTriangleUV();
        	gl_Position = vec4(FullscreenTriangleNdc(), 0.0, 1.0);
        }
    }

    Fragment
    {
        #define LIGHT_GRID_WORLD_CELLS lightGridWorldCells
        #include <Mesh/LightGrid.glslh>
        #include <Common/ReconstructPosition.glslh>

        In(0) vec2 v_TexCoord;

        Uniform(0) sampler2D u_Depth; // the G-buffer's device depth (render extent)
        ReadBuffer(1) LightGridWorldViewUB
        {
        	mat4 u_InvJitteredViewProjection; // the inverse of the matrix the depth was rasterised with
        };
        ReadBuffer(2) LightGridParamsUB
        {
        	LightGridParams lightGrid;
        };
        ReadBuffer(3) LightGridWorldCellsUB
        {
        	uint lightGridWorldCells[];
        };

        Out(0) vec4 oColor;

        vec3 HeatColor(float t)
        {
        	t = clamp(t, 0.0, 1.0);
        	return clamp(vec3(1.5 - abs(4.0 * t - 3.0),
        	                  1.5 - abs(4.0 * t - 2.0),
        	                  1.5 - abs(4.0 * t - 1.0)), 0.0, 1.0);
        }

        void main()
        {
        	const ivec2 size  = textureSize(u_Depth, 0);
        	const ivec2 texel = clamp(ivec2(v_TexCoord * vec2(size)), ivec2(0), size - 1);
        	const float depth = texelFetch(u_Depth, texel, 0).r;
        	const vec3  p     = ReconstructWorldPosition(v_TexCoord, depth, u_InvJitteredViewProjection);

        	const uint cell = LightGridWorldCellAt(lightGrid, p);
        	if (cell == LIGHT_GRID_NO_CELL)
        	{
        		oColor = vec4(0.0, 0.0, 0.0, 1.0);
        		return;
        	}
        	const LightGridWorldRange range = LightGridWorldLightsAt(lightGrid, p);
        	vec3 colour = range.everyLight ? vec3(1.0, 0.0, 1.0) : HeatColor(float(range.count) / 8.0);
        	// Cell edges: the distance to the nearest face of the cell, in cells of the clipmap the point fell in.
        	for (uint i = 0u; i < lightGrid.counts.w; i++)
        	{
        		const vec3 local = (p - lightGrid.clipmaps[i].xyz) / lightGrid.clipmaps[i].w;
        		if (all(greaterThanEqual(local, vec3(0.0))) && all(lessThan(local, vec3(float(lightGrid.size.x)))))
        		{
        			const vec3  f    = fract(local);
        			const vec3  edge = min(f, 1.0 - f);
        			const float near = min(min(edge.x, edge.y), edge.z);
        			colour *= mix(0.35, 1.0, smoothstep(0.0, 0.04, near));
        			break;
        		}
        	}
        	oColor = vec4(colour, 1.0);
        }
    }
}
