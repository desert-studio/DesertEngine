#include <Engine/Core/ModuleHooks.hpp>

// THE FEATURE MODULES ENGINE STARTS (UE: the module manager loading the enabled plugins and calling their
// StartupModule). Engine names only each module's startup function; everything a module adds to the engine goes
// through the hooks it registers there (ModuleHooks.hpp, PhysicsSceneFeature.hpp, MeshStandInDraw.hpp,
// ComponentEventFacts.hpp). Run at static initialisation and again (a no-op) from Scene, whose reference links
// this file into every executable that has a Scene.

namespace Desert::Destruction
{
    void StartupModule();
} // namespace Desert::Destruction

namespace Desert::LandscapeCore
{
    void StartupModule();
}
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

namespace Desert::Core
{
    void StartupEngineModules()
    {
        static const bool started = []
        {
            Destruction::StartupModule();
            // Before Water: the landscape's collector precedes the water zones' (SceneRenderCollectors.cpp).
            Desert::LandscapeCore::StartupModule(); // the landscape world-partition descriptor hooks
                                                    // (BUILD-MOD-6L)
            Landscape::StartupModule();
            VFX::StartupModule();
            Water::StartupModule();
            return true;
        }();
        (void)started;
    }

    namespace
    {
        [[maybe_unused]] const bool kStarted = ( StartupEngineModules(), true );
    } // namespace
} // namespace Desert::Core
