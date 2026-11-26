#include <Engine/Core/ModuleHooks.hpp>

namespace Desert::Core
{
    ModuleHookLists& ModuleHooks()
    {
        static ModuleHookLists hooks;
        return hooks;
    }
}
