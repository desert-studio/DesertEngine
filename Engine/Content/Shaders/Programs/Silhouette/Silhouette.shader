// DesertAsset {"Kind":"Shader","Guid":"d37c5b1cc25c4c36a84689f282071eda","Versions":{"SHDR":1},"Dependencies":[]}
Shader "Silhouette"
{
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

        void main()
        {
            o_Color = vec4(1.0, 1.0, 1.0, 1.0);
        }
    }
}
