// DesertAsset {"Kind":"Shader","Guid":"3aaf4d331cc05e4f9f58669726b859c7","Versions":{"SHDR":1},"Dependencies":[]}
Shader "Silhouette_Skinned"
{
    // Skinned variant of the silhouette mask: same flat-white fragment, but the vertex skins by the Bones SSBO
    // (binding 1) so a selected SKINNED mesh's Jump Flood outline matches its posed/animated shape.

    Vertex
    {
        In(0) vec3 a_Position;
        In(1) vec3 a_Normal;
        In(2) vec3 a_Tangent;
        In(3) vec3 a_Bitangent;
        In(4) vec2 a_TextureCoord;
        In(5) ivec4 a_BoneIndices;
        In(6) vec4  a_BoneWeights;

        #include <Common/CameraUB.glslh>
        #include <Common/SkinCacheStreams.glslh>

        // BoneOffset is this path's own field: ONE material serves every selected skinned mesh, so all
        // their poses are packed into the single Bones buffer below and each draw names its slice here.
        // Uploading per draw instead left every recorded draw reading the last mesh's pose — the same
        // defect the forward and shadow skinned paths carried.
        PushConstant constants
        {
            mat4 Transform;            // offset 0
            uint BoneOffset;           // offset 64
            // The primitive's GPU skin-cache streams (Common/SkinCacheStreams.glslh; MaterialSilhouetteSkinned::
            // SetSkinCache from its GpuObjectMotion row): vertex count 0 = not cached, skin from Bones below.
            uint SkinCacheVertexCount; // offset 68
            uint SkinCacheAddressLo;   // offset 72
            uint SkinCacheAddressHi;   // offset 76
        } m_PushConstants;

        // Same bone matrices the skinned mesh is rendered with, so the silhouette (hence the outline) matches the
        // posed/animated mesh exactly: the light views' Bones buffer (binding 1), generated from
        // ShaderProtocols/SkinnedMaterialUB.hpp.
        #include <Mesh/SkinnedBones.glslh>

        void main()
        {
            // A cached primitive was skinned once this frame by "SkinCache: Skin": fetch its posed position, so the
            // outline matches the drawn pose and nothing skins it a second time.
            vec3 posed;
            if (m_PushConstants.SkinCacheVertexCount != 0u)
            {
                posed = DesertSkinCachePosition(
                    DesertSkinCacheStreams(m_PushConstants.SkinCacheAddressLo, m_PushConstants.SkinCacheAddressHi),
                    uint(gl_VertexIndex));
            }
            else
            {
                // int, not uint: a_BoneIndices is ivec4 and GLSL will not mix the two silently.
                int b = int(m_PushConstants.BoneOffset);
                mat4 skin = bones.BoneMatrices[b + a_BoneIndices.x].Matrix * a_BoneWeights.x +
                            bones.BoneMatrices[b + a_BoneIndices.y].Matrix * a_BoneWeights.y +
                            bones.BoneMatrices[b + a_BoneIndices.z].Matrix * a_BoneWeights.z +
                            bones.BoneMatrices[b + a_BoneIndices.w].Matrix * a_BoneWeights.w;
                posed = (skin * vec4(a_Position, 1.0)).xyz;
            }

            gl_Position = cameraUB.Projection * cameraUB.View * m_PushConstants.Transform * vec4(posed, 1.0);
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
