#include "IncludeLists.hpp"

#include <Water/Private/WaterLayout.hpp>
#include <Water/Public/WaterLayout.hpp>

#include <array>

namespace Desert::ShaderProtocolGen
{
    namespace
    {
        constexpr std::array<const ShaderIncludeDecl*, 8> kWaterIncludes = {
             &Water::kWaterShoreSampleShaderInclude,
             &Graphic::System::kWaterMeshTilesShaderInclude,
             &Graphic::System::kWaterCausticsShaderInclude,
             &Graphic::System::kWaterFoamRowsShaderInclude,
             &Graphic::System::kWaterUnderwaterShaderInclude,
             &Graphic::System::kWaterFoamUpdateShaderInclude,
             &Graphic::System::kWaterShoreUploadShaderInclude,
             &Graphic::System::kWaterFFTShaderInclude,
        };
    } // namespace

    std::span<const ShaderIncludeDecl* const> WaterIncludes()
    {
        return kWaterIncludes;
    }
} // namespace Desert::ShaderProtocolGen
