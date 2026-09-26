// DesertAsset {"Kind":"Shader","Guid":"68041dd187501b26980a7415a7b882fc","Versions":{"SHDR":1},"Dependencies":[]}
// Fully data-driven surface shader in the Desert Shader Language (single file: properties,
// render state and all stages together). The Properties block both drives the Details UI and
// (via Binding/TextureBinding) auto-generates this material's row of the shared `Materials[]`
// storage buffer + its samplers in the fragment stage — the parameter is declared exactly once.
Shader "Unlit"
{
    Domain Surface

    Properties Binding(1) TextureBinding(2)
    {
        Color     Color       ("Color")  = (0.8, 0.4, 0.1, 1)
        Texture2D u_AlbedoTex ("Albedo")
    }

    State
    {
        Cull Back
        ZTest LEqual
        ZWrite On
    }

    Vertex
    {
        In(0) vec3 a_Position;
        In(1) vec3 a_Normal;
        In(2) vec3 a_Tangent;
        In(3) vec3 a_Bitangent;
        In(4) vec2 a_TextureCoord;

        #include <Common/CameraUB.glslh>

        // Transform + the material row index, in the one block both stages declare (see the header).
        #include <Common/MaterialTransport.glslh>

        Out(0) vec2 v_UV;

        void main()
        {
            v_UV        = a_TextureCoord;
            gl_Position = cameraUB.Projection * cameraUB.View * m_PushConstants.Transform * vec4( a_Position, 1.0 );
        }
    }

    Fragment
    {
        In(0) vec2 v_UV;
        Out(0) vec4 o_Color;

        void main()
        {
            o_Color = texture( u_AlbedoTex, v_UV ) * u_Material.Color;
        }
    }

    // NO depth-only pass. There used to be a Pass "Depth" here, described as the shadow
    // variant; ShaderService registered it as its own program under
    // "Unlit/Depth" and compiled a SPIR-V module for it at every startup, and nothing could consume it —
    // no GetByName call in the engine has ever asked for a "<Shader>/<Pass>" name.
    //
    // It could not have served if one had, but NOT for the reason the file made it look like. Its vertex
    // stage was the right maths: byte for byte what Shadow.shader does, and reading cameraUB is exactly
    // how the engine's own shadow vertex works — MaterialShadow feeds the LIGHT's view/projection into
    // that same block, so shader text cannot tell a camera from a light. What was missing is that no
    // material would ever have done so for this program. The disqualifier is the other one: the pass
    // declared no FRAGMENT stage at all, while a cascade is a colour R32F attachment a fragment shader
    // must write (Shadow.shader writes gl_FragCoord.z into it), so it would have rasterized and emitted
    // nothing.
    //
    // A mesh with this material casts through the engine's shadow pipeline over the generic queue
    // (MeshRenderer::RegisterShadowPass). Depth is material-independent, so a per-material depth shader
    // has nothing to contribute. Tests/Engine/ShippedShaderPasses holds both halves of that.
}

