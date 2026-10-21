// DesertAsset {"Kind":"Shader","Guid":"985856fb30db42128fe508e194ac331d","Versions":{"SHDR":1},"Dependencies":[]}
Shader "DepthOfFieldTileDilate"
{
    Compute
    {
        // MR3 (View/DepthOfField.hpp). Pass 3 of 6, one invocation per tile: the largest foreground / background
        // radius of the tiles within DilateTiles (the largest bokeh, Graphic::DofDilateTiles) whose disc can
        // reach this tile, so a pixel's gather kernel covers every disc that lands on it.
        LocalSize(8, 8, 1);

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(3) sampler PointClampSampler;
        Uniform(0) texture2D TilesTexture; // x foreground radius, y background radius; tile extent
        #define TilesTexture sampler2D(TilesTexture, PointClampSampler)
        layout(binding = 1, rg16f) restrict writeonly uniform image2D DilatedTexture;

        // The C++ side is Graphic::DofParams (View/DepthOfField.hpp), offsets pinned by static_asserts.
        layout(std430, binding = 2) readonly buffer DepthOfFieldBuffer
        {
            vec2  OutputSize;
            vec2  RenderSize;
            vec2  HalfSize;
            vec2  TileCount;
            vec4  DepthToView;
            float FocalDistanceMm;
            float FocalLengthMm;
            float CocScale;
            float MaxRadius;
            int   Rings;
            int   DilateTiles;
            float Pad0;
            float Pad1;
        };

        const int kTileSize = 8; // Graphic::kDofTileSize

        void main()
        {
            const ivec2 tile  = ivec2(gl_GlobalInvocationID.xy);
            const ivec2 count = ivec2(TileCount);
            if (any(greaterThanEqual(tile, count)))
                return;
            vec2 radius = texelFetch(TilesTexture, tile, 0).xy;
            for (int dy = -DilateTiles; dy <= DilateTiles; ++dy)
                for (int dx = -DilateTiles; dx <= DilateTiles; ++dx)
                {
                    const ivec2 other = clamp(tile + ivec2(dx, dy), ivec2(0), count - 1);
                    // The gap between the two tiles' nearest pixels, in half-res pixels.
                    const float gap       = float(max(max(abs(dx), abs(dy)) - 1, 0) * kTileSize);
                    const vec2  neighbour = texelFetch(TilesTexture, other, 0).xy;
                    radius = max(radius, mix(vec2(0.0), neighbour, greaterThan(neighbour, vec2(gap))));
                }
            imageStore(DilatedTexture, tile, vec4(radius, 0.0, 0.0));
        }
    }
}
