#include "PanelRequests.hpp"

namespace Desert::Editor::Core
{
    auto PanelRequests::Pending() -> std::unordered_map<std::string, Action>&
    {
        static std::unordered_map<std::string, Action> s_Pending;
        return s_Pending;
    }
}
