#pragma once

#include "System.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityVisibility.hpp>

#include <Engine/Graphic/Render/Commands/SpotLightCommand.hpp>

#include <glm/gtc/constants.hpp>

namespace Desert::ECS
{
    class SpotLightECSSystem : public System
    {
    public:
        using System::System;

        // Render-data collector (read-only light collection) — safe to run concurrently with the other collectors.
        bool CanRunParallel() const override
        {
            return true;
        }

        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer& renderCommandBuffer,
                     const Common::Timestep& /*ts*/ ) override
        {
            auto spotLightView = registry.view<SpotLightComponent, TransformComponent>();

            spotLightView.each(
                 [&]( auto entity, const auto& spotlight, const auto& transform )
                 {
                     // A hidden light emits NOTHING. It is a light, not a mesh, so nothing about it is
                     // "drawn" — but the outliner's eye is the only switch an artist has for killing one
                     // contribution while keeping the authored radius/colour, and a light that keeps lighting
                     // the room after its entity was hidden is the exact complaint that opened this task.
                     if ( ECS::IsHidden( registry, entity ) )
                         return;

                     glm::mat4 worldTransform = transform.GetTransform();

                     entt::entity current = entity;
                     while ( registry.has<RelationshipComponent>( current ) )
                     {
                         const auto& rel = registry.get<RelationshipComponent>( current );
                         if ( rel.Parent == entt::null )
                             break;

                         current = rel.Parent;
                         if ( registry.has<TransformComponent>( current ) )
                         {
                             const auto& parentTransform = registry.get<TransformComponent>( current );
                             worldTransform = parentTransform.GetTransform() * worldTransform;
                         }
                     }

                     // Spot points along the entity's forward axis (-Z of the world rotation).
                     const glm::vec3 forward = glm::normalize( -glm::vec3( worldTransform[2] ) );

                     // Cone edges precomputed as cosines (shader avoids acos); clamp inner <= outer so the
                     // smooth edge never divides by zero.
                     const float outer    = glm::radians( spotlight.Data.OuterConeAngle );
                     const float inner    = glm::radians( glm::min( spotlight.Data.InnerConeAngle,
                                                                    spotlight.Data.OuterConeAngle ) );

                     Graphic::ShaderProtocols::SpotLightPayload light{};
                     light.Color     = spotlight.Data.Color;
                     light.Intensity = spotlight.Data.Intensity;
                     light.Position  = glm::vec3( worldTransform[3] );
                     light.Range     = spotlight.Data.Range;
                     light.Direction = forward;
                     light.CosInner  = glm::cos( inner );
                     light.CosOuter  = glm::cos( outer );
                     light.Falloff   = static_cast<int32_t>( spotlight.Data.Falloff );
                     // UE: contact shadows are part of the light's shadowing, so a light that casts none has none.
                     light.ContactShadowLength =
                          spotlight.Data.CastShadows ? spotlight.Data.ContactShadowLength : 0.0f;
                     light.ContactShadowInWorld          = spotlight.Data.ContactShadowLengthInWorldSpace ? 1 : 0;
                     light.ContactShadowCastingIntensity = spotlight.Data.ContactShadowCastingIntensity;
                     light.ContactShadowNonCastingIntensity = spotlight.Data.ContactShadowNonCastingIntensity;

                     // What the light asks of its shadow, and who it is (its tiles stay its own while it and
                     // its casters stand still: MeshRenderer::UpdateLocalShadows).
                     const Graphic::LocalLightShadowSettings shadow{
                          .CastShadows     = spotlight.Data.CastShadows,
                          .DepthBias       = spotlight.Data.ShadowBias,
                          .SlopeBias       = spotlight.Data.ShadowSlopeBias,
                          .ResolutionScale = spotlight.Data.ShadowResolutionScale,
                          .SourceId        = static_cast<uint32_t>( entity ) + 1u };

                     renderCommandBuffer.Emplace<Graphic::Render::SpotLightCommand>( light, shadow );
                 } );
        }
    };
} // namespace Desert::ECS
