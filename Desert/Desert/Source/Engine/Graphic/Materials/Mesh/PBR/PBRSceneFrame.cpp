#include "PBRSceneFrame.hpp"

#include <Engine/Graphic/Clouds/CloudShadowBinding.hpp>
#include <Engine/Graphic/Materials/SceneLightingBinding.hpp>

#include <chrono>

namespace Desert::Graphic
{
    void PBRSceneFrame::ApplyTo( Material* material ) const
    {
        if ( !material )
            return;

        SceneCameraBind( material, Camera );

        // Engine time, for any shader declaring TimeUB — the shader graph's Time node. It belongs in the
        // snapshot for the same reason everything else here does: it is per-frame scene state, and while
        // it was filled only inside MeshRenderer::DrawGenericMeshes it was the shape of the problem
        // rather than an exception to it.
        {
            static const auto s_TimeOrigin = std::chrono::steady_clock::now();
            SceneTimeBind(
                 material,
                 std::chrono::duration<float>( std::chrono::steady_clock::now() - s_TimeOrigin ).count() );
        }

        if ( PointLights && SpotLights && DirectionLights )
            SceneLightsBind( material, *PointLights, *SpotLights, *DirectionLights );

        // The map array is handed over as-is (`Image2D* const*`) rather than copied into a local: a copy
        // is where a fifth cascade would get lost, because a hand-written brace list does not grow with
        // kMaxCascades and does not fail to compile when it stops matching.
        //
        // CascadeCount, not kMaxCascades. The ceiling was passed here for as long as every renderer had
        // four cascades, which made the two indistinguishable; they are not, and the difference is a
        // preview that binds one map and would otherwise ask the shader to walk four.
        SceneShadowBind( material, CascadeViewProj, CascadeMaps, CascadeCount, ShadowBias, ShadowsEnabled,
                         ShadowDebugMode, ShowNormals, CascadeTexelWorld, LightingDebug );

        SceneEnvironmentBind( material, IrradianceMap, PrefilteredMap, BrdfLut, EnvironmentLook );
        CloudShadowBind( material, CloudShadow );
    }

    void PBRSceneFrame::ApplyTo( MaterialInstance* instance ) const
    {
        if ( !instance )
            return;
        ApplyTo( instance->GetParentMaterial() );
    }
} // namespace Desert::Graphic
