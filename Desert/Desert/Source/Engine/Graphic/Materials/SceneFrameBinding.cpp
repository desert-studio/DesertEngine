#include "SceneFrameBinding.hpp"

#include <Engine/Graphic/Clouds/CloudShadowBinding.hpp>
#include <Engine/Graphic/Materials/Properties/StorageBufferProperty.hpp>
#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>
#include <Engine/Graphic/Materials/SceneLightingBinding.hpp>

namespace Desert::Graphic
{
    void SceneFrameBinding::ApplyTo( Material* material ) const
    {
        if ( !material )
            return;

        using Core::Formats::Reads;
        using Core::Formats::SceneRead;
        const SceneRead groups = Groups( material->GetMaterialLayout() );

        if ( Reads( groups, SceneRead::Camera ) && View != nullptr )
            SceneCameraBind( material, *View );

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
            SceneShadowBind( material, CascadeViewProj, CascadeCount, ShadowsEnabled, ShadowDebugMode, ShowNormals,
                             CascadeTexelWorld, CascadeDepthRange, SunSourceTanHalf, LightingDebug, ContactShadow,
                             ShadowFilter );
        }

        if ( Reads( groups, SceneRead::Environment ) )
        {
            SceneEnvironmentBind( material, EnvironmentLook );
        }
        // The block only: CloudShadowMapTexture is a pass parameter, bound by every mesh node that draws these
        // materials (CloudShadowMapOrWhite through RDG::PassBindings).
        if ( Reads( groups, SceneRead::CloudShadow ) )
        {
            CloudShadowUpload( material, CloudShadow );
        }
        // A view-pass vertex stage with no rows would read an unbound buffer: the producer always fills them.
        if ( Reads( groups, SceneRead::ObjectMotion ) )
        {
            auto*      rows  = material->Get<StorageBufferProperty>( SceneResources::kObjectMotions );
            const bool bound = rows != nullptr && ObjectMotions != nullptr;
            DESERT_VERIFY( bound, "a view-pass material reads ObjectMotions but the frame carries none" );
            rows->LendViewBuffer( ObjectMotions );
        }
        if ( Reads( groups, SceneRead::ObjectBones ) )
        {
            auto*      palettes = material->Get<StorageBufferProperty>( SceneResources::kObjectBones );
            const bool bound    = palettes != nullptr && ObjectBones != nullptr;
            DESERT_VERIFY( bound, "a skinned view-pass material reads ObjectBones but the frame carries none" );
            palettes->LendViewBuffer( ObjectBones );
        }
        // A surface that reads a world parameter with no table bound would read an unbound buffer: every view
        // writes one each frame, an empty collection included.
        if ( Reads( groups, SceneRead::WorldParameters ) )
        {
            auto*      table = material->Get<StorageBufferProperty>( SceneResources::kWorldParameters );
            const bool bound = table != nullptr && WorldParameters != nullptr;
            DESERT_VERIFY( bound, "a material reads WorldParameters but the frame carries none" );
            table->LendViewBuffer( WorldParameters );
        }
        // The block only: RainOcclusionMapTexture is a pass parameter, bound by the mesh node that draws these
        // materials (SceneViewInputs::RainOcclusionMap through BindSceneViewInputs).
        if ( Reads( groups, SceneRead::RainOcclusion ) )
        {
            // Find: the RainOcclusion group is entered by RainOcclusionMapTexture as well, so a reader of the map
            // alone has no block.
            if ( auto* ub = material->Find<UniformBufferProperty>( SceneResources::kRainOcclusionBlock ) )
                ub->SetRawData( reinterpret_cast<const std::byte*>( &RainOcclusion ), sizeof( RainOcclusion ) );
        }
    }

    void SceneFrameBinding::ApplyTo( MaterialInstance* instance ) const
    {
        if ( !instance )
            return;
        ApplyTo( instance->GetParentMaterial() );
    }
} // namespace Desert::Graphic
