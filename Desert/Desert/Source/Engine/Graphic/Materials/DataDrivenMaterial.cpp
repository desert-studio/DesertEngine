#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>

#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

namespace Desert::Graphic
{
    DataDrivenMaterial::DataDrivenMaterial( const std::string& shaderName )
         : Material( "DDM_" + shaderName, std::string( shaderName ) ), m_ShaderName( shaderName )
    {
        if ( auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( shaderName ) )
            m_Schema = shader->GetProgramMeta();

        ApplyDefaults();
    }
} // namespace Desert::Graphic
