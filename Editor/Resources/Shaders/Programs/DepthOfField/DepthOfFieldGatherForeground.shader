// DesertAsset {"Kind":"Shader","Guid":"fc46fe46bd354cfbad63b00422eb2fcf","Versions":{"SHDR":1},"Dependencies":[]}
Shader "DepthOfFieldGatherForeground"
{
    Compute
    {
        // MR3 (View/DepthOfField.hpp). Pass 4 of 6, one invocation per HALF-res pixel:
        // the foreground layer (discs in front of the focus plane), colour and opacity.
        // Scatter-as-gather over rings (UE Diaphragm DOF): ring k of u_Rings holds 8k samples at radius k / u_Rings
        // of the tile's dilated kernel, plus the centre; a sample counts where its own disc covers this pixel, its
        // weight is the coverage over the disc's area (a large disc spreads its colour thinner).
        LocalSize(8, 8, 1);

        Uniform(0) sampler2D u_Half;    // rgb colour, a signed CoC (half-res pixels); HalfExtent
        Uniform(1) sampler2D u_Dilated; // x foreground radius, y background radius; tile extent
        layout(binding = 2, rgba16f) restrict writeonly uniform image2D u_Layer;

        // The C++ side is Graphic::DofParams (View/DepthOfField.hpp), offsets pinned by static_asserts.
        layout(std430, binding = 3) readonly buffer DepthOfFieldBuffer
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
        const bool kForeground = true;

        void main()
        {
            const ivec2 pixel    = ivec2(gl_GlobalInvocationID.xy);
            const ivec2 halfSize = ivec2(u_HalfSize);
            if (any(greaterThanEqual(pixel, halfSize)))
                return;
            const vec4  centre     = texelFetch(u_Half, pixel, 0);
            const vec2  tileRadius = texelFetch(u_Dilated, pixel / kTileSize, 0).xy;
            const float kernelRadius = kForeground ? tileRadius.x : tileRadius.y;
            if (kernelRadius < 0.5)
            {
                imageStore(u_Layer, pixel, vec4(centre.rgb, 0.0));
                return;
            }

            // Interleaved gradient noise rotates the rings per pixel (no banding between the rings' samples).
            const float rotation =
                 6.2831853 * fract(52.9829189 * fract(dot(vec2(pixel), vec2(0.06711056, 0.00583715))));
            const int rings   = max(u_Rings, 1);
            const int samples = 1 + 4 * rings * (rings + 1); // Graphic::DofSampleCount

            vec3  sum    = vec3(0.0);
            float weight = 0.0;
            for (int k = 0; k <= rings; ++k)
            {
                const int   count  = k == 0 ? 1 : 8 * k;
                const float radius = kernelRadius * float(k) / float(rings);
                for (int j = 0; j < count; ++j)
                {
                    const float angle  = rotation + 6.2831853 * (float(j) + 0.5 * float(k & 1)) / float(count);
                    const vec2  offset = radius * vec2(cos(angle), sin(angle));
                    const ivec2 tap    = clamp(pixel + ivec2(round(offset)), ivec2(0), halfSize - 1);
                    const vec4  s      = texelFetch(u_Half, tap, 0);
                    const float coc    = kForeground ? -s.a : s.a; // this layer's discs only
                    if (coc <= 0.0)
                        continue;
                    const float coverage = clamp(coc - radius + 0.5, 0.0, 1.0);
                    const float w        = coverage / max(coc * coc, 0.25);
                    sum += s.rgb * w;
                    weight += w;
                }
            }
            if (weight <= 0.0)
            {
                imageStore(u_Layer, pixel, vec4(centre.rgb, 0.0));
                return;
            }
            // Opacity: each sample stands for kernelRadius^2 / samples of the kernel area (pi cancels), spread over
            // its own disc of coc^2; a pixel inside a blurred foreground object is fully covered (1).
            const float opacity = clamp(weight * kernelRadius * kernelRadius / float(samples), 0.0, 1.0);
            imageStore(u_Layer, pixel, vec4(sum / weight, opacity));
        }
    }
}
