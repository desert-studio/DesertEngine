#include "PBRSceneFrame.hpp"

#include <Engine/Graphic/Clouds/CloudShadowBinding.hpp>
#include <Engine/Graphic/Materials/SceneLightingBinding.hpp>

namespace Desert::Graphic
{
    void PBRSceneFrame::ApplyTo( Material* material ) const
    {
        if ( !material )
            return;

        using Core::Formats::Reads;
        using Core::Formats::SceneRead;
        const SceneRead groups = Groups( material->GetMaterialLayout() );

        if ( Reads( groups, SceneRead::Camera ) )
            SceneCameraBind( material, Camera );

        // World time, for any shader declaring TimeUB — the shader graph's Time node. It belongs in the
        // snapshot for the same reason everything else here does: it is per-frame scene state, and it is
        // the world clock (TimeSeconds), so pause and dilation reach the shader as they reach gameplay.
        if ( Reads( groups, SceneRead::Time ) )
        {
            SceneTimeBind( material, TimeSeconds );
        }

        if ( Reads( groups, SceneRead::Lights ) && PointLights != nullptr && SpotLights != nullptr &&
             DirectionLights != nullptr )
            SceneLightsBind( material, *PointLights, *SpotLights, *DirectionLights );

        // The map array is handed over as-is (`Image2D* const*`) rather than copied into a local: a copy
        // is where a fifth cascade would get lost, because a hand-written brace list does not grow with
        // kMaxCascades and does not fail to compile when it stops matching.
        //
        // CascadeCount, not kMaxCascades. The ceiling was passed here for as long as every renderer had
        // four cascades, which made the two indistinguishable; they are not, and the difference is a
        // preview that binds one map and would otherwise ask the shader to walk four.
        if ( Reads( groups, SceneRead::Shadow ) )
        {
            SceneShadowBind( material, CascadeViewProj, CascadeCount, ShadowBias, ShadowsEnabled, ShadowDebugMode,
                             ShowNormals, CascadeTexelWorld, LightingDebug );
        }

        if ( Reads( groups, SceneRead::Environment ) )
        {
            SceneEnvironmentBind( material, EnvironmentLook );
        }
        // The block only: u_CloudShadowMap is a pass parameter, bound by every mesh node that draws these
        // materials (CloudShadowMapOrWhite through RDG::PassBindings).
        if ( Reads( groups, SceneRead::CloudShadow ) )
        {
            CloudShadowUpload( material, CloudShadow );
        }
    }

    void PBRSceneFrame::ApplyTo( MaterialInstance* instance ) const
    {
        if ( !instance )
            return;
        ApplyTo( instance->GetParentMaterial() );
    }
} // namespace Desert::Graphic
