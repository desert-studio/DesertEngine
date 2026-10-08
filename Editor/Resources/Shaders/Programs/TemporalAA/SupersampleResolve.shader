// DesertAsset {"Kind":"Shader","Guid":"38269115a0604291b7df59df9dd4b5c8","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SupersampleResolve"
{
    Compute
    {
        // TAA1-B. The SSAA downsample (ResolutionSplit::Mode == Supersample): one axis of a separable Catmull-Rom
        // (B = 0, C = 0.5) reconstruction, the kernel stretched by the source/destination ratio so every source
        // texel under the destination pixel's footprint contributes (a box aliases at a non-integer ratio). Run
        // twice by Graphic::SupersampleResolve: horizontal, then vertical. Negative lobes can ring below zero on
        // a hard HDR edge: the result is clamped at 0.

        LocalSize(8, 8, 1);

        Uniform(0) sampler2D u_Source;
        layout(binding = 1, rgba16f) restrict writeonly uniform image2D u_Destination;

        // The C++ side is Graphic::SupersampleResolvePush (View/TemporalAA.hpp).
        PushConstant PushConstants
        {
            int u_SourceWidth;
            int u_SourceHeight;
            int u_DestinationWidth;
            int u_DestinationHeight;
            int u_Axis; // 0 horizontal, 1 vertical
            int u_Pad0;
            int u_Pad1;
            int u_Pad2;
        };

        float CatmullRom(float x)
        {
            x = abs(x);
            if (x < 1.0)
                return (1.5 * x - 2.5) * x * x + 1.0;
            if (x < 2.0)
                return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
            return 0.0;
        }

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            if (pixel.x >= u_DestinationWidth || pixel.y >= u_DestinationHeight)
                return;

            const bool  horizontal  = u_Axis == 0;
            const int   sourceSize  = horizontal ? u_SourceWidth : u_SourceHeight;
            const int   destination = horizontal ? u_DestinationWidth : u_DestinationHeight;
            const float scale       = max(float(sourceSize) / float(destination), 1.0);
            const float coordinate  = float(horizontal ? pixel.x : pixel.y);
            const float center      = (coordinate + 0.5) * scale - 0.5;
            const int   first       = int(ceil(center - 2.0 * scale));
            const int   last        = int(floor(center + 2.0 * scale));

            vec3  sum    = vec3(0.0);
            float weight = 0.0;
            for (int i = first; i <= last; ++i)
            {
                const float w = CatmullRom((float(i) - center) / scale);
                const int   s = clamp(i, 0, sourceSize - 1);
                const ivec2 p = horizontal ? ivec2(s, pixel.y) : ivec2(pixel.x, s);
                sum += texelFetch(u_Source, p, 0).rgb * w;
                weight += w;
            }
            imageStore(u_Destination, pixel, vec4(max(sum / weight, vec3(0.0)), 1.0));
        }
    }
}
