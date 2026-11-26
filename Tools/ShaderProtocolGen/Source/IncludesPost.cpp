// The post and screen-space protocols (film grade, GTAO, SSGI, subsurface, translucency lighting volume, depth of
// field, motion blur, TAA, TSR): one generated include per C++ layout, under Generated/Post/.
#include "IncludeLists.hpp"

#include <Engine/Graphic/PostProcessing/FilmGradeLayout.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/SSAOLayout.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/SSGILayout.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/SubsurfaceLayout.hpp>
#include <Engine/Graphic/Systems/Scene/Translucency/TranslucencyLightingVolumeLayout.hpp>
#include <Engine/Graphic/View/DepthOfFieldLayout.hpp>
#include <Engine/Graphic/View/MotionBlurLayout.hpp>
#include <Engine/Graphic/View/TemporalAALayout.hpp>
#include <Engine/Graphic/View/TemporalSuperResolutionLayout.hpp>

#include <array>

namespace Desert::ShaderProtocolGen
{
    namespace
    {
        constexpr std::array<const ShaderIncludeDecl*, 9> kPostIncludes = {
             &Graphic::kFilmGradeShaderInclude,
             &Graphic::System::kSSAOUBDataShaderInclude,
             &Graphic::System::kSSGIUniformsShaderInclude,
             &Graphic::System::kSubsurfaceViewGpuShaderInclude,
             &Graphic::kTranslucencyLightingVolumeShaderInclude,
             &Graphic::kDofParamsShaderInclude,
             &Graphic::kMotionBlurParamsShaderInclude,
             &Graphic::kTemporalAAParamsShaderInclude,
             &Graphic::kTsrParamsShaderInclude };
    } // namespace

    std::span<const ShaderIncludeDecl* const> PostIncludes()
    {
        return kPostIncludes;
    }
} // namespace Desert::ShaderProtocolGen
