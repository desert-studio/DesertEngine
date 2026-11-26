#include <Engine/Graphic/Systems/RayTracing/RayTracingSbtWrites.hpp>

namespace Desert::Graphic::RayTracing
{
    auto RayTracingSbtWrites::Get() -> State&
    {
        static State state;
        return state;
    }
}
