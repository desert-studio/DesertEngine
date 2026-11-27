#include <VFX/Public/ParticleRenderer.hpp>
#include <VFX/Public/VFXComponent.hpp>
#include <VFX/Public/VFXSystem.hpp>
#include <VFX/Public/VFXSystemAsset.hpp>
#include <VFX/Public/VFXSystemService.hpp>
#include <VFX/Public/VFXWorld.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AssetRootSet.hpp>
#include <Common/Core/Logger.hpp>
#include <Engine/Core/DebugDraw.hpp>
#include <Engine/Core/ModuleHooks.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/SequencerEffectDrive.hpp>
#include <Engine/Graphic/Systems/Scene/SceneFXSystem.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// THE VFX MODULE (UE: the Niagara plugin's StartupModule). Everything effects add to the engine is registered
// here, through the engine's generic hooks: the scene's effects world (a per-scene module state), the `.dfx`
// service, the systems the placed effects name as asset roots, the component pool, the sequencer's effect drive
// and the renderer's FX system (ISceneFXSystem). Engine names only VFX::StartupModule (Engine/Core/Modules.cpp).

namespace Desert::Runtime
{
    VFXSystemService* GetVFXSystemService()
    {
        static VFXSystemService service;
        return &service;
    }
} // namespace Desert::Runtime

namespace Desert::VFX
{
    namespace
    {
        // The slot of the effects world in every scene's module states (ModuleHooks().SceneStates).
        std::size_t g_WorldSlot = 0;

        class VFXSceneState final : public Core::ISceneModuleState
        {
        public:
            void Tick( Core::Scene& scene, entt::registry& registry, const double seconds ) override
            {
                {
                    DESERT_PROFILE_SCOPE( "VFX World" );
                    World.Tick( registry, seconds );
                }
#if DESERT_DEV_INSTRUMENTS
                // VFX-16 (UE Niagara debugger "show bounds"): every system's fixed bounds, its local box through
                // the entity's transform, as this frame's debug lines.
                if ( !World.GetDebugSettings().DrawBounds )
                    return;
                for ( const VFXSystemDebugStats& system : World.GetDebugStats().Systems )
                    for ( const VFXEmitterDebugStats& emitter : system.Emitters )
                    {
                        if ( emitter.Capacity == 0 )
                            continue; // disabled: draws nothing, culls nothing
                        // The box the renderer culls the emitter by (VFXBoundsAt: the entity's full transform).
                        const Common::Math::AABB box =
                             Assets::Serialization::VFXBoundsAt( emitter.Bounds, glm::mat4( system.Transform ) );
                        glm::vec3 corner[8];
                        for ( int i = 0; i < 8; ++i )
                            corner[i] =
                                 glm::vec3( ( i & 1 ) ? box.Max.x : box.Min.x, ( i & 2 ) ? box.Max.y : box.Min.y,
                                            ( i & 4 ) ? box.Max.z : box.Min.z );
                        Core::DebugDrawStyle style;
                        style.Color = glm::vec4( 1.0f, 0.55f, 0.1f, 1.0f );
                        for ( int i = 0; i < 8; ++i )
                            for ( int axis = 1; axis < 8; axis <<= 1 )
                                if ( ( i & axis ) == 0 )
                                    Core::DebugDraw::Line( scene, corner[i], corner[i | axis], style ); // 12 edges
                    }
#else
                (void)scene;
#endif
            }

            void Reset() override
            {
                World.Reset();
            }

            void Clear() override
            {
                World.Clear();
            }

            VFXWorld World;
        };
    } // namespace

