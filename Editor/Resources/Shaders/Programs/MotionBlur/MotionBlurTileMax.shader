// DesertAsset {"Kind":"Shader","Guid":"a8d9b5178938429b9e91a76db09039a0","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MotionBlurTileMax"
{
    Compute
    {
        // MR2 (View/MotionBlur.hpp). Pass 2 of 4, one invocation per 16x16 render tile (kMotionBlurTileSize): the
        // longest flattened velocity of the tile (UE's velocity tile max). CPU twin: Graphic::LongestVelocity.
        LocalSize(8, 8, 1);

        const int kTileSize = 16; // Graphic::kMotionBlurTileSize

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(3) sampler PointClampSampler;
        Uniform(0) texture2D FlattenedVelocityTexture; // RenderExtent
        #define FlattenedVelocityTexture sampler2D(FlattenedVelocityTexture, PointClampSampler)
        layout(binding = 1, rg16f) restrict writeonly uniform image2D TileMaxTexture;

        // The C++ side is Graphic::MotionBlurParams (View/MotionBlur.hpp), offsets pinned by static_asserts.
        layout(std430, binding = 2) readonly buffer MotionBlurBuffer
        {
            vec2  RenderSize;
            vec2  OutputSize;
            vec2  TileCount;
            float VelocityScale;
            float MaxPixels;
            int   Samples;
            float Pad0;
            float Pad1;
            float Pad2;
        };

        void main()
        {
            const ivec2 tile = ivec2(gl_GlobalInvocationID.xy);
            if (any(greaterThanEqual(tile, ivec2(TileCount))))
                return;
            const ivec2 size    = ivec2(RenderSize);
            const ivec2 origin  = tile * kTileSize;
            vec2        longest = vec2(0.0);
            float       best    = 0.0;
            for (int y = 0; y < kTileSize; ++y)
                for (int x = 0; x < kTileSize; ++x)
                {
                    const ivec2 pixel = origin + ivec2(x, y);
                    if (pixel.x >= size.x || pixel.y >= size.y)
                        continue;
                    const vec2  v        = texelFetch(FlattenedVelocityTexture, pixel, 0).xy;
                    const float lengthSq = dot(v, v);
                    if (lengthSq > best)
                    {
                        best    = lengthSq;
                        longest = v;
                    }
                }
            imageStore(TileMaxTexture, tile, vec4(longest, 0.0, 0.0));
        }
    }
}
