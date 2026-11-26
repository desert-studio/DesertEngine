// DesertAsset {"Kind":"Shader","Guid":"488bdd88df4d4a66a13904368c86fbaa","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MotionBlurNeighborMax"
{
    Compute
    {
        // MR2 (View/MotionBlur.hpp). Pass 3 of 4, one invocation per tile: the longest tile max of the 3x3
        // neighbourhood (edges clamped), so a moving silhouette reaches the still pixels beside it - the soft edge.
        // CPU twin: Graphic::NeighborhoodMaxVelocity. z: the shortest tile-min length of the same 3x3 (the
        // neighbourhood's slowest pixel), which Gather compares with the longest to pick its fast path.
        LocalSize(8, 8, 1);

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(3) sampler PointClampSampler;
        Uniform(0) texture2D TileMaxTexture; // tile extent
        #define TileMaxTexture sampler2D(TileMaxTexture, PointClampSampler)
        layout(binding = 1, rgba16f) restrict writeonly uniform image2D NeighborMaxTexture;

        // The C++ side is Graphic::MotionBlurParams (View/MotionBlur.hpp), offsets pinned by static_asserts.
        #include <Generated/Post/MotionBlurParams.glslh>
        layout(std430, binding = 2) readonly buffer MotionBlurBuffer { DESERT_MotionBlurParams_MEMBERS };

        void main()
        {
            const ivec2 tile  = ivec2(gl_GlobalInvocationID.xy);
            const ivec2 count = ivec2(TileCount);
            if (any(greaterThanEqual(tile, count)))
                return;
            vec2  longest = vec2(0.0);
            float best    = 0.0;
            float least   = 3.0e38;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const ivec2 at       = clamp(tile + ivec2(dx, dy), ivec2(0), count - 1);
                    const vec3  tileMax  = texelFetch(TileMaxTexture, at, 0).xyz;
                    const vec2  v        = tileMax.xy;
                    const float lengthSq = dot(v, v);
                    least                = min(least, tileMax.z);
                    if (lengthSq > best)
                    {
                        best    = lengthSq;
                        longest = v;
                    }
                }
            imageStore(NeighborMaxTexture, tile, vec4(longest, least, 0.0));
        }
    }
}