    VFXWorld& VFXWorld::Of( Core::Scene& scene )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the slot holds this module's state
        return static_cast<VFXSceneState&>( scene.GetModuleState( g_WorldSlot ) ).World;
    }

    const VFXWorld& VFXWorld::Of( const Core::Scene& scene )
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the slot holds this module's state
        return static_cast<const VFXSceneState&>( scene.GetModuleState( g_WorldSlot ) ).World;
    }

    void StartupModule()
    {
        Core::ModuleHookLists& hooks = Core::ModuleHooks();
        g_WorldSlot                  = hooks.SceneStates.size();
        hooks.SceneStates.push_back( []( entt::registry& /*registry*/ ) -> std::unique_ptr<Core::ISceneModuleState>
                                     { return std::make_unique<VFXSceneState>(); } );
        hooks.Services.push_back( { []( const std::weak_ptr<Assets::AssetManager>& assets )
                                    { Runtime::GetVFXSystemService()->BindAssetManager( assets ); },
                                    []() { Runtime::GetVFXSystemService()->Clear(); } } );
        // A VFX system is named only by the entities that play it: without this row the eviction sweep drops the
        // `.dfx` under a live entity and VFXWorld finds a handle the manager no longer has.
        hooks.AssetRoots.push_back(
             []( entt::registry& registry, Assets::AssetRootSet& roots )
             {
                 for ( const auto entity : registry.view<ECS::VFXComponent>() )
                     roots.Mark( registry.get<ECS::VFXComponent>( entity ).Data.System, "an entity plays it" );
             } );
        // The `.dfx` window edits the asset in place; a reference stored by path is announced, not read:
        // VFXSystemService requests the read from the registry row on the first tick that asks for it, and the
        // asset's constructor adopts the handle its header GUID names, so the row and this shell agree.
        hooks.AssetTypes.push_back(
             { "VFXSystemAsset",
               []( Assets::AssetManager& m, const std::filesystem::path& full ) -> std::uint64_t
               {
                   auto a = m.FindByPath<Assets::VFXSystemAsset>( full );
                   if ( !a )
                       a = m.CreateAsset<Assets::VFXSystemAsset>( full, /*loadAfterCreate=*/false );
                   return a ? static_cast<std::uint64_t>( a->GetMetadata().Handle ) : 0;
               },
               nullptr } );
        hooks.HotReloads.push_back(
             { []( Assets::AssetManager& assetManager, std::vector<std::shared_ptr<Assets::AssetBase>>& watched )
               {
                   for ( const auto& [handle, asset] : assetManager.FindAllByType<Assets::VFXSystemAsset>() )
                       if ( asset )
                           watched.push_back( asset );
               },
               []( Assets::AssetManager&                                      assetManager,
                   const std::function<bool( const std::filesystem::path& )>& changed )
               {
                   for ( const auto& [handle, asset] : assetManager.FindAllByType<Assets::VFXSystemAsset>() )
                   {
                       if ( !asset )
                           continue;

                       // Touched before the readiness test, for the prefab's reason.
                       const auto& path   = asset->GetMetadata().Filepath;
                       const bool  edited = changed( path );
                       if ( !edited || !asset->IsReadyForUse() )
                           continue;

                       const std::string key = path.generic_string();

                       // UNSAVED EDITS ARE THE ONLY COPY (the `.dfx` window edits this asset in place): the file
                       // is not read over them. The window shows the question - Reload (the file wins) or Keep
                       // (the edits win).
                       if ( !asset->IsReloadableFromFile() )
                       {
                           asset->NoteChangedOnDisk();
                           LOG_WARN( "[HotReload] VFX system '{}' changed on disk, unsaved edits kept - its "
                                     "window offers "
                                     "Reload (read the file) or Keep (the next Save overwrites it).",
                                     key );
                           continue;
                       }

                       const auto before = asset->Snapshot();
                       if ( const auto reloaded = asset->LoadFromFile(); !reloaded )
                       {
                           LOG_ERROR( "[HotReload] VFX system '{}' could not be re-read (the last good system is "
                                      "kept): {}",
                                      key, reloaded.GetError() );
                           continue;
                       }
                       // The file holds what the asset holds (its own save, a touch): nothing was replaced.
                       if ( asset->Snapshot() == before )
                           continue;
                       LOG_INFO( "[HotReload] VFX system '{}' reloaded - {} emitter(s); every VFXComponent naming "
                                 "it plays "
                                 "the file's version from the next VFX tick.",
                                 key, asset->GetData().Emitters.size() );
                   }
               } } );
        hooks.Prepares.push_back( []( entt::registry& r ) { r.prepare<ECS::VFXComponent>(); } );

        ECS::SequencerEffectHookTable& sequencer = ECS::SequencerEffectHooks();
        sequencer.DriveOf                        = []( entt::registry&    registry,
                                const entt::entity entity ) -> std::optional<ECS::VFXSequencerDrive>*
        {
            auto* vfx = registry.try_get<ECS::VFXComponent>( entity );
            return vfx != nullptr ? &vfx->SequencerDrive : nullptr;
        };
        sequencer.Activate = []( entt::registry& registry, const entt::entity entity ) -> bool
        {
            auto* vfx = registry.try_get<ECS::VFXComponent>( entity );
            if ( vfx == nullptr )
                return false;
            vfx->Data.AutoActivate = true;
            vfx->RequestRestart    = true;
            return true;
        };

        Graphic::System::RegisterSceneFXSystem(
             []( Graphic::SceneRenderer* renderer, const std::shared_ptr<Graphic::Framebuffer>& target )
                  -> std::shared_ptr<Graphic::System::ISceneFXSystem>
             { return std::make_shared<Graphic::System::ParticleRenderer>( renderer, target ); } );
    }
} // namespace Desert::VFX
