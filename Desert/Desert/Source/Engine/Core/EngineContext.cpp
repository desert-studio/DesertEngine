#include <Engine/Core/EngineContext.hpp>

template <>
std::unique_ptr<Desert::EngineContext>& Common::Singleton<Desert::EngineContext>::Instance()
{
    static std::unique_ptr<Desert::EngineContext> instance;
    return instance;
}
