// DesertAsset {"Kind":"Shader","Guid":"6dad0189e4dc4a0e966fd55ef41fdada","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TSRDilateVelocity"
{
    Compute
    {
        // TSR.DilateVelocity. Ported from UE 5.8 Engine/Shaders/Private/TemporalSuperResolution/TSRDilateVelocity.usf
        // (the closest depth of the 3x3 picks the velocity) and TSRClosestOccluder.ush ScatterClosestOccluder :101
        // (the closest depth scattered to where it was last frame, InterlockedMax), adapted: one pixel per lane, no
        // motion-blur directions output, no velocity-edge (decimate) mask, the scatter is atomicMax on a storage
        // buffer over the previous position's bilinear 2x2 footprint. A pixel nothing wrote velocity for (depth 0:
        // the reversed-Z far plane, the sky) is reprojected through the camera, as TemporalAA.shader does.
        LocalSize(8, 8, 1);

        #include <Common/TSRCommon.glslh>

        Uniform(1) sampler2D u_SceneDepth; // reversed-Z device depth, RenderExtent
        Uniform(2) sampler2D u_Velocity;   // NDC current - previous, RenderExtent
        layout(binding = 3, rgba16f) restrict writeonly uniform image2D u_DilatedVelocity; // xy: NDC velocity
        layout(binding = 4, r32f) restrict writeonly uniform image2D u_ClosestDepth;       // device depth
        layout(std430, binding = 5) restrict buffer TSRPrevAtomicBuffer
        {
            uint u_PrevAtomic[];
        };

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            if (any(greaterThanEqual(pixel, ivec2(u_RenderSize))))
                return;

            float closest      = -1.0;
            ivec2 closestPixel = pixel;
            for (int y = -1; y <= 1; ++y)
            {
                for (int x = -1; x <= 1; ++x)
                {
                    const ivec2 q     = TsrClampRender(pixel + ivec2(x, y));
                    const float depth = texelFetch(u_SceneDepth, q, 0).r;
                    if (depth > closest) // reversed-Z: larger is nearer
                    {
                        closest      = depth;
                        closestPixel = q;
                    }
                }
            }

            // The closest surface at THIS pixel, in homogeneous world space, never divided: the sky at depth 0 is a
            // point at infinity, and its reprojection is still that of its direction.
            const vec2 uv       = TsrSurfaceUv(pixel);
            const vec4 world    = u_InvViewProjection * vec4(ScreenUVToNdc(uv), closest, 1.0);
            const vec4 prevClip = u_PrevViewProjection * world;

            vec2 velocity = texelFetch(u_Velocity, closestPixel, 0).xy;
            if (closest <= 0.0)
                velocity = ScreenUVToNdc(uv) - prevClip.xy / prevClip.w;

            imageStore(u_DilatedVelocity, pixel, vec4(velocity, 0.0, 0.0));
            imageStore(u_ClosestDepth, pixel, vec4(closest));

            if (closest <= 0.0 || prevClip.w <= 0.0)
                return;
            const float prevDepth = clamp(prevClip.z / prevClip.w, 0.0, 1.0);
            const vec2  prevPixel = (uv - velocity * vec2(0.5, -0.5)) * u_RenderSize - 0.5;
            const ivec2 base      = ivec2(floor(prevPixel));
            for (int i = 0; i < 4; ++i)
            {
                const ivec2 q = base + ivec2(i & 1, i >> 1);
                if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, ivec2(u_RenderSize))))
                    continue;
                // Positive floats order as their bit patterns: atomicMax on the bits keeps the nearest.
                atomicMax(u_PrevAtomic[q.y * int(u_RenderSize.x) + q.x], floatBitsToUint(prevDepth));
            }
        }
    }
}
