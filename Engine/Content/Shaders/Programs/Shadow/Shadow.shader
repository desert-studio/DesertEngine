// DesertAsset {"Kind":"Shader","Guid":"8c15cd100049ba406879540002f6519a","Versions":{"SHDR":1},"Dependencies":[]}
Shader "Shadow"
{
    Fragment
    {
        // Light-space depth written to an R32F colour target (sampled later in PBR). Using a colour target
        // instead of a sampled depth-stencil image sidesteps Vulkan depth-aspect sampling caveats.
        Out(0) vec4 o_Depth;

        void main()
        {
            o_Depth = vec4(gl_FragCoord.z, 0.0, 0.0, 1.0);
        }
    }

    Vertex
    {
        In(0) vec3 a_Position;
        In(1) vec3 a_Normal;
        In(2) vec3 a_Tangent;
        In(3) vec3 a_Bitangent;
        In(4) vec2 a_TextureCoord;

        // The shared CameraUB is fed the LIGHT's view/projection by MaterialShadow (not the camera's).
        #include <Common/CameraUB.glslh>

        // Per-mesh transform, pushed automatically by Renderer::RenderMesh.
        PushConstant constants
        {
            mat4 Transform;
        } m_PushConstants;

        void main()
        {
            gl_Position = cameraUB.Projection * cameraUB.View * m_PushConstants.Transform * vec4(a_Position, 1.0);
        }
    }
}
