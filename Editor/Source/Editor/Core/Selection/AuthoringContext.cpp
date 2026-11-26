#include "AuthoringContext.hpp"

namespace Desert::Editor::Core
{
    AuthoringContextHost& ActiveAuthoringContext()
    {
        static AuthoringContextHost s_Host;
        return s_Host;
    }
}
