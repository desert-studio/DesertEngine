// THE MODULES AN EXECUTABLE LINKS (UE: the target's module list, whose StartupModule the module manager calls).
// Compiled into every executable by desert_link_engine (cmake/DesertModules.cmake), beside the header tool's
// generated lists (Reflection.gen.cpp, Components.gen.cpp) that name every module's generated registration. A
// module DLL never names a module above it (cmake/DesertModuleTable.cmake); only the executable, which links
// them all, names each one's startup.
//
// Order: Engine's own startup first (its world systems and the LandscapeCore descriptor hooks it depends on),
// then each feature module, which registers into the hooks Engine owns (Engine/Core/ModuleHooks.hpp). Run at
// static initialisation of the executable, so every hook list is filled before main and before the first Scene;
// the hook lists live until process exit, so there is no shutdown step.

namespace Desert::Core
{
    void StartupEngineModules();
} // namespace Desert::Core

namespace Desert::Destruction
{
    void StartupModule();
} // namespace Desert::Destruction

namespace Desert::Landscape
{
    void StartupModule();
} // namespace Desert::Landscape

namespace Desert::VFX
{
    void StartupModule();
} // namespace Desert::VFX

namespace Desert::Water
{
    void StartupModule();
} // namespace Desert::Water

namespace
{
    bool StartupLinkedModules()
    {
        Desert::Core::StartupEngineModules();
        Desert::Destruction::StartupModule();
        Desert::Landscape::StartupModule();
        Desert::VFX::StartupModule();
        Desert::Water::StartupModule();
        return true;
    }

    [[maybe_unused]] const bool kLinkedModulesStarted = StartupLinkedModules();
} // namespace
