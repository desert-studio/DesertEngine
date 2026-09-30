#include "MaterialShadow.hpp"

#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>
#include <Engine/Graphic/ShaderProtocols/Camera.hpp>
#include <Engine/Graphic/ShaderProtocols/SkinnedMaterialUB.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>

#include <Common/Core/Logger.hpp>

#include <cstddef>
#include <span>

namespace Desert::Graphic
{
    namespace
    {
        // The shaders come from the ONE table (MeshShaderFor) on the DEFAULT SURFACE template, found by that
        // role, so a caster variant cannot be named here and somewhere else and drift.
        std::string ShadowShaderName( MeshVertexPath path )
        {
            const auto name =
                 Runtime::ResourceRegistry::GetMaterialService()->DefaultSurfaceShader( path, MeshPass::ShadowDepth );
            if ( !name )
            {
                LOG_ERROR( "[MaterialShadow] no {} caster: {}", MeshVertexPathName( path ), name.GetError() );
                return {};
            }
            return name.GetValue();
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

    void WriteLightCamera( Material& material, const glm::mat4& view, const glm::mat4& projection )
    {
        ShaderProtocols::Camera cameraUB;
        cameraUB.Projection = projection;
        cameraUB.View       = view;
        cameraUB.CameraPos  = glm::vec3( 0.0f );

        if ( auto* camera = material.Get<UniformBufferProperty>( ShaderProtocols::Camera::Name ) )
        {
            const auto bytes = std::as_bytes( std::span{ &cameraUB, 1 } );
            camera->SetRawData( bytes.data(), bytes.size() );
        }
    }

    void MaterialShadow::SetLightMatrix( const glm::mat4& view, const glm::mat4& projection )
    {
        WriteLightCamera( *this, view, projection );
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
        // `BoneOffset` in the Skinned.ShadowDepth cell's push block (Vertex_Skinned.glslh), found by name.
        WritePushField( "BoneOffset", &firstBone, sizeof( uint32_t ) );
    }
} // namespace Desert::Graphic
