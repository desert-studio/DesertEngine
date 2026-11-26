#include "AssetOpen.hpp"

namespace Desert::Editor::Core
{
    auto AssetFieldRequests::Pending() -> std::vector<AssetFieldRequest>&
    {
        static std::vector<AssetFieldRequest> s_Pending;
        return s_Pending;
    }
}
