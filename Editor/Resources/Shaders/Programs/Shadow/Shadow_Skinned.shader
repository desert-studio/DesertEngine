// DesertAsset {"Kind":"Shader","Guid":"a6ca9bef934226c62c1daadf465602cf","Versions":{"SHDR":1},"Dependencies":[]}
Shader "Shadow_Skinned"
{
    // The (Skinned x ShadowDepth) cell of Graphic::MeshShaderFor's table, and the cell that did not
    // exist: the cascade pass had one caster shader, that shader transformed a_Position by the model
    // matrix alone, and a skinned mesh therefore rasterized its BIND pose into the map — except that it
    // never got that far, because the pass only ever walked the static queue and a character cast
    // nothing at all.
    //
    // Depth does not care which shader would have coloured the surface. It cares very much WHERE the
    // vertex ended up, which is precisely the axis a vertex path is: the fragment stage below is
    // Shadow.shader's, verbatim, and only the vertex stage differs.

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
        In(5) ivec4 a_BoneIndices;
        In(6) vec4  a_BoneWeights;

        // The shared CameraUB is fed the LIGHT's view/projection by MaterialShadow (not the camera's).
        #include <Common/CameraUB.glslh>

        // Transform is pushed per submesh by Renderer::RenderMesh. BoneOffset is written by
        // MaterialShadowSkinned::SetBoneOffset and is what lets ONE material per cascade serve every
        // skinned caster: all their poses are packed end to end into the single Bones buffer below, and
        // each recorded draw carries its own slice in a push constant, which Vulkan snapshots. Uploading
        // a pose per draw instead would leave every draw reading the last caster's pose.
        PushConstant constants
        {
            mat4 Transform;  // offset 0
            uint BoneOffset; // offset 64
        } m_PushConstants;

        // raw-glsl: implicit (shared) layout kept — std430 would change the bone matrix offsets.
        // Binding 1 is the skinned path's own slot, the same one SkinnedMeshPBR and Silhouette_Skinned use.
        layout(binding = 1) readonly buffer Bones
        {
            mat4 BoneMatrices[];
        } bones;

        void main()
        {
            // int, not uint: a_BoneIndices is ivec4 and GLSL will not mix the two silently.
            int b = int(m_PushConstants.BoneOffset);
            mat4 skin = bones.BoneMatrices[b + a_BoneIndices.x] * a_BoneWeights.x +
                        bones.BoneMatrices[b + a_BoneIndices.y] * a_BoneWeights.y +
                        bones.BoneMatrices[b + a_BoneIndices.z] * a_BoneWeights.z +
                        bones.BoneMatrices[b + a_BoneIndices.w] * a_BoneWeights.w;

            gl_Position = cameraUB.Projection * cameraUB.View * m_PushConstants.Transform * ( skin * vec4(a_Position, 1.0) );
        }
    }
}
