#include <Engine/Core/ModuleHooks.hpp>

// ENGINE'S OWN STARTUP (UE: the engine module's StartupModule). Registers Engine's world systems and the
// LandscapeCore descriptor hooks (a module below Engine). Feature modules above Engine are started by the
// executable that links them (Desert/Launch/LinkedModules.cpp), never named here: a lower DLL names nothing
// above it. Run at static initialisation and again (a no-op) from Scene and from LinkedModules.cpp.

namespace Desert::LandscapeCore
{
    void StartupModule();
} // namespace Desert::LandscapeCore

namespace Desert::Core
{
    void StartupEngineModules()
    {
        static const bool started = []
        {
            RegisterEngineWorldSystems();
            Desert::LandscapeCore::StartupModule(); // the landscape world-partition descriptor hooks
                                                    // (BUILD-MOD-6L)
            return true;
        }();
        (void)started;
    }

    namespace
    {
        [[maybe_unused]] const bool kStarted = ( StartupEngineModules(), true );
    } // namespace
} // namespace Desert::Core
