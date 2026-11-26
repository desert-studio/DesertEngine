#include "SceneOpenRequest.hpp"

namespace Desert::Editor::Core
{
    auto SceneOpenRequest::Pending() -> std::optional<std::string>&
    {
        static std::optional<std::string> s_Pending;
        return s_Pending;
    }
}
