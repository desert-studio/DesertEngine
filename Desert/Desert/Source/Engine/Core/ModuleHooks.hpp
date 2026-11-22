#pragma once

// WHAT A FEATURE MODULE REGISTERS WITH THE ENGINE AT STARTUP (UE: IModuleInterface::StartupModule registering a
// plugin's subsystems, component classes and asset types; the engine then calls through these, never naming the
// module). The registration point is Engine/Core/Modules.cpp; the lists are read by the engine code named per hook.

#include <entt/entt.hpp>

#include <memory>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
    class AssetRootSet;
} // namespace Desert::Assets

namespace Desert::Core
{
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

    struct ModuleHookLists
    {
        std::vector<ModuleService>   Services;
        std::vector<AssetRootWalker> AssetRoots;
        std::vector<RegistryPrepare> Prepares;
    };

    inline ModuleHookLists& ModuleHooks()
    {
        static ModuleHookLists hooks;
        return hooks;
    }

    /// Starts the feature modules the executable links (idempotent; Engine/Core/Modules.cpp). Scene calls it,
    /// which links that file in.
    void StartupEngineModules();
} // namespace Desert::Core
