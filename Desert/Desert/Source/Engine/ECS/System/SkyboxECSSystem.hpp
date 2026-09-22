#pragma once

#include "System.hpp"
#include "SystemRules.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/SkyRules.hpp>
#include <Engine/Graphic/SkySettings.hpp>
#include <Engine/Assets/AssetManager.hpp>

#include <Engine/Graphic/Render/Commands/SkyboxCommand.hpp>
#include <Engine/Graphic/Render/Commands/ProceduralSkyCommand.hpp>
#include <Engine/Graphic/SunLightFx.hpp>

#include <Common/Core/Logger.hpp>

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace Desert::ECS
{
    class SkyboxECSSystem : public System
    {
    public:
        using System::System;

        // Render-data collector (only touches sky component state) — safe to run concurrently with the other
        // collectors.
        bool CanRunParallel() const override
        {
            return true;
        }

        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer& renderCommandBuffer,
                     const Common::Timestep& /*ts*/ ) override
        {
            const auto sun = ResolveAtmosphereSun( registry );

            // --- Which sky entity drives the frame ---------------------------------------------------
            // Lowest UUID, not "whichever entity entt visited first": the old rule broke out of the loop
            // after one entity, and that order changes when an unrelated component is added elsewhere.
            std::vector<entt::entity> skyEntities;
            std::vector<uint64_t>     skyIds;
            {
                auto atmospheres = registry.view<ECS::SkyAtmosphereComponent>();
                for ( const auto entity : atmospheres )
                {
                    skyEntities.push_back( entity );
                    skyIds.push_back( EntityId( registry, entity ) );
                }
            }

            const auto primarySky = Graphic::SelectPrimarySky( skyIds );
            // `skyEntities.size() > 1` is not the same statement as "SelectPrimarySky found one" — it only
            // implies it through a contract of SelectPrimarySky that nothing at this call site says. Asked
            // directly, so a rule change there cannot turn this log line into a dereference of nothing.
            if ( primarySky.has_value() && skyEntities.size() > 1 && !m_DuplicateSkyLogged )
            {
                LOG_WARN( "[SkyAtmosphere] {} Sky Atmosphere components in the scene; the one on entity '{}' "
                          "drives the frame (lowest id). The others are ignored: {}",
                          skyEntities.size(), EntityName( registry, skyEntities[*primarySky] ),
                          OtherSkyNames( registry, skyEntities, *primarySky ) );
                m_DuplicateSkyLogged = true;
            }

            bool atmosphereEnabled = false;
            if ( !primarySky )
            {
                // No sky component at all. Say so EXPLICITLY rather than emitting nothing: the renderer
                // keeps its sky state across frames, so a component deleted mid-session would otherwise
                // leave the last one it saw on screen — and would keep publishing an AtmosphereEnv marked
                // valid for a sky that no longer exists.
                renderCommandBuffer.Emplace<Graphic::Render::ProceduralSkyCommand>(
                     false, Rules::FallbackAtmosphereSunDirection(), /*bakeNow=*/false, Graphic::SkySettings{},
                     Graphic::SunLightFx{} );
            }
            else
            {
                auto& atmosphere = registry.get<ECS::SkyAtmosphereComponent>( skyEntities[*primarySky] );

                // One-shot Bake request from the editor: forward it for this frame, then clear it.
                const bool bakeNow     = atmosphere.RequestBake;
                atmosphere.RequestBake = false;

                atmosphereEnabled = atmosphere.Data.Enabled;

                // ONE conversion from the authored component to the renderer's transport struct — the
                // degrees-to-radians, diameter-to-radius and kilometres-to-world-units steps all live in it.
                renderCommandBuffer.Emplace<Graphic::Render::ProceduralSkyCommand>(
                     atmosphere.Data.Enabled, sun, bakeNow, Graphic::MakeSkySettings( atmosphere.Data ), m_SunFx );
            }

            // The HDR cubemap is the other Sky-pass mode: only when no atmosphere is driving the sky, and
            // only if an asset is assigned.
            //
            // EXACTLY ONE COMMAND EVERY FRAME, carrying the cubemap or carrying NOTHING. It used to be
            // emitted only in the case that found one, which made "this scene has no HDR skybox"
            // inexpressible — and the renderer keeps its state across frames by design, so a cubemap it was
            // once given stayed. Deleting the SkyboxComponent left it drawing; switching the atmosphere on
            // left it feeding the IBL; loading a level without one onto a renderer that had one left the
            // previous LEVEL's sky behind the new world. Same rule, same reason, as the "no sky" command
            // above and as VolumetricCloudECSSystem's `present = false`.
            std::shared_ptr<Graphic::MaterialSkybox> cubemap;
            // The authored look travels WITH the cubemap, as one value. Three loose floats is three
            // chances for the next knob to reach the backdrop and miss the lighting — which is exactly
            // what happened to the brightness this replaces (Graphic::SkyLook).
            Graphic::SkyLook look{};

            if ( Graphic::ResolveSkyMode( atmosphereEnabled, /*hasHdrSkybox=*/true ) ==
                 Graphic::SkyMode::HdrCubemap )
            {
                auto skyboxes = registry.view<ECS::SkyboxComponent>();
                for ( const auto skyboxEntity : skyboxes )
                {
                    const auto& skybox = registry.get<ECS::SkyboxComponent>( skyboxEntity );
                    cubemap            = Runtime::ResourceRegistry::GetSkyboxService()->Get( skybox.SkyboxHandle );
                    look.Intensity       = skybox.Intensity;
                    look.RotationDegrees = skybox.Rotation;
                    look.Tint            = skybox.Tint;
                    break;
                }
            }

            renderCommandBuffer.Emplace<Graphic::Render::SkyboxCommand>( cubemap, look );
        }

    private:
        static uint64_t EntityId( entt::registry& registry, entt::entity entity )
        {
            if ( auto* id = registry.try_get<ECS::UUIDComponent>( entity ) )
                return static_cast<uint64_t>( id->UUID );
            return 0;
        }

        static std::string EntityName( entt::registry& registry, entt::entity entity )
        {
            if ( auto* tag = registry.try_get<ECS::TagComponent>( entity ) )
                return tag->Tag;
            return "<unnamed>";
        }

        static std::string OtherSkyNames( entt::registry& registry, const std::vector<entt::entity>& entities,
                                          size_t chosen )
        {
            std::string names;
            for ( size_t i = 0; i < entities.size(); ++i )
            {
                if ( i == chosen )
                    continue;
                if ( !names.empty() )
                    names += ", ";
                names += '\'' + EntityName( registry, entities[i] ) + '\'';
            }
            return names;
        }

        // Direction TOWARD the sun for this frame. The choice of WHICH light is a pure rule
        // (Rules::SelectAtmosphereSun); this function only fetches its arguments and reports what it said.
        glm::vec3 ResolveAtmosphereSun( entt::registry& registry )
        {
            // Reset to defaults FIRST: the fallback paths below return without choosing a light, and a
            // stale FX slice from a light deleted last frame would keep its shafts in the sky.
            m_SunFx = Graphic::SunLightFx{};

            std::vector<Rules::SunCandidate> candidates;
            std::vector<entt::entity>        entities;

            auto dirLights = registry.view<ECS::DirectionLightComponent, ECS::TransformComponent>();
            for ( const auto entity : dirLights )
            {
                const auto& transform = dirLights.get<ECS::TransformComponent>( entity );
                const auto& light     = dirLights.get<ECS::DirectionLightComponent>( entity );

                entities.push_back( entity );
                candidates.push_back( Rules::SunCandidate{
                     .Id             = EntityId( registry, entity ),
                     .Marked         = light.Data.AtmosphereSunLight,
                     .Index          = light.Data.AtmosphereSunLightIndex,
                     .DirectionValid = Rules::IsSunDirectionValid( transform.Translation ) } );
            }

            // v1 renders exactly one directional light, so index 0 is the only one that can be the sun.
            const auto selection = Rules::SelectAtmosphereSun( candidates, /*wantedIndex=*/0 );

            if ( !m_SunSelectionLogged )
            {
                m_SunSelectionLogged = true;

                for ( const size_t i : selection.Collisions )
                    LOG_WARN( "[SkyAtmosphere] Entity '{}' is also marked as Atmosphere Sun Light at index 0; "
                              "the lowest-id light drives the sky and this one is ignored.",
                              EntityName( registry, entities[i] ) );

                for ( const size_t i : selection.WrongIndex )
                    LOG_WARN( "[SkyAtmosphere] Entity '{}' is marked as Atmosphere Sun Light at index {}, but "
                              "the engine renders one directional light — only index 0 drives the sky.",
                              EntityName( registry, entities[i] ), candidates[i].Index );

                if ( selection.Fallback && selection.Chosen )
                    LOG_INFO( "[SkyAtmosphere] No directional light is marked as the Atmosphere Sun Light; "
                              "'{}' (lowest id) drives the sky.",
                              EntityName( registry, entities[*selection.Chosen] ) );

                if ( !selection.Chosen )
                    LOG_INFO( "[SkyAtmosphere] No directional light with a usable direction; the sky falls "
                              "back to its documented default sun." );
            }

            if ( !selection.Chosen )
                return Rules::FallbackAtmosphereSunDirection();

            // The chosen light's render effects travel WITH its direction: the shafts are properties of
            // the sun, and reading them from any other light would let an unmarked light in the corner
            // of the scene tint the sky's own sun.
            const auto& light          = registry.get<ECS::DirectionLightComponent>( entities[*selection.Chosen] );
            m_SunFx.LightShaftBloom    = light.Data.LightShaftBloom;
            m_SunFx.BloomScale         = light.Data.BloomScale;
            m_SunFx.BloomThreshold     = light.Data.BloomThreshold;
            m_SunFx.BloomMaxBrightness = light.Data.BloomMaxBrightness;
            m_SunFx.BloomTint          = light.Data.BloomTint;
            m_SunFx.AffectedByAtmosphereTransmittance = light.Data.AffectedByAtmosphereTransmittance;
            m_SunFx.OuterSpaceIlluminance             = light.Data.Color * light.Data.Intensity;

            const auto& transform = registry.get<ECS::TransformComponent>( entities[*selection.Chosen] );
            return Rules::AtmosphereSunDirection( transform.Translation );
        }

        // The evaluated FX slice of the chosen sun light, refreshed by ResolveAtmosphereSun each frame
        // and defaulted (shafts off, scale white) when no light is chosen.
        Graphic::SunLightFx m_SunFx;

        // Both of these describe the SCENE, not the frame, so they are said once. A per-frame LOG_WARN is
        // how a real message becomes invisible.
        bool m_SunSelectionLogged = false;
        bool m_DuplicateSkyLogged = false;
    };
} // namespace Desert::ECS
