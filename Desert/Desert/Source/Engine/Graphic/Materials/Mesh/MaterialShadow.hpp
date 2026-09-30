#pragma once

#include <Engine/Graphic/Materials/Material.hpp>

#include <glm/glm.hpp>

#include <vector>

namespace Desert::Graphic
{
    // Writes a cascade's light matrices into @p material's camera block (Projection, View, CameraPos = 0) — the
    // one statement of how a caster sees the light, shared by the renderer's own caster materials and the
    // per-cascade copies a Masked material casts through (MaterialService::GetViewVariant).
    void WriteLightCamera( Material& material, const glm::mat4& view, const glm::mat4& projection );

    // Depth-only material for the directional shadow pass. Feeds the LIGHT's view/projection into the
    // shared CameraUB (the per-mesh transform is pushed by Renderer::RenderMesh).
    class MaterialShadow : public Material
    {
    public:
        MaterialShadow();

        void SetLightMatrix( const glm::mat4& view, const glm::mat4& projection );

    protected:
        // Lets a path variant bind a different shader while reusing SetLightMatrix. The debug name is a
        // parameter and not derived from the shader name because it is what the Vulkan object labels and
        // the descriptor-pool logs are keyed on — two variants sharing one label made the instanced and
        // the skinned caster indistinguishable in a capture.
        MaterialShadow( std::string&& debugName, std::string&& shaderName );
    };

    // Instanced depth-only shadow material: bound to StandardSurface/Instanced.ShadowDepth, whose vertex reads
    // each caster's model matrix from the InstanceTransforms SSBO (binding 16) by gl_InstanceIndex.
    class MaterialShadowInstanced final : public MaterialShadow
    {
    public:
        MaterialShadowInstanced();
    };

    // Skinned depth-only shadow material: the (Skinned x ShadowDepth) cell of MeshShaderFor's table,
    // which did not exist and is why a character cast no shadow while its silhouette outline tracked its
    // pose perfectly. Depth does not care what shader would have coloured the surface, but it very much
    // cares WHERE the vertex ended up — so a caster whose vertices are skinned needs a caster shader that
    // skins them, and the cascade pass has to walk the skinned queue as well as the static one.
    //
    // ONE material serves a whole cascade (like MaterialShadow does), so the poses of every skinned
    // caster are packed end to end into its single Bones buffer and each draw names its own slice with
    // SetBoneOffset. Uploading per draw instead would leave every recorded draw pointing at the last
    // caster's pose, which is the same hazard the glass and RSM passes were given their own materials to
    // avoid.
    class MaterialShadowSkinned final : public MaterialShadow
    {
    public:
        MaterialShadowSkinned();

        void UploadBones( const std::vector<glm::mat4>& packedBoneMatrices );
        void SetBoneOffset( uint32_t firstBone );
    };
} // namespace Desert::Graphic
