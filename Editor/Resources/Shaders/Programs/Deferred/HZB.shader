// DesertAsset {"Kind":"Shader","Guid":"0ef415bf01f649cda94365ad231455f0","Versions":{"SHDR":1},"Dependencies":[]}
Shader "HZB"
{
    // THE CLOSEST-DEPTH HIERARCHICAL Z-BUFFER (UE: HZB.usf, the ClosestHZB; FidelityFX SSSR's depth hierarchy),
    // one mip per dispatch. Mip 0 is the scene depth at full resolution, every mip above it the CLOSEST device depth
    // of the texels of the mip below it covers. The scene is reversed-Z (Core/Projection.hpp: 1 at the near plane),
    // so "closest" is the MAXIMUM. A ray that is in front of a texel's value is in front of everything that texel
    // covers - the screen tracer (Common/HZBTrace.glslh) skips it whole.
    //
    // Any size, not only powers of two: a destination texel d covers the source texels
    // [floor(d * src / dst), ceil((d + 1) * src / dst)) - 2 per axis on an even edge, 3 where the source size is
    // odd, 1 for mip 0 (src == dst) - so every source texel is inside some destination texel and the uv -> texel
    // mapping of every mip is uv * mipSize. The tracer relies on that mapping.

    Compute
    {
        // Shared sampler states (UE shared samplers): each input below is a separate texture read through one of
        // these, bound once by the pass by name and numbered by the program build (SharedSamplerBinding.hpp).
        Uniform sampler u_PointClampSampler;
        Uniform(0) texture2D u_Source; // mip 0: the scene depth; mip n: this HZB's mip n - 1 (a single-mip view)
        #define u_Source sampler2D(u_Source, u_PointClampSampler)
        layout(binding = 1, r32f) writeonly uniform image2D u_Output;

        LocalSize(8, 8, 1);
        void main()
        {
        	const ivec2 dst = imageSize(u_Output);
        	const ivec2 d   = ivec2(gl_GlobalInvocationID.xy);
        	if (d.x >= dst.x || d.y >= dst.y)
        		return;
        	const ivec2 src   = textureSize(u_Source, 0);
        	const vec2  scale = vec2(src) / vec2(dst);
        	const ivec2 p0    = ivec2(floor(vec2(d) * scale));
        	const ivec2 p1    = min(ivec2(ceil(vec2(d + 1) * scale)), src);
        	float closest = 0.0; // the far plane under reversed-Z
        	for (int y = p0.y; y < p1.y; ++y)
        		for (int x = p0.x; x < p1.x; ++x)
        			closest = max(closest, texelFetch(u_Source, ivec2(x, y), 0).r);
        	imageStore(u_Output, d, vec4(closest));
        }
    }
}
