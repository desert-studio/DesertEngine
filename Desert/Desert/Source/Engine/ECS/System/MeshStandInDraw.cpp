#include <Engine/ECS/System/MeshStandInDraw.hpp>

namespace Desert::ECS
{
    std::vector<MeshStandInDrawFactory>& MeshStandInDraws()
    {
        static std::vector<MeshStandInDrawFactory> factories;
        return factories;
    }
}
