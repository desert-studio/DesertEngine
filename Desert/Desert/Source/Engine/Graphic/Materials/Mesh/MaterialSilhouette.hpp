#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Core/Camera.hpp>

#include <vector>
#include <glm/glm.hpp>

namespace Desert::Graphic
{
    // Renders selected meshes as a flat white mask. The mask is consumed by the Jump Flood
    // outline pipeline (see JumpFloodOutlineRenderer) to build the object outline.
    //
    // The per-mesh transform is pushed automatically by Renderer::RenderMesh; this material
    // only feeds the shared camera uniform buffer.
    class MaterialSilhouette final : public Material
    {
    public:
        MaterialSilhouette();

        void UpdateCamera( const Core::Camera* camera );
    };

    // Skinned silhouette mask: feeds the camera UB AND the bone matrices (the same pose the mesh is rendered
    // with) so a selected skinned mesh's Jump Flood outline tracks its posed/animated shape.
    class MaterialSilhouetteSkinned final : public Material
    {
    public:
        MaterialSilhouetteSkinned();

        void UpdateCamera( const Core::Camera* camera );

        // Every selected skinned mesh's pose, packed end to end, uploaded ONCE; each draw then names its
        // slice with SetBoneOffset. It used to be one upload per mesh into a buffer the already-recorded
        // draws still pointed at, so outlining two skinned meshes at once drew both in the second one's
        // pose. Same shape as the forward and shadow skinned paths — see Material::SetSkinnedBoneOffset.
        void UploadBones( const std::vector<glm::mat4>& packedBoneMatrices );
        void SetBoneOffset( uint32_t firstBone );
    };
} // namespace Desert::Graphic
