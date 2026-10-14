#include <Engine/Core/WorldContext.hpp>

namespace Desert::Core
{
    namespace
    {
        thread_local const WorldContext* t_Current = nullptr;
    }

    const WorldContext* WorldContext::Current()
    {
        return t_Current;
    }

    WorldContext::Scope::Scope( const WorldContext& context ) : m_Previous( t_Current )
    {
        t_Current = &context;
    }

    WorldContext::Scope::~Scope()
    {
        t_Current = m_Previous;
    }
} // namespace Desert::Core
