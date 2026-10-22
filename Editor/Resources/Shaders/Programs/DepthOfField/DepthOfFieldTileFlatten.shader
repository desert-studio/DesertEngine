// DesertAsset {"Kind":"Shader","Guid":"f7f03d291b88433282e4a0201475907f","Versions":{"SHDR":1},"Dependencies":[]}
Shader "DepthOfFieldTileFlatten"
{
    Compute
    {
        // MR3 (View/DepthOfField.hpp). Pass 2 of 6, one invocation per tile of kTileSize^2 half-res pixels: the
        // largest foreground radius (x, from the most negative CoC) and the largest background radius (y).
        LocalSize(8, 8, 1);

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(3) sampler PointClampSampler;
        Uniform(0) texture2D HalfResolutionColorCocTexture; // rgb colour, a signed CoC (half-res pixels); HalfExtent
        #define HalfResolutionColorCocTexture sampler2D(HalfResolutionColorCocTexture, PointClampSampler)
        layout(binding = 1, rg16f) restrict writeonly uniform image2D TilesTexture;

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
            const ivec2 tile = ivec2(gl_GlobalInvocationID.xy);
            if (any(greaterThanEqual(tile, ivec2(TileCount))))
                return;
            const ivec2 halfSize   = ivec2(HalfSize);
            float       foreground = 0.0;
            float       background = 0.0;
            for (int y = 0; y < kTileSize; ++y)
                for (int x = 0; x < kTileSize; ++x)
                {
                    const ivec2 pixel = tile * kTileSize + ivec2(x, y);
                    if (any(greaterThanEqual(pixel, halfSize)))
                        continue;
                    const float coc = texelFetch(HalfResolutionColorCocTexture, pixel, 0).a;
                    foreground      = max(foreground, -coc);
                    background      = max(background, coc);
                }
            imageStore(TilesTexture, tile, vec4(foreground, background, 0.0, 0.0));
        }
    }
}
