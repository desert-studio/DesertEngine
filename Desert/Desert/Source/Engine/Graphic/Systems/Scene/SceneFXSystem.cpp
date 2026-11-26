#include <Engine/Graphic/Systems/Scene/SceneFXSystem.hpp>

namespace Desert::Graphic::System
{
    SceneFXSystemFactory& SceneFXSystemFactorySlot()
    {
        static SceneFXSystemFactory factory = nullptr;
        return factory;
    }
}
