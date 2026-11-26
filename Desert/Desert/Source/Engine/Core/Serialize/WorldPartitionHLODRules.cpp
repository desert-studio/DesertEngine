#include <Engine/Core/Serialize/WorldPartitionHLODRules.hpp>

namespace Desert::Core::Rules::Detail
{
    std::vector<HLODBuilderComponent>& HLODBuilderComponentList()
    {
        static std::vector<HLODBuilderComponent> rows;
        return rows;
    }
}
