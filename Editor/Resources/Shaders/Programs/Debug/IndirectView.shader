// DesertAsset {"Kind":"Shader","Guid":"f77413c80e194f6ca586d1c4a95c3aed","Versions":{"SHDR":1},"Dependencies":[]}
Shader "IndirectView"
{
    // The Indirect Lighting view mode (DeferredDebugMode::GI): the GI method's diffuse indirect this frame
    // (FrameTransients::GIDiffuse - the denoised SSGI signal, or the traced final gather's irradiance) multiplied
    // by the surface's diffuse colour, base colour x (1 - metallic) from GBufferA (UE DiffuseIndirectComposite:
    // DiffuseColor x DiffuseIndirect). Drawn over the post input by "Debug: Indirect" after the temporal resolve,
    // so the scene colour history the screen traces read back stays the LIT scene (System::IndirectViewRenderer).
    // Pre-exposed like the lit result (x PreExposure): the tonemap's 1 / PreExposure shows it as radiance.

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

        // Shared sampler state (UE's shared samplers); a same-name macro after each texture keeps the sampled form.
        Uniform sampler LinearClampSampler;
        Uniform(0) texture2D GiDiffuseTexture; // the GI method's diffuse indirect, the render extent
        #define GiDiffuseTexture sampler2D(GiDiffuseTexture, LinearClampSampler)
        Uniform(1) texture2D GBufferATexture; // rgb = base colour, a = metallic; 0 where no geometry (the sky)
        #define GBufferATexture sampler2D(GBufferATexture, LinearClampSampler)
        ReadBuffer(2) IndirectViewUB
        {
        	vec4 IndirectViewParams; // x = ViewFrame::PreExposure, yzw unused
        };

        Out(0) vec4 oColor;

        void main()
        {
        	const vec4 ga           = texture(GBufferATexture, v_TexCoord);
        	const vec3 diffuseColor = ga.rgb * (1.0 - clamp(ga.a, 0.0, 1.0));
        	const vec3 indirect     = max(texture(GiDiffuseTexture, v_TexCoord).rgb, vec3(0.0));
        	oColor                  = vec4(diffuseColor * indirect * IndirectViewParams.x, 1.0);
        }
    }
}
