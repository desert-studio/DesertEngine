// DesertAsset {"Kind":"Shader","Guid":"488bdd88df4d4a66a13904368c86fbaa","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MotionBlurNeighborMax"
{
    Compute
    {
        // MR2 (View/MotionBlur.hpp). Pass 3 of 4, one invocation per tile: the longest tile max of the 3x3
        // neighbourhood (edges clamped), so a moving silhouette reaches the still pixels beside it - the soft edge.
        // CPU twin: Graphic::NeighborhoodMaxVelocity.
        LocalSize(8, 8, 1);

        Uniform(0) sampler2D u_TileMax; // tile extent
        layout(binding = 1, rg16f) restrict writeonly uniform image2D u_NeighborMax;

        // The C++ side is Graphic::MotionBlurParams (View/MotionBlur.hpp), offsets pinned by static_asserts.
        layout(std430, binding = 2) readonly buffer MotionBlurBuffer
        {
            vec2  u_RenderSize;
            vec2  u_OutputSize;
            vec2  u_TileCount;
            float u_VelocityScale;
            float u_MaxPixels;
            int   u_Samples;
            float u_Pad0;
            float u_Pad1;
            float u_Pad2;
        };

        void main()
        {
            const ivec2 tile  = ivec2(gl_GlobalInvocationID.xy);
            const ivec2 count = ivec2(u_TileCount);
            if (any(greaterThanEqual(tile, count)))
                return;
            vec2  longest = vec2(0.0);
            float best    = 0.0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const ivec2 at       = clamp(tile + ivec2(dx, dy), ivec2(0), count - 1);
                    const vec2  v        = texelFetch(u_TileMax, at, 0).xy;
                    const float lengthSq = dot(v, v);
                    if (lengthSq > best)
                    {
                        best    = lengthSq;
                        longest = v;
                    }
                }
            imageStore(u_NeighborMax, tile, vec4(longest, 0.0, 0.0));
        }
    }
}
