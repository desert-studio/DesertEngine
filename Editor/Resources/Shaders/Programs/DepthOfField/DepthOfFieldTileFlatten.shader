// DesertAsset {"Kind":"Shader","Guid":"f7f03d291b88433282e4a0201475907f","Versions":{"SHDR":1},"Dependencies":[]}
Shader "DepthOfFieldTileFlatten"
{
    Compute
    {
        // MR3 (View/DepthOfField.hpp). Pass 2 of 6, one invocation per tile of kTileSize^2 half-res pixels: the
        // largest foreground radius (x, from the most negative CoC) and the largest background radius (y).
        LocalSize(8, 8, 1);

        Uniform(0) sampler2D u_Half; // rgb colour, a signed CoC (half-res pixels); HalfExtent
        layout(binding = 1, rg16f) restrict writeonly uniform image2D u_Tiles;

        // The C++ side is Graphic::DofParams (View/DepthOfField.hpp), offsets pinned by static_asserts.
        layout(std430, binding = 2) readonly buffer DepthOfFieldBuffer
        {
            vec2  u_OutputSize;
            vec2  u_RenderSize;
            vec2  u_HalfSize;
            vec2  u_TileCount;
            vec4  u_DepthToView;
            float u_FocalDistanceMm;
            float u_FocalLengthMm;
            float u_CocScale;
            float u_MaxRadius;
            int   u_Rings;
            int   u_DilateTiles;
            float u_Pad0;
            float u_Pad1;
        };

        const int kTileSize = 8; // Graphic::kDofTileSize

        void main()
        {
            const ivec2 tile = ivec2(gl_GlobalInvocationID.xy);
            if (any(greaterThanEqual(tile, ivec2(u_TileCount))))
                return;
            const ivec2 halfSize   = ivec2(u_HalfSize);
            float       foreground = 0.0;
            float       background = 0.0;
            for (int y = 0; y < kTileSize; ++y)
                for (int x = 0; x < kTileSize; ++x)
                {
                    const ivec2 pixel = tile * kTileSize + ivec2(x, y);
                    if (any(greaterThanEqual(pixel, halfSize)))
                        continue;
                    const float coc = texelFetch(u_Half, pixel, 0).a;
                    foreground      = max(foreground, -coc);
                    background      = max(background, coc);
                }
            imageStore(u_Tiles, tile, vec4(foreground, background, 0.0, 0.0));
        }
    }
}
