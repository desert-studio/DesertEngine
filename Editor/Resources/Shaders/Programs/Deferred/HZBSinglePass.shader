// DesertAsset {"Kind":"Shader","Guid":"bf1b13f1594441e8bf9d768bd93ae4e5","Versions":{"SHDR":1},"Dependencies":[]}
Shader "HZBSinglePass"
{
    // THE CLOSEST- OR FURTHEST-DEPTH HIERARCHICAL Z-BUFFER, UP TO FOUR MIPS PER DISPATCH (UE BuildHZB: several
    // mips of one dispatch through group-shared memory, after AMD FidelityFX SPD). The same hierarchy as
    // Programs/Deferred/HZB.shader, texel for texel: every texel of every mip is the min (furthest) or max
    // (closest, reversed-Z) of exactly the source texels HZB.shader takes for it,
    //     [floor(d * src / dst), ceil((d + 1) * src / dst)) per axis,
    // with the same float expressions, and min / max are exact and order-free - so the result is bit-equal and
    // the uv -> texel mapping the tracer relies on (Common/HZBTrace.glslh) holds for any size.
    //
    // A dispatch makes Params.y (1..4) CONSECUTIVE mips: DestinationMip0 is computed from SourceMipTexture (the
    // scene depth for mip 0, else the mip below as a single-mip view), each next one from the previous one kept
    // in group-shared memory. A group OWNS a 4x4 tile of the TOP mip of the dispatch and computes, bottom-up,
    // every texel of the lower mips that tile's footprint covers. Footprints of neighbouring tiles overlap by a
    // texel on odd sizes: both groups compute and store that texel, the same value from the same inputs.
    // Footprint bound: a range of n destination texels covers at most 2n + 1 source texels (src = 2 dst or
    // 2 dst + 1), plus one per end for the float rounding HZB.shader shares - 2n + 3: top 4 -> 11 -> 25 -> 53.

    Compute
    {
        Uniform sampler PointClampSampler;
        // No binding numbers: the compiler numbers each resource (ShaderCompiler.cpp, auto-mapped bindings) and
        // the passes bind them by name (HZBRenderer's shader parameters).
        uniform texture2D SourceMipTexture; // mip 0: the scene depth; else this HZB's mip below (one mip)
        #define SourceMipTexture sampler2D(SourceMipTexture, PointClampSampler)
        // The dispatch's mips, lowest first. A slot past Params.y is bound to a scratch target and never written.
        layout(r32f) writeonly uniform image2D DestinationMip0;
        layout(r32f) writeonly uniform image2D DestinationMip1;
        layout(r32f) writeonly uniform image2D DestinationMip2;
        layout(r32f) writeonly uniform image2D DestinationMip3;

        PushConstant PushConstants
        {
            uvec4 Params; // x = 1: furthest (min), 0: closest (max); y = mips of this dispatch, 1..4
        };

        LocalSize(16, 16, 1);

        // Group-shared mips 0..2 of the dispatch (the top one is only stored), at the 2n + 3 footprint bound of
        // the header: 53 x 53, 25 x 25, 11 x 11.
        const int kTile = 4;
        const int kOffset1 = 53 * 53;
        const int kOffset2 = kOffset1 + 25 * 25;
        shared float s_Depth[kOffset2 + 11 * 11];

        int SharedOffset(int level)
        {
            if (level == 0)
                return 0;
            if (level == 1)
                return kOffset1;
            return kOffset2;
        }

        ivec2 LevelSize(int level)
        {
            if (level == 0)
                return imageSize(DestinationMip0);
            if (level == 1)
                return imageSize(DestinationMip1);
            if (level == 2)
                return imageSize(DestinationMip2);
            return imageSize(DestinationMip3);
        }

        void StoreLevel(int level, ivec2 d, float depth)
        {
            if (level == 0)
                imageStore(DestinationMip0, d, vec4(depth));
            else if (level == 1)
                imageStore(DestinationMip1, d, vec4(depth));
            else if (level == 2)
                imageStore(DestinationMip2, d, vec4(depth));
            else
                imageStore(DestinationMip3, d, vec4(depth));
        }

        void main()
        {
            const int  top      = int(clamp(Params.y, 1u, 4u)) - 1;
            const bool furthest = Params.x != 0u;
            const int  local    = int(gl_LocalInvocationIndex);
            const int  threads  = int(gl_WorkGroupSize.x * gl_WorkGroupSize.y);

            // The ranges [lo, hi) each mip of the dispatch computes, top-down: the union of the footprints of a
            // contiguous range is the footprint of its ends (floor and ceil are monotonic).
            ivec2 lo[4];
            ivec2 hi[4];
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
                    if (top > 0)
                        s_Depth[i] = depth;
                    StoreLevel(0, d, depth);
                }
            }

            // Each next mip from the one below it in group-shared memory: the same footprint, read at its offset
            // from the range below.
            for (int level = 1; level <= top; ++level)
            {
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
                    const ivec2 p0 = ivec2(floor(vec2(d) * scale)) - below;
                    const ivec2 p1 = min(ivec2(ceil(vec2(d + 1) * scale)), src) - below;
                    float depth = furthest ? 1.0 : 0.0;
                    for (int y = p0.y; y < p1.y; ++y)
                        for (int x = p0.x; x < p1.x; ++x)
                        {
                            const float texel = s_Depth[inBase + y * stride + x];
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
