#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>
#include <Engine/Graphic/ShadowCascades.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>

#include <glm/glm.hpp>

#include <vector>

namespace Desert::Graphic
{
    // Writes a cascade's light view into @p material's camera block (Projection, View, the light's eye and the
    // world clock @p timeSeconds) — the one statement of how a caster sees the light, shared by the renderer's own
    // caster materials and the per-cascade copies a Masked material casts through
    // (MaterialService::GetViewVariant). The clock is the frame's (UE's shadow view shares View.GameTime): a
    // Masked surface whose mask reads SurfaceInput.Time clips its shadow at the same instant it clips itself.
    // @p parallax is the scene view's (ViewFrame::Parallax): a parallax caster marches the relief with the steps
    // the view marches it with.
    void WriteLightCamera( Material& material, const glm::mat4& view, const glm::mat4& projection,
                           double timeSeconds, const CascadeCasterBias& bias, const ParallaxQuality& parallax );

    // Depth-only material for the directional shadow pass. Feeds the LIGHT's view/projection into the
    // shared CameraUB (the per-mesh transform is pushed by Renderer::RenderMesh).
    // @p pass is the caster cell it draws: ShadowDepth for a cascade or local view (depth attachment),
    // VirtualShadowDepth for a virtual shadow map level (the attachment-less pool raster). Both share the vertex
    // stage and the camera block, so one class serves both.
    class MaterialShadow : public Material
    {
    public:
        explicit MaterialShadow( MeshPass pass );

        void SetLightMatrix( const glm::mat4& view, const glm::mat4& projection, double timeSeconds,
                             const CascadeCasterBias& bias, const ParallaxQuality& parallax );

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
        explicit MaterialShadowInstanced( MeshPass pass );
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
        explicit MaterialShadowSkinned( MeshPass pass );

        void UploadBones( const std::vector<glm::mat4>& packedBoneMatrices );
        void SetBoneOffset( uint32_t firstBone );
        // The draw's GPU skin-cache streams (its GpuObjectMotion row's SkinCacheAddress / SkinCacheVertexCount):
        // a vertex count of 0 skins from the palette, any other fetches the pose "SkinCache: Skin" wrote this
        // frame.
        void SetSkinCache( uint64_t address, uint32_t vertexCount );
    };
} // namespace Desert::Graphic
