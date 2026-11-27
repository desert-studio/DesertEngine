#include <Engine/ECS/System/MeshECSSystem.hpp>

#include <atomic>

namespace Desert::ECS
{
    void MeshECSSystem::WarnCustomShaderIsmOnce()
    {
        static std::atomic<bool> s_WarnedCustomISM{ false };
        if ( !s_WarnedCustomISM.exchange( true ) )
        {
            LOG_WARN( "Instanced Static Mesh doesn't support custom-shader materials "
                      "(instancing is a lit-path optimization) — the sections using one "
                      "are not drawn. Assign a lit material." );
        }
    }
} // namespace Desert::ECS
