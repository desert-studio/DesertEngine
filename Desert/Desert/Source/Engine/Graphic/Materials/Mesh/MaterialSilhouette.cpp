#include "MaterialSilhouette.hpp"

#include <Engine/Graphic/ShaderProtocols/Camera.hpp>
#include <Engine/Graphic/ShaderProtocols/SkinnedMaterialUB.hpp>

namespace Desert::Graphic
{
    MaterialSilhouette::MaterialSilhouette() : Material( "MaterialSilhouette", "Silhouette" )
    {
    }

    void MaterialSilhouette::UpdateCamera( const Core::Camera* camera )
    {
        if ( !camera )
            return;

        ShaderProtocols::Camera cameraUB;
        cameraUB.Projection = camera->GetProjectionMatrix();
        cameraUB.View       = camera->GetViewMatrix();
        cameraUB.CameraPos  = camera->GetPosition();

        Get<UniformBufferProperty>( ShaderProtocols::Camera::Name )
             ->SetRawData( reinterpret_cast<const std::byte*>( &cameraUB ), sizeof( cameraUB ) );
    }

    MaterialSilhouetteSkinned::MaterialSilhouetteSkinned()
         : Material( "MaterialSilhouetteSkinned", "Silhouette_Skinned" )
    {
    }

    void MaterialSilhouetteSkinned::UpdateCamera( const Core::Camera* camera )
    {
        if ( !camera )
            return;

        ShaderProtocols::Camera cameraUB;
        cameraUB.Projection = camera->GetProjectionMatrix();
        cameraUB.View       = camera->GetViewMatrix();
        cameraUB.CameraPos  = camera->GetPosition();

        Get<UniformBufferProperty>( ShaderProtocols::Camera::Name )
             ->SetRawData( reinterpret_cast<const std::byte*>( &cameraUB ), sizeof( cameraUB ) );
    }

    void MaterialSilhouetteSkinned::UploadBones( const std::vector<glm::mat4>& packedBoneMatrices )
    {
        if ( packedBoneMatrices.empty() )
            return;
        if ( auto* sb = Get<StorageBufferProperty>( ShaderProtocols::SkinnedUB::Name ) )
            sb->SetRawData( packedBoneMatrices.data(),
                            static_cast<uint32_t>( packedBoneMatrices.size() * sizeof( glm::mat4 ) ) );
    }

    void MaterialSilhouetteSkinned::SetBoneOffset( uint32_t firstBone )
    {
        // `BoneOffset` in Silhouette_Skinned's push block, found by name in the cell's layout; the whole
        // reflected range is pushed per draw.
        WritePushField( "BoneOffset", &firstBone, sizeof( uint32_t ) );
    }
} // namespace Desert::Graphic
