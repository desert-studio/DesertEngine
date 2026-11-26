// THE SCRIPT LANGUAGE MODULE'S STARTUP (UE: a script plugin's StartupModule). Registers into the hooks Engine owns
// (Engine/Core/ModuleHooks.hpp) what Engine reaches the scripts through without naming this module: the world
// system that runs a played world's scripts (ECS::ScriptSystem, found by Engine as Core::ScriptHostSystem), the
// reader of a script file's SaveGame property names, and the gameplay debugger's Script category. Called once by
// the executable that links the module (Desert/Launch/LinkedModules.cpp), after Engine's own startup.

#include <Engine/Core/ModuleHooks.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/WorldSystems.hpp>
#include <Engine/ECS/System/ScriptSystem.hpp>
#include <Engine/Scripting/ScriptProperty.hpp>

#include <Common/Core/DevInstruments.hpp>

#if DESERT_DEV_INSTRUMENTS
#include <Engine/GameplayDebugger/GameplayDebuggerCategoryScript.hpp>
#endif

#include <memory>

namespace Desert::ScriptLuau
{
    void StartupModule()
    {
        Core::ModuleHookLists& hooks = Core::ModuleHooks();
        // Think 10: the scripts decide before GameMode (Think 20) sees what they killed this frame.
        hooks.WorldSystems.push_back( { "Script", Core::WorldStage::Think, 10, Core::kPlayedWorlds, true, false,
                                        []( Core::Scene& scene, const Core::WorldServices& services )
                                        { scene.AddSystem<ECS::ScriptSystem>( &scene, services.Assets ); } } );
        hooks.ScriptSaveGameNames = &Scripting::ReadScriptSaveGameProperties;
#if DESERT_DEV_INSTRUMENTS
        hooks.DebuggerCategories.push_back( { 3, []() -> std::unique_ptr<Debug::IGameplayDebuggerCategory> {
                                                 return std::make_unique<Debug::GameplayDebuggerCategoryScript>();
                                             } } );
#endif
    }
} // namespace Desert::ScriptLuau
