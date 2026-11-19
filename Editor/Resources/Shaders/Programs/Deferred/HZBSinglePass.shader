// DesertAsset {"Kind":"Shader","Guid":"bf1b13f1594441e8bf9d768bd93ae4e5","Versions":{"SHDR":1},"Dependencies":[]}
Shader "HZBSinglePass"
{
    // THE CLOSEST- OR FURTHEST-DEPTH HIERARCHICAL Z-BUFFER, UP TO SIX MIPS PER DISPATCH (UE BuildHZB: several
    // mips of one dispatch through group-shared memory, after AMD FidelityFX SPD: a group reduces a 32 x 32 tile
    // of its first mip down to one texel). The same hierarchy as Programs/Deferred/HZB.shader, texel for texel:
    // every texel of every mip is the min (furthest) or max (closest, reversed-Z) of exactly the source texels
    // HZB.shader takes for it,
    //     [floor(d * src / dst), ceil((d + 1) * src / dst)) per axis,
    // with the same float expressions, and min / max are exact and order-free - so the result is bit-equal and
    // the uv -> texel mapping the tracer relies on (Common/HZBTrace.glslh) holds for any size. A 12-mip view
    // (up to 4095 texels a side) is two dispatches: mips 0-5, then mips 6-11 in a single group.
    //
    // A dispatch makes Params.y (1..6) CONSECUTIVE mips: DestinationMip0 is computed from SourceMipTexture (the
    // scene depth for mip 0, else the mip below as a single-mip view), each next one from the previous one. A
    // group OWNS one texel of the TOP mip of the dispatch and computes, bottom-up, every texel of the lower mips
    // that texel's footprint covers. Footprints of neighbouring groups overlap by a texel on odd sizes: both
    // groups compute and store that texel, the same value from the same inputs. The first mip is read back from
    // DestinationMip0 itself (coherent; the group's own stores, made visible by memoryBarrierImage + barrier),
    // mips 1..4 of the dispatch from group-shared memory; the top one is only stored.
    // Footprint bound: a range of n destination texels covers at most 2n + 1 source texels (src = 2 dst or
    // 2 dst + 1), plus one per end for the float rounding HZB.shader shares - 2n + 3: top 1 -> 5 -> 13 -> 29 -> 61.

    Compute
    {
        Uniform sampler PointClampSampler;
        // No binding numbers: the compiler numbers each resource (ShaderCompiler.cpp, auto-mapped bindings) and
        // the passes bind them by name (HZBRenderer's shader parameters).
        uniform texture2D SourceMipTexture; // mip 0: the scene depth; else this HZB's mip below (one mip)
        #define SourceMipTexture sampler2D(SourceMipTexture, PointClampSampler)
        // The dispatch's mips, lowest first. A slot past Params.y is bound to a scratch target and never written.
        // The first is read back by the group that wrote it (coherent, not writeonly).
        layout(r32f) coherent uniform image2D DestinationMip0;
        layout(r32f) writeonly uniform image2D DestinationMip1;
        layout(r32f) writeonly uniform image2D DestinationMip2;
        layout(r32f) writeonly uniform image2D DestinationMip3;
        layout(r32f) writeonly uniform image2D DestinationMip4;
        layout(r32f) writeonly uniform image2D DestinationMip5;

        PushConstant PushConstants
        {
            uvec4 Params; // x = 1: furthest (min), 0: closest (max); y = mips of this dispatch, 1..6
        };

        LocalSize(16, 16, 1);

        // Group-shared mips 1..4 of the dispatch (mip 0 is read back from its image, the top one is only stored),
        // at the 2n + 3 footprint bound of the header: 61 x 61, 29 x 29, 13 x 13, 5 x 5 (19 KB).
        const int kMaxMips = 6;
        const int kTile    = 1;
        const int kOffset2 = 61 * 61;
        const int kOffset3 = kOffset2 + 29 * 29;
        const int kOffset4 = kOffset3 + 13 * 13;
        shared float s_Depth[kOffset4 + 5 * 5];

        int SharedOffset(int level)
        {
            if (level <= 1)
                return 0;
            if (level == 2)
                return kOffset2;
            if (level == 3)
                return kOffset3;
            return kOffset4;
        }

        ivec2 LevelSize(int level)
        {
            if (level == 0)
                return imageSize(DestinationMip0);
            if (level == 1)
                return imageSize(DestinationMip1);
            if (level == 2)
                return imageSize(DestinationMip2);
            if (level == 3)
                return imageSize(DestinationMip3);
            if (level == 4)
                return imageSize(DestinationMip4);
            return imageSize(DestinationMip5);
        }

        void StoreLevel(int level, ivec2 d, float depth)
        {
            if (level == 0)
                imageStore(DestinationMip0, d, vec4(depth));
            else if (level == 1)
                imageStore(DestinationMip1, d, vec4(depth));
            else if (level == 2)
                imageStore(DestinationMip2, d, vec4(depth));
            else if (level == 3)
                imageStore(DestinationMip3, d, vec4(depth));
            else if (level == 4)
                imageStore(DestinationMip4, d, vec4(depth));
            else
                imageStore(DestinationMip5, d, vec4(depth));
        }

        void main()
        {
            const int  top      = int(clamp(Params.y, 1u, uint(kMaxMips))) - 1;
            const bool furthest = Params.x != 0u;
            const int  local    = int(gl_LocalInvocationIndex);
            const int  threads  = int(gl_WorkGroupSize.x * gl_WorkGroupSize.y);

            // The ranges [lo, hi) each mip of the dispatch computes, top-down: the union of the footprints of a
            // contiguous range is the footprint of its ends (floor and ceil are monotonic).
            ivec2 lo[kMaxMips];
            ivec2 hi[kMaxMips];
            lo[top] = ivec2(gl_WorkGroupID.xy) * kTile;
            hi[top] = min(lo[top] + kTile, LevelSize(top));
            for (int level = top; level > 0; --level)
            {
                const ivec2 dst   = LevelSize(level);
                const ivec2 src   = LevelSize(level - 1);
                const vec2  scale = vec2(src) / vec2(dst);
                lo[level - 1] = ivec2(floor(vec2(lo[level]) * scale));
                hi[level - 1] = min(ivec2(ceil(vec2(hi[level]) * scale)), src);
            }

            // Mip 0 of the dispatch from the source texture: HZB.shader's loop, texel for texel.
            {
                const ivec2 dst   = LevelSize(0);
                const ivec2 src   = textureSize(SourceMipTexture, 0);
                const vec2  scale = vec2(src) / vec2(dst);
                const ivec2 n     = max(hi[0] - lo[0], ivec2(0));
                for (int i = local; i < n.x * n.y; i += threads)
                {
                    const ivec2 d  = lo[0] + ivec2(i % n.x, i / n.x);
                    const ivec2 p0 = ivec2(floor(vec2(d) * scale));
                    const ivec2 p1 = min(ivec2(ceil(vec2(d + 1) * scale)), src);
                    float depth = furthest ? 1.0 : 0.0; // the near / far plane under reversed-Z
                    for (int y = p0.y; y < p1.y; ++y)
                        for (int x = p0.x; x < p1.x; ++x)
                        {
                            const float texel = texelFetch(SourceMipTexture, ivec2(x, y), 0).r;
                            depth = furthest ? min(depth, texel) : max(depth, texel);
                        }
                    StoreLevel(0, d, depth);
                }
            }

            // Each next mip from the one below it (the image for mip 0, else group-shared memory at its offset
            // from the range below): the same footprint. `top` comes from the push constant, so every invocation
            // of the group runs the same iterations and reaches each barrier.
            for (int level = 1; level <= top; ++level)
            {
                memoryBarrierImage();
                barrier();
                const ivec2 dst     = LevelSize(level);
                const ivec2 src     = LevelSize(level - 1);
                const vec2  scale   = vec2(src) / vec2(dst);
                const ivec2 below   = lo[level - 1];
                const int   stride  = max(hi[level - 1].x - below.x, 0);
                const int   inBase  = SharedOffset(level - 1);
                const int   outBase = SharedOffset(level);
                const ivec2 n       = max(hi[level] - lo[level], ivec2(0));
                for (int i = local; i < n.x * n.y; i += threads)
                {
                    const ivec2 d  = lo[level] + ivec2(i % n.x, i / n.x);
                    const ivec2 p0 = ivec2(floor(vec2(d) * scale));
                    const ivec2 p1 = min(ivec2(ceil(vec2(d + 1) * scale)), src);
                    float depth = furthest ? 1.0 : 0.0;
                    for (int y = p0.y; y < p1.y; ++y)
                        for (int x = p0.x; x < p1.x; ++x)
                        {
                            const float texel = level == 1
                                                    ? imageLoad(DestinationMip0, ivec2(x, y)).r
                                                    : s_Depth[inBase + (y - below.y) * stride + (x - below.x)];
                            depth = furthest ? min(depth, texel) : max(depth, texel);
                        }
                    if (level < top)
                        s_Depth[outBase + i] = depth;
                    StoreLevel(level, d, depth);
                }
            }
        }
    }
}
