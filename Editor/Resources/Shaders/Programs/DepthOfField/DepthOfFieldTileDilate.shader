// DesertAsset {"Kind":"Shader","Guid":"985856fb30db42128fe508e194ac331d","Versions":{"SHDR":1},"Dependencies":[]}
Shader "DepthOfFieldTileDilate"
{
    Compute
    {
        // MR3 (View/DepthOfField.hpp). Pass 3 of 6, one invocation per tile: the largest foreground / background
        // radius of the tiles within u_DilateTiles (the largest bokeh, Graphic::DofDilateTiles) whose disc can
        // reach this tile, so a pixel's gather kernel covers every disc that lands on it.
        LocalSize(8, 8, 1);

        Uniform(0) sampler2D u_Tiles; // x foreground radius, y background radius; tile extent
        layout(binding = 1, rg16f) restrict writeonly uniform image2D u_Dilated;

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
            const ivec2 tile  = ivec2(gl_GlobalInvocationID.xy);
            const ivec2 count = ivec2(u_TileCount);
            if (any(greaterThanEqual(tile, count)))
                return;
            vec2 radius = texelFetch(u_Tiles, tile, 0).xy;
            for (int dy = -u_DilateTiles; dy <= u_DilateTiles; ++dy)
                for (int dx = -u_DilateTiles; dx <= u_DilateTiles; ++dx)
                {
                    const ivec2 other = clamp(tile + ivec2(dx, dy), ivec2(0), count - 1);
                    // The gap between the two tiles' nearest pixels, in half-res pixels.
                    const float gap       = float(max(max(abs(dx), abs(dy)) - 1, 0) * kTileSize);
                    const vec2  neighbour = texelFetch(u_Tiles, other, 0).xy;
                    radius = max(radius, mix(vec2(0.0), neighbour, greaterThan(neighbour, vec2(gap))));
                }
            imageStore(u_Dilated, tile, vec4(radius, 0.0, 0.0));
        }
    }
}
