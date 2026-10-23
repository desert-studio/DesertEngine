#pragma once

#include "System.hpp"
#include "SystemRules.hpp"

#include <Engine/ECS/AtmosphereSunLight.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/SkyRules.hpp>
#include <Engine/Graphic/SkySettings.hpp>
#include <Engine/Assets/AssetManager.hpp>

#include <Engine/Graphic/Render/Commands/SkyboxCommand.hpp>
#include <Engine/Graphic/Render/Commands/ProceduralSkyCommand.hpp>
#include <Engine/Graphic/SunLightFx.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Json/Template.hpp>

#include <glm/glm.hpp>

#include <format>
#include <optional>
#include <string>
#include <vector>

namespace Desert::ECS
{
    /// THE ONE CONVERSION from the authored component to the look the renderer samples with. The frame
    /// collector below and the editor's Details preview both call it, so a fourth knob added to the
    /// component is added here once and cannot reach the scene and miss the preview beside its slider.
    [[nodiscard]] inline Graphic::SkyLook SkyLookOf( const SkyboxComponent& skybox )
    {
        Graphic::SkyLook look;
        look.RotationDegrees = skybox.Rotation;
        look.Intensity       = skybox.Intensity;
        look.Tint            = skybox.Tint;
        return look;
    }

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
                    // A hidden Sky Atmosphere is not a candidate. Unticking Visible on the sky used to do
                    // NOTHING — this collector never asked — and the renderer keeps its sky state across
                    // frames, so the only thing that can clear the sky is the `Enabled = false` command
                    // emitted below when no candidate survives.
                    if ( ECS::IsHidden( registry, entity ) )
                        continue;
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
                     false, kNoSunDirection, /*bakeNow=*/false, Graphic::SkySettings{}, Graphic::SunLightFx{} );
            }
            else
            {
                auto& atmosphere = registry.get<ECS::SkyAtmosphereComponent>( skyEntities[*primarySky] );

                // One-shot Bake request from the editor: forward it for this frame, then clear it.
                const bool bakeNow     = atmosphere.RequestBake;
                atmosphere.RequestBake = false;

                atmosphereEnabled = atmosphere.Data.Enabled && sun.has_value();

                // NO SUN, NO ATMOSPHERE — and an error, not a default sun. The physical sky is the sun's light
                // scattered; a direction nobody authored would light a dome the ground below it does not
                // share (Scene.cpp collects no directional light in the same case). UE's sky atmosphere
                // likewise has nothing to scatter without an Atmosphere Sun Light.
                if ( atmosphere.Data.Enabled && !sun && !m_NoSunLogged )
                {
                    LOG_ERROR( "[SkyAtmosphere] Sky Atmosphere on '{}' is enabled but the scene has no visible "
                               "directional light with a usable direction — the atmosphere is NOT drawn. Add a "
                               "Directional Light and mark it Atmosphere Sun Light.",
                               EntityName( registry, skyEntities[*primarySky] ) );
                }
                m_NoSunLogged = atmosphere.Data.Enabled && !sun;

                // ONE conversion from the authored component to the renderer's transport struct — the
                // degrees-to-radians, diameter-to-radius and kilometres-to-world-units steps all live in it.
                renderCommandBuffer.Emplace<Graphic::Render::ProceduralSkyCommand>(
                     atmosphereEnabled, sun.value_or( kNoSunDirection ), bakeNow,
                     Graphic::MakeSkySettings( atmosphere.Data ), m_SunFx );
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
                    // Hidden HDR skybox: skip it and keep looking, so hiding one of two authored cubemaps
                    // hands the frame to the other rather than leaving the sky black-by-accident. If none
                    // survives, `cubemap` stays null and the SkyboxCommand below carries NOTHING — the
                    // one command every frame that makes "this scene has no HDR skybox" expressible.
                    if ( ECS::IsHidden( registry, skyboxEntity ) )
                        continue;

                    const auto& skybox = registry.get<ECS::SkyboxComponent>( skyboxEntity );
                    auto*       skyboxes = Runtime::ResourceRegistry::GetSkyboxService();
                    cubemap              = skyboxes->Get( skybox.SkyboxHandle );
                    // Being read or baked (AL1-3): keep drawing the sky that was there, never a black frame.
                    if ( !cubemap && skyboxes->IsPending( skybox.SkyboxHandle ) && m_LastCubemap )
                    {
                        if ( m_ReportedPending != skybox.SkyboxHandle )
                        {
                            LOG_INFO( "[Skybox] skybox {} is still loading; the previous sky is drawn until it "
                                      "is ready.",
                                      static_cast<uint64_t>( skybox.SkyboxHandle ) );
                            m_ReportedPending = skybox.SkyboxHandle;
                        }
                        cubemap = m_LastCubemap;
                    }
                    else if ( cubemap )
                    {
                        m_LastCubemap = cubemap;
                    }
                    look               = SkyLookOf( skybox );
                    break;
                }
            }

            renderCommandBuffer.Emplace<Graphic::Render::SkyboxCommand>( cubemap, look );
        }

    private:
        std::shared_ptr<Graphic::MaterialSkybox> m_LastCubemap;
        Assets::AssetHandle                      m_ReportedPending;

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
            std::vector<std::string> names;
            for ( size_t i = 0; i < entities.size(); ++i )
            {
                if ( i != chosen )
                    names.push_back( std::format( "'{}'", EntityName( registry, entities[i] ) ) );
            }
            return Common::Text::Join( names, ", " );
        }

        // Direction TOWARD the sun for this frame, or nothing when the scene has no sun. The choice of WHICH
        // light is ECS::PickAtmosphereSun — the same call the deferred light list, the shadow settings and
        // the clock make — and this function only reports what it said.
        std::optional<glm::vec3> ResolveAtmosphereSun( entt::registry& registry )
        {
            // Reset to defaults FIRST: the no-sun path returns without choosing a light, and a stale FX
            // slice from a light deleted last frame would keep its shafts in the sky.
            m_SunFx = Graphic::SunLightFx{};

            // A HIDDEN SUN IS NOT A SUN CANDIDATE (SunCandidacy::Lit): the sun disc, the shafts and the
            // atmosphere's in-scattering are that one light's contribution, and Scene.cpp drops the same
            // entity from the deferred light list through the same call.
            const AtmosphereSunPick pick      = PickAtmosphereSun( registry, SunCandidacy::Lit );
            const auto&             selection = pick.Selection;

            if ( !m_SunSelectionLogged )
            {
                m_SunSelectionLogged = true;

                for ( const size_t i : selection.Collisions )
                    LOG_WARN( "[SkyAtmosphere] Entity '{}' is also marked as Atmosphere Sun Light at index 0; "
                              "the lowest-id light drives the sky and this one is ignored.",
                              EntityName( registry, pick.Entities[i] ) );

                for ( const size_t i : selection.WrongIndex )
                    LOG_WARN( "[SkyAtmosphere] Entity '{}' is marked as Atmosphere Sun Light at index {}, but "
                              "the engine renders one directional light — only index 0 drives the sky.",
                              EntityName( registry, pick.Entities[i] ), pick.Candidates[i].Index );

                if ( selection.Fallback && selection.Chosen )
                    LOG_INFO( "[SkyAtmosphere] No directional light is marked as the Atmosphere Sun Light; "
                              "'{}' (lowest id) drives the sky.",
                              EntityName( registry, pick.Entities[*selection.Chosen] ) );
            }

            const auto sun = pick.Sun();
            if ( !sun )
                return std::nullopt;

            // The chosen light's render effects travel WITH its direction: the shafts are properties of
            // the sun, and reading them from any other light would let an unmarked light in the corner
            // of the scene tint the sky's own sun.
            const auto& light                         = registry.get<ECS::DirectionLightComponent>( *sun );
            m_SunFx.LightShaftBloom    = light.Data.LightShaftBloom;
            m_SunFx.BloomScale         = light.Data.BloomScale;
            m_SunFx.BloomThreshold     = light.Data.BloomThreshold;
            m_SunFx.BloomMaxBrightness = light.Data.BloomMaxBrightness;
            m_SunFx.BloomTint          = light.Data.BloomTint;
            m_SunFx.AffectedByAtmosphereTransmittance = light.Data.AffectedByAtmosphereTransmittance;
            m_SunFx.OuterSpaceIlluminance             = light.Data.Color * light.Data.Intensity;
            m_SunFx.SunDiskColorScale                 = light.Data.AtmosphereSunDiskColorScale;

            const auto& transform = registry.get<ECS::TransformComponent>( *sun );
            return Rules::AtmosphereSunDirection( transform.Translation );
        }

        // The evaluated FX slice of the chosen sun light, refreshed by ResolveAtmosphereSun each frame
        // and defaulted (shafts off, scale white) when no light is chosen.
        Graphic::SunLightFx m_SunFx;

        // Both of these describe the SCENE, not the frame, so they are said once. A per-frame LOG_WARN is
        // how a real message becomes invisible.
        bool m_SunSelectionLogged = false;
        bool m_DuplicateSkyLogged = false;
        // Latched on the STATE: the error is said when the sun goes missing and again if it goes missing
        // after coming back, never sixty times a second.
        bool m_NoSunLogged = false;

        // The direction a DISABLED sky command carries. It is never evaluated (SkyboxRenderer publishes an
        // empty AtmosphereEnv and uploads nothing while the sky is off); it only has to be a unit vector.
        static constexpr glm::vec3 kNoSunDirection{ 0.0f, 1.0f, 0.0f };
    };
} // namespace Desert::ECS
