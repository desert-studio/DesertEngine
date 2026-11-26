#include <Common/Core/Constants.hpp>

namespace Common::Constants::Path::Detail
{
    namespace
    {
        // The engine's and the project's path roots, held by Common's image alone (BINT-STATE): one object, so
        // every module DLL reads the roots SetEngineDir/SetProjectRoot wrote.
        struct PathRoots
        {
            EngineState  Engine;
            ProjectState Project;
        };

        PathRoots& Roots() noexcept
        {
            static PathRoots roots;
            return roots;
        }
    } // namespace

    EngineState& Engine() noexcept
    {
        return Roots().Engine;
    }

    ProjectState& Project() noexcept
    {
        return Roots().Project;
    }
} // namespace Common::Constants::Path::Detail
