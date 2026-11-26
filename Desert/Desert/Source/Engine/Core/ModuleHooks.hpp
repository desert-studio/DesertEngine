#pragma once

// WHAT A FEATURE MODULE REGISTERS WITH THE ENGINE AT STARTUP (UE: IModuleInterface::StartupModule registering a
// plugin's subsystems, component classes and asset types; the engine then calls through these, never naming the
// module). The registration point is Engine/Core/Modules.cpp; the lists are read by the engine code named per hook.

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>
#include <Common/Core/Math/Ray.hpp>
#include <Engine/Core/WorldSystems.hpp>

#include <Common/Core/Entt.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Assets
{
    class AssetBase;
    class AssetManager;
    class AssetRootSet;
} // namespace Desert::Assets

namespace Desert::Core::Rules
{
    class HLODBuilderRegistry;
} // namespace Desert::Core::Rules

namespace Desert::Debug
{
    class IGameplayDebuggerCategory;
} // namespace Desert::Debug

namespace Desert::Core
{
    class Scene;

    /// A module's asset service: bound to the project's AssetManager and cleared with the others
    /// (Runtime::ResourceRegistry::BindAssetManager / Clear).
    struct ModuleService
    {
        void ( *Bind )( const std::weak_ptr<Assets::AssetManager>& assets ) = nullptr;
        void ( *Clear )()                                                    = nullptr;
    };
    /// The assets a module's components name: marked as roots of the loaded scene (CollectAssetRoots).
    using AssetRootWalker = void ( * )( entt::registry& registry, Assets::AssetRootSet& roots );
    /// A module's component pools, prepared before the first parallel group (Scene's registry setup).
    using RegistryPrepare = void ( * )( entt::registry& registry );

    /// A module's asset type named by a stored reference (AssetResolver): the asset created at @p file, not read.
    struct AssetAnnouncer
    {
        const char* Type = "";
        std::uint64_t ( *Announce )( Assets::AssetManager& assets, const std::filesystem::path& file ) = nullptr;
        /// A reference stored BY GUID (the handle IS HandleForGuid of the header GUID): whether the asset is
        /// already on record under @p handle; otherwise AssetResolver finds its content-registry row and announces
        /// that file. Null: the type is stored by path only.
        bool ( *IsRecorded )( const Assets::AssetManager& assets, std::uint64_t handle ) = nullptr;
    };
    /// A module's hot-reloaded assets (Runtime/AssetHotReload.cpp): Watch adds its loaded assets to the probe's
    /// watched files, Poll re-reads those whose file @p changed says changed.
    struct HotReloadPoller
    {
        void ( *Watch )( Assets::AssetManager&                            assets,
                         std::vector<std::shared_ptr<Assets::AssetBase>>& watched )          = nullptr;
        void ( *Poll )( Assets::AssetManager&                                      assets,
                        const std::function<bool( const std::filesystem::path& )>& changed ) = nullptr;
    };

    /// A module's per-scene state (UE UWorldSubsystem / FNiagaraWorldManager): one per Scene, made by the scene's
    /// constructors in hook order, ticked once per scene update with the scene's clock (the editor's delta in
    /// Edit, the gameplay delta in Play, nothing while paused), reset when Play begins or ends, cleared with the
    /// scene's entities. The module reaches its own through Scene::GetModuleState( slot ), slot = its index here.
    class ISceneModuleState
    {
    public:
        virtual ~ISceneModuleState()                                                = default;
        virtual void Tick( Scene& scene, entt::registry& registry, double seconds ) = 0;
        virtual void Reset()                                                        = 0;
        virtual void Clear()                                                        = 0;
    };
    /// Makes a scene's state over the scene's registry, which outlives it.
    using SceneModuleStateMaker = std::unique_ptr<ISceneModuleState> ( * )( entt::registry& registry );

    /// The assets a module's asset names (Assets/AssetEviction.cpp's reachability walk): Visit calls @p visitGuid
    /// with each asset-GUID text the asset under @p handle stores, when that asset is the module's type.
    struct AssetReferenceVisitor
    {
        void ( *Visit )( Assets::AssetManager& assets, std::uint64_t handle,
                         const std::function<void( const std::string& guidText, const char* why )>& visitGuid ) =
             nullptr;
    };
    /// A material parameter naming a module's asset by kind (DShader `Asset(<Kind>)`): the label the renderer
    /// matches by (the terrain's layer inputs match a `LandscapeLayerInfoAsset` by its layer name,
    /// Graphic/Systems/Scene/Terrain/TerrainLayerInputs.cpp). Label is null while the asset is not read.
    struct AssetParamLabel
    {
        std::string_view AssetKind;
        const std::string* ( *Label )( std::uint64_t handle ) = nullptr;
    };

