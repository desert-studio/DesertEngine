#include <Engine/Graphic/MaterialPipelineStates.hpp>

namespace Desert::Graphic
{
    auto MaterialPipelineRequests::Get() -> MaterialPipelineRequests&
    {
        static MaterialPipelineRequests requests;
        return requests;
    }
}
