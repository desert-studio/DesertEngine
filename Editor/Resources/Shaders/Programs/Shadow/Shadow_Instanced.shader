// DesertAsset {"Kind":"Shader","Guid":"de9400f971c5e2ac21c65411080b933c","Versions":{"SHDR":1},"Dependencies":[]}
Shader "Shadow_Instanced"
{
    // Instanced variant of the depth-only shadow caster: identical fragment, but the vertex reads each caster's
    // model matrix from the InstanceTransforms SSBO (binding 16) by gl_InstanceIndex. Batched shadow casters of
    // the same mesh collapse to one instanced draw per cascade (the dominant cost in the 256-mesh stress test).

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

        // The shared CameraUB is fed the LIGHT's view/projection by MaterialShadowInstanced (not the camera's).
        #include <Common/CameraUB.glslh>

        // Per-instance world transforms (binding 17), indexed by gl_InstanceIndex. Anonymous block so reflection
        // registers it under the BLOCK name "InstanceTransforms" (matches Static_Instanced.glsl.vert and the C++
        // Get<StorageBufferProperty>("InstanceTransforms")). One instanced draw renders all N shadow casters.
        // Binding 17 (not 16) to stay consistent with the PBR instanced vertex, where 16 collides with SpotLightsUB.
        ReadBuffer(17) InstanceTransforms
        {
            mat4 transforms[];
        };

        void main()
        {
            mat4 model  = transforms[gl_InstanceIndex];
            gl_Position = cameraUB.Projection * cameraUB.View * model * vec4(a_Position, 1.0);
        }
    }
}