    /// A surface a scene raycast meets beside the entities' meshes (Scene::Raycast / RaycastComplex; UE: a
    /// landscape's collision answering the scene query): the hit names the entity the surface belongs to.
    struct RaySurfaceHit
    {
        Common::UUID Entity;
        glm::vec3    Point{ 0.0f };
        glm::vec3    Normal{ 0.0f, 1.0f, 0.0f };
        float        Distance = 0.0f;
    };
    /// One module's surfaces as they are at one moment: the nearest hit nearer than @p limit among the entities
    /// @p accept takes (an empty accept takes all). It borrows the module's tables, so it lives no longer than
    /// that moment.
    using RaySurfaceTrace = std::function<std::optional<RaySurfaceHit>(
         const Common::Math::Ray& ray, float limit, const std::function<bool( const Common::UUID& )>& accept )>;
    /// A module's raycastable surfaces, gathered once for a moment (Scene::GatherRaycastSurfaces); an empty
    /// trace when it has none. The landscape's drawn tiles are the first.
    using RaySurfaceGather = RaySurfaceTrace ( * )( entt::registry& registry );

    /// A module's HLOD builders (Core/HLODMeshBuilder.cpp's HLODBuilders; UE: a plugin's UHLODBuilder
    /// classes): added to the process's registry when it is first made.
    using HLODBuilderAdder = void ( * )( Rules::HLODBuilderRegistry& builders );

    /// After a scene is read and every pass is done (prefab instances included): a module checks what it owns
    /// across entities (SceneSerializer::Deserialize; UE: a plugin's actors' PostLoad) - the landscape refuses a
    /// tile its root cannot hold.
    using SceneLoadCheck = std::function<void( const Scene& scene, std::string_view sceneName )>;
    /// Before a scene is written: a module writes the files the scene names beside it (UE: a level's external
    /// packages saved with it; the landscape's tile heights) and returns why it failed, which stops the save
    /// before the scene points at a file that did not land (SceneSerializer::Serialize).
    using SceneSaveWriter =
         std::function<std::optional<std::string>( Scene& scene, const std::filesystem::path& scenePath )>;

    /// A module's gameplay-debugger category (UE: IGameplayDebugger::RegisterCategory from a module's
    /// StartupModule): every GameplayDebugger makes one, at its digit Key among the engine's own (1 Basic,
    /// 2 Physics, 4 Rendering, 5 WorldPartition). Read only where DESERT_DEV_INSTRUMENTS builds the debugger.
    struct GameplayDebuggerCategoryMaker
    {
        std::uint8_t Key = 0;
        std::unique_ptr<Debug::IGameplayDebuggerCategory> ( *Make )() = nullptr;
    };
    /// The script language's reader of a script file's SaveGame property names (Core::ScriptFileSaveGameLookup;
    /// UE: the script class's SaveGame-flagged properties).
    using ScriptSaveGameReader = Common::ResultStr<std::vector<std::string>> ( * )( const std::string& scriptPath );

    struct ModuleHookLists
    {
        std::vector<ModuleService>   Services;
        std::vector<AssetRootWalker> AssetRoots;
        std::vector<RegistryPrepare> Prepares;
        std::vector<AssetAnnouncer>       AssetTypes;
        std::vector<WorldSystemRegistration> WorldSystems; // Core/WorldSystems.hpp
        std::vector<SceneModuleStateMaker> SceneStates;
        std::vector<HotReloadPoller>       HotReloads;
        std::vector<AssetReferenceVisitor> AssetReferences;
        std::vector<AssetParamLabel>       AssetParamLabels;
        std::vector<HLODBuilderAdder>      HLODBuilders;
        std::vector<RaySurfaceGather>      RaySurfaces;
        std::vector<SceneLoadCheck>        SceneLoadChecks;
        std::vector<SceneSaveWriter>       SceneSaveWriters;
        /// A module's shared tables brought up to date serially before each parallel system group, so the
        /// group's readers only read them (Scene's update; the landscape's tile table).
        std::vector<RegistryPrepare> BeforeParallelGroup;
        std::vector<GameplayDebuggerCategoryMaker> DebuggerCategories;
        /// One per process: set by the script language module's StartupModule; SaveGame refuses a script
        /// lookup while it is unset.
        ScriptSaveGameReader ScriptSaveGameNames = nullptr;
    };

    ModuleHookLists& ModuleHooks();

    /// Starts the feature modules the executable links (idempotent; Engine/Core/Modules.cpp). Scene calls it,
    /// which links that file in.
    void StartupEngineModules();
} // namespace Desert::Core
