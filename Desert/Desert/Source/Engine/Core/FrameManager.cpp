#include <Engine/Core/FrameManager.hpp>

template <>
std::unique_ptr<Desert::Engine::FrameManager>& Common::Singleton<Desert::Engine::FrameManager>::Instance()
{
    static std::unique_ptr<Desert::Engine::FrameManager> instance;
    return instance;
}
