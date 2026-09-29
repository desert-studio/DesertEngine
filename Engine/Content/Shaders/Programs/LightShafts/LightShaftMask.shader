// DesertAsset {"Kind":"Shader","Guid":"c1dc048173848862fd9bd20e7717d0eb","Versions":{"SHDR":1},"Dependencies":[]}
Shader "LightShaftMask"
{
    Compute
    {
        // Light-shaft SOURCE MASK (UE's "Light Shaft Bloom", first half). Extracts the energy that is
        // allowed to streak: the HDR scene colour above a threshold, windowed by distance to the sun's
        // position on screen. Occlusion needs no separate depth pass here — the scene colour ALREADY
        // encodes it: geometry in front of the sky is lit surface radiance (below the threshold), so an
        // occluder in front of the sun leaves nothing above the threshold to streak. That is exactly the
        // "sun breaking through the gaps" look the reference shots are made of.
        //
        // Runs at half resolution: shafts are a low-frequency effect and the radial blur that follows
        // reads this image dozens of times per pixel.

        Uniform(0) sampler2D u_SceneColor;
        layout(binding = 1, rgba16f) restrict writeonly uniform image2D u_Mask;

        PushConstant ShaftMaskPush
        {
            vec2  u_SunUV;         // sun position in [0,1] screen UV (may be slightly off-screen)
            float u_Threshold;     // HDR luminance below this contributes nothing (UE: Bloom Threshold)
            float u_MaxBrightness; // cap on the extracted energy (UE: Bloom Max Brightness) — one
                                   // blown-out pixel must not own the whole streak
            float u_WindowRadius;  // radial window (UV units) beyond which the mask fades to zero
        };

        LocalSize(16, 16, 1);

        void main()
        {
            ivec2 size  = imageSize(u_Mask);
            ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
            if (coord.x >= size.x || coord.y >= size.y)
                return;

            vec2 uv = (vec2(coord) + vec2(0.5f, 0.5f)) / vec2(size);

            vec3  colour     = textureLod(u_SceneColor, uv, 0.0f).rgb;
            float brightness = max(colour.r, max(colour.g, colour.b));

            // Keep only the energy above the threshold, hue preserved — the same bright-pass form the
            // bloom uses, so the two effects agree about what "bright" means.
            float contribution = max(brightness - u_Threshold, 0.0f) / max(brightness, 1e-4f);
            vec3  masked       = min(colour * contribution, vec3(u_MaxBrightness));

            // Radial window about the sun: energy far from the sun has no business in a sun streak —
            // without this, every bright rim on screen would smear toward the sun.
            float d      = distance(uv, u_SunUV);
            float window = 1.0f - smoothstep(u_WindowRadius * 0.5f, u_WindowRadius, d);

            imageStore(u_Mask, coord, vec4(masked * window, 1.0f));
        }
    }
}
