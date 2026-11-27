#include <Engine/Core/Serialize/WorldPartitionActivationCost.hpp>

namespace Desert::Core::Rules::Detail
{
    std::vector<ActivationKindComponent>& ActivationKindComponentList()
    {
        static std::vector<ActivationKindComponent> rows;
        return rows;
    }
}
