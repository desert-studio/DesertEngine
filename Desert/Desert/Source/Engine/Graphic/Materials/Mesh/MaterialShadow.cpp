#include "MaterialShadow.hpp"

#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>
#include <Engine/Graphic/Materials/SceneLightingBinding.hpp>
#include <Engine/Graphic/ShaderProtocols/SkinnedMaterialUB.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>

#include <Common/Core/Logger.hpp>

#include <cstddef>

namespace Desert::Graphic
{
    namespace
    {
        // The shaders come from the ONE table (MeshShaderFor) on the DEFAULT SURFACE template, found by that
        // role, so a caster variant cannot be named here and somewhere else and drift.
        std::string ShadowShaderName( MeshVertexPath path, MeshPass pass )
        {
            const auto name = Runtime::ResourceRegistry::GetMaterialService()->DefaultSurfaceShader( path, pass );
            if ( !name )
            {
                LOG_ERROR( "[MaterialShadow] no ({} x {}) caster: {}", MeshVertexPathName( path ),
                           MeshPassName( pass ), name.GetError() );
                return {};
            }
            return name.GetValue();
        }
    } // namespace

    MaterialShadow::MaterialShadow( const MeshPass pass )
         : Material( "MaterialShadow", ShadowShaderName( MeshVertexPath::Static, pass ) )
    {
    }

    MaterialShadow::MaterialShadow( std::string&& debugName, std::string&& shaderName )
         : Material( std::move( debugName ), std::move( shaderName ) )
    {
    }

    MaterialShadowInstanced::MaterialShadowInstanced( const MeshPass pass )
         : MaterialShadow( "MaterialShadowInstanced", ShadowShaderName( MeshVertexPath::Instanced, pass ) )
    {
    }

    MaterialShadowSkinned::MaterialShadowSkinned( const MeshPass pass )
         : MaterialShadow( "MaterialShadowSkinned", ShadowShaderName( MeshVertexPath::Skinned, pass ) )
    {
    }

    void WriteLightCamera( Material& material, const glm::mat4& view, const glm::mat4& projection,
                           const double timeSeconds, const CascadeCasterBias& bias,
                           const ParallaxQuality& parallax )
    {
        // A light camera is not a view: no jitter, no previous frame (MakeStillViewFrame); the camera block still
        // has its one writer. Its position is the light camera's eye, its clock the frame's. The constant bias
        // is already in `projection` (CasterViewProj); the slope term travels in the block.
        ViewFrame frame =
             MakeStillViewFrame( view, projection, glm::vec3( glm::inverse( view )[3] ), timeSeconds );
        frame.ShadowSlopeBias  = bias.SlopeBias;
        frame.ShadowSlopeClamp = bias.SlopeClamp;
        frame.Parallax         = parallax;
        SceneCameraBind( &material, frame );
    }

    void MaterialShadow::SetLightMatrix( const glm::mat4& view, const glm::mat4& projection,
                                         const double timeSeconds, const CascadeCasterBias& bias,
                                         const ParallaxQuality& parallax )
    {
        WriteLightCamera( *this, view, projection, timeSeconds, bias, parallax );
    }

    void MaterialShadowSkinned::UploadBones( const std::vector<glm::mat4>& packedBoneMatrices )
    {
        if ( packedBoneMatrices.empty() )
            return;
        if ( auto* sb = Get<StorageBufferProperty>( ShaderProtocols::SkinnedUB::Parameter ) )
            sb->SetRawData( packedBoneMatrices.data(),
                            static_cast<uint32_t>( packedBoneMatrices.size() * sizeof( glm::mat4 ) ) );
    }

    void MaterialShadowSkinned::SetBoneOffset( uint32_t firstBone )
    {
        // `BoneOffset` in the Skinned.ShadowDepth cell's push block (Vertex_Skinned.glslh), found by name.
        WritePushField( "BoneOffset", &firstBone, sizeof( uint32_t ) );
    }

    void MaterialShadowSkinned::SetSkinCache( const uint64_t address, const uint32_t vertexCount )
    {
        // The push block's SkinCache* fields (Mesh/Surface/Vertex_Skinned.glslh), found by name like BoneOffset.
        const uint32_t addressLo = static_cast<uint32_t>( address & 0xffffffffu );
        const uint32_t addressHi = static_cast<uint32_t>( address >> 32u );
        WritePushField( "SkinCacheVertexCount", &vertexCount, sizeof( uint32_t ) );
        WritePushField( "SkinCacheAddressLo", &addressLo, sizeof( uint32_t ) );
        WritePushField( "SkinCacheAddressHi", &addressHi, sizeof( uint32_t ) );
    }
} // namespace Desert::Graphic
