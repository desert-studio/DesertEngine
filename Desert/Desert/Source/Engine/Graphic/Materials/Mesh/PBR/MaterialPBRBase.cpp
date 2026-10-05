#include "MaterialPBRBase.hpp"

namespace Desert::Graphic
{
    MaterialPBRBase::MaterialPBRBase( std::string&& debugName, std::string&& shaderName,
                                      const Core::Formats::ShaderProgramMeta* parameterSchema )
         : Material( std::move( debugName ), std::move( shaderName ), parameterSchema )
    {
    }
} // namespace Desert::Graphic
