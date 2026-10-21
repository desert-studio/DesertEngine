// DesertAsset {"Kind":"Shader","Guid":"a27a62a7d0e83c23d952f206693856e8","Versions":{"SHDR":1},"Dependencies":[]}
Shader "OverdrawResolve"
{
    // Fullscreen resolve for the Overdraw view: reads the additive accumulation buffer and maps the per-pixel
    // overdraw count to a heat colour (blue -> green -> yellow -> red), composited over the scene colour (LOAD).
    // Texels with no geometry drawn (accum == 0) are discarded so the scene / sky shows through.

    Vertex
    {
        #include <Common/FullscreenTriangle.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
        	v_TexCoord  = FullscreenTriangleUV();
        	gl_Position = vec4(FullscreenTriangleNdc(), 0.0, 1.0);
        }
    }

    Fragment
    {
        In(0) vec2 v_TexCoord;

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(0) sampler u_LinearRepeatSampler;
        Uniform(1) texture2D u_Overdraw; // .r = accumulated step (0.1 per fragment drawn)
        #define u_Overdraw sampler2D(u_Overdraw, u_LinearRepeatSampler)

        Out(0) vec4 oColor;

        // Standard "jet"-style ramp: 0 -> dark blue, up through cyan/green/yellow -> red.
        vec3 HeatColor(float t)
        {
        	t = clamp(t, 0.0, 1.0);
        	return clamp(vec3(1.5 - abs(4.0 * t - 3.0),
        	                  1.5 - abs(4.0 * t - 2.0),
        	                  1.5 - abs(4.0 * t - 1.0)), 0.0, 1.0);
        }

        void main()
        {
        	float acc = texture(u_Overdraw, v_TexCoord).r;
        	if (acc <= 0.0001)
        		discard; // no geometry drawn here -> keep the scene/sky underneath

        	// 0.1 per overdraw; 10 overlapping draws (= 1.0) map to full red.
        	oColor = vec4(HeatColor(acc), 1.0);
        }
    }
}
