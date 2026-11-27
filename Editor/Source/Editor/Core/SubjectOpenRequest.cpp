#include "SubjectOpenRequest.hpp"

namespace Desert::Editor::Core
{
    auto SubjectOpenRequests::Pending() -> std::vector<SubjectId>&
    {
        static std::vector<SubjectId> s_Pending;
        return s_Pending;
    }
}
