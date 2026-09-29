#include "MaterialShadow.hpp"

#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>
#include <Engine/Graphic/ShaderProtocols/Camera.hpp>
#include <Engine/Graphic/ShaderProtocols/SkinnedMaterialUB.hpp>

namespace Desert::Graphic
{
    namespace
    {
        // The shaders come from the ONE table (MeshShaderFor), so a caster variant cannot be named here
        // and somewhere else and drift.
        std::string ShadowShaderName( MeshVertexPath path )
        {
            const char* name = MeshShaderFor( path, MeshPass::ShadowDepth );
            return name ? std::string( name ) : std::string();
        }
    } // namespace

    MaterialShadow::MaterialShadow() : Material( "MaterialShadow", ShadowShaderName( MeshVertexPath::Static ) )
    {
    }

    MaterialShadow::MaterialShadow( std::string&& debugName, std::string&& shaderName )
         : Material( std::move( debugName ), std::move( shaderName ) )
    {
    }

    MaterialShadowInstanced::MaterialShadowInstanced()
         : MaterialShadow( "MaterialShadowInstanced", ShadowShaderName( MeshVertexPath::Instanced ) )
    {
    }

    MaterialShadowSkinned::MaterialShadowSkinned()
         : MaterialShadow( "MaterialShadowSkinned", ShadowShaderName( MeshVertexPath::Skinned ) )
    {
    }

    void MaterialShadow::SetLightMatrix( const glm::mat4& view, const glm::mat4& projection )
    {
        ShaderProtocols::Camera cameraUB;
        cameraUB.Projection = projection;
        cameraUB.View       = view;
        cameraUB.CameraPos  = glm::vec3( 0.0f );

        Get<UniformBufferProperty>( ShaderProtocols::Camera::Name )
             ->SetRawData( reinterpret_cast<const std::byte*>( &cameraUB ), sizeof( cameraUB ) );
    }

    void MaterialShadowSkinned::UploadBones( const std::vector<glm::mat4>& packedBoneMatrices )
    {
        if ( packedBoneMatrices.empty() )
            return;
        if ( auto* sb = Get<StorageBufferProperty>( ShaderProtocols::SkinnedUB::Name ) )
            sb->SetRawData( packedBoneMatrices.data(),
                            static_cast<uint32_t>( packedBoneMatrices.size() * sizeof( glm::mat4 ) ) );
    }

    void MaterialShadowSkinned::SetBoneOffset( uint32_t firstBone )
    {
        // `BoneOffset` in Shadow_Skinned's push block, found by name in the cell's layout.
        WritePushField( "BoneOffset", &firstBone, sizeof( uint32_t ) );
    }
} // namespace Desert::Graphic
