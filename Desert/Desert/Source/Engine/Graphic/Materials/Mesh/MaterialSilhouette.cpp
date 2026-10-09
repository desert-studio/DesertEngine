#include "MaterialSilhouette.hpp"

#include <Engine/Graphic/Materials/SceneLightingBinding.hpp>
#include <Engine/Graphic/ShaderProtocols/SkinnedMaterialUB.hpp>

namespace Desert::Graphic
{
    MaterialSilhouette::MaterialSilhouette() : Material( "MaterialSilhouette", "Silhouette" )
    {
    }

    void MaterialSilhouette::UpdateCamera( const ViewFrame& view )
    {
        SceneCameraBind( this, view );
    }

    MaterialSilhouetteSkinned::MaterialSilhouetteSkinned()
         : Material( "MaterialSilhouetteSkinned", "Silhouette_Skinned" )
    {
    }

    void MaterialSilhouetteSkinned::UpdateCamera( const ViewFrame& view )
    {
        SceneCameraBind( this, view );
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
