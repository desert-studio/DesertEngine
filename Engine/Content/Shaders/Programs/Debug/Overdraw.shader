// DesertAsset {"Kind":"Shader","Guid":"0fe6bea21ae50de727ac34bee06928ef","Versions":{"SHDR":1},"Dependencies":[]}
Shader "Overdraw"
{
    // Overdraw accumulation pass. Every opaque mesh is re-drawn with NO depth test and ADDITIVE blend into a
    // dedicated float target, emitting a small constant per fragment — so a pixel shaded N times accumulates
    // N * step. OverdrawResolve then heat-maps the result. Vertex stage mirrors Silhouette (camera + push
    // constant model transform).

    Vertex
    {
        In(0) vec3 a_Position;
        In(1) vec3 a_Normal;
        In(2) vec3 a_Tangent;
        In(3) vec3 a_Bitangent;
        In(4) vec2 a_TextureCoord;

        #include <Common/CameraUB.glslh>

        // Transform is supplied automatically by Renderer::RenderMesh as the only push constant.
        PushConstant constants
        {
            mat4 Transform;
        } m_PushConstants;

        void main()
        {
            gl_Position = cameraUB.Projection * cameraUB.View * m_PushConstants.Transform * vec4(a_Position, 1.0);
        }
    }

    Fragment
    {
        Out(0) vec4 o_Color;

        // 0.1 per drawn fragment; with additive blend, 10 overlapping draws reach 1.0 (full heat in resolve).
        void main()
        {
            o_Color = vec4(0.1);
        }
    }
}
