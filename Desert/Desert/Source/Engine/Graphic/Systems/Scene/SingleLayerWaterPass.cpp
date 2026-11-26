#include <Engine/Graphic/Systems/Scene/SingleLayerWaterPass.hpp>

namespace Desert::Graphic::System
{
    SingleLayerWaterPassFactory& SingleLayerWaterPassFactorySlot()
    {
        static SingleLayerWaterPassFactory factory = nullptr;
        return factory;
    }
}
