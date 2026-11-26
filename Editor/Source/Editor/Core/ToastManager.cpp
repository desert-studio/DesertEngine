#include "ToastManager.hpp"

namespace Desert::Editor
{
    auto ToastManager::Get() -> ToastManager&
    {
        static ToastManager s_Instance;
        return s_Instance;
    }
}
