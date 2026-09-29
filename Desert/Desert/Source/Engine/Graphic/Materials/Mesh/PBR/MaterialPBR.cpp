#include "MaterialPBR.hpp"

#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/ShaderProtocols/SkinnedMaterialUB.hpp>
#include <Engine/Graphic/Shader.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Logger.hpp>

#include <string>

namespace Desert::Graphic
{
    std::shared_ptr<MaterialPBR> MaterialPBR::Create( MeshVertexPath path, MeshPass pass )
    {
        const char* shaderName = MeshShaderFor( path, pass );
        if ( !shaderName )
        {
            // Names the pair, because "material failed to create" is unactionable and this is exactly the
            // message that would have turned defect (2) in MeshVertexPath.hpp from a missing shadow into
            // a line in the log.
            LOG_ERROR( "[MaterialPBR] No mesh shader exists for vertex path '{}' in pass '{}'; refusing to "
                       "build a material for a combination the engine cannot draw.",
                       MeshVertexPathName( path ), MeshPassName( pass ) );
            return nullptr;
        }
        return std::shared_ptr<MaterialPBR>( new MaterialPBR( path, pass, shaderName ) );
    }

    MaterialPBR::MaterialPBR( MeshVertexPath path, MeshPass pass, const char* shaderName )
         : MaterialPBRBase( std::string( "PBR_" ) + MeshVertexPathName( path ) + "_" + MeshPassName( pass ),
                            std::string( shaderName ) ),
           m_Path( path ), m_Pass( pass )
    {
        if ( auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( shaderName ) )
            m_Schema = shader->GetProgramMeta();
        m_Row = Core::Formats::MaterialParamDefaultRow( m_Schema );
    }

    void MaterialPBR::UploadBones( const glm::mat4* matrices, size_t count )
    {
        // A pose uploaded to a material that has no skinning stage would vanish without a trace, so this
        // is a bug in the caller and says so rather than doing nothing.
        DESERT_VERIFY( m_Path == MeshVertexPath::Skinned, "UploadBones on a non-skinned vertex path" );
        if ( !matrices || count == 0 )
            return;
        if ( auto* sb = Get<StorageBufferProperty>( ShaderProtocols::SkinnedUB::Name ) )
            sb->SetRawData( matrices, static_cast<uint32_t>( count * sizeof( glm::mat4 ) ) );
    }

    void MaterialPBR::UpdateTransform( MaterialInstance* instance, const glm::mat4& transform )
    {
        instance->SetMat4( "Transform", transform );
    }

    void MaterialPBR::Bind( const MaterialInstance* instance )
    {
        if ( !m_MaterialExecutor )
            return;

        // Transform sub-block at offset 0 (RenderMesh overwrites it per submesh), and — on the skinned
        // path only — where this draw's bones start in the packed Bones buffer. Both are PUSH constants
        // and not material state on purpose: Vulkan snapshots a push at record time, so a value written
        // here cannot be clobbered by the next object's Bind before the GPU runs the draw. That is the
        // property the bone matrices did not have when they lived on the material.
        //
        // The row index is the third such value and is written by Material::SetMaterialIndex, straight
        // into this same buffer, because generic draws need it without ever calling Bind.
        glm::mat4 transform = instance->GetMat4( "Transform" );
        m_MaterialExecutor->PushConstant( &transform, sizeof( glm::mat4 ), kPushTransformOffset );
        if ( m_Path == MeshVertexPath::Skinned )
            m_MaterialExecutor->PushConstant( &m_BoneOffset, sizeof( uint32_t ), kPushBoneOffsetOffset );

        // Flush shared uniform buffers (camera/lights), textures and the Materials storage descriptor.
        Material::Bind( instance );
    }
} // namespace Desert::Graphic
