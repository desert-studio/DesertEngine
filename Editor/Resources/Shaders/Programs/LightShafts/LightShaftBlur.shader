// DesertAsset {"Kind":"Shader","Guid":"c2f1a96e5a86b31899cfd81b09efb640","Versions":{"SHDR":1},"Dependencies":[]}
Shader "LightShaftBlur"
{
    Compute
    {
        // Radial blur toward the sun (UE's "Light Shaft Bloom", second half; Mitchell, GPU Gems 3 ch.13).
        // Each pass samples N points from the pixel toward the sun with exponentially decaying weights;
        // run three times with the reach growing N-fold per pass, the effective kernel is N^3 samples
        // long — a full-screen streak for the price of 3 x N taps.

        Uniform(0) sampler2D u_Source;
        layout(binding = 1, rgba16f) restrict writeonly uniform image2D u_Output;

        PushConstant ShaftBlurPush
        {
            vec2  u_SunUV;   // streak origin in [0,1] UV
            float u_Reach;   // fraction of the pixel->sun distance this pass walks (grows per pass)
            float u_Decay;   // per-tap weight decay along the streak
        };

        LocalSize(16, 16, 1);

        const int kTaps = 12;

        void main()
        {
            ivec2 size  = imageSize(u_Output);
            ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
            if (coord.x >= size.x || coord.y >= size.y)
                return;

            vec2 uv     = (vec2(coord) + vec2(0.5f, 0.5f)) / vec2(size);
            vec2 toward = (u_SunUV - uv) * (u_Reach / float(kTaps));

            vec3  total  = vec3(0.0f, 0.0f, 0.0f);
            float weight = 1.0f;
            float sum    = 0.0f;
            vec2  tapUv  = uv; // NOT named `sample` — that is a reserved word in GLSL
            for (int i = 0; i < kTaps; ++i)
            {
                // The global sampler is REPEAT; a tap past the edge would wrap and streak the far side
                // of the screen into this one.
                total += textureLod(u_Source, clamp(tapUv, vec2(0.0f), vec2(1.0f)), 0.0f).rgb * weight;
                sum += weight;
                weight *= u_Decay;
                tapUv += toward;
            }

            imageStore(u_Output, coord, vec4(total / max(sum, 1e-4f), 1.0f));
        }
    }
}
