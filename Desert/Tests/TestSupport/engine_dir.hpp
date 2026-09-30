#pragma once

// THE HOST STEP, TAKEN THE WAY THE EDITOR TAKES IT (UE: FPaths::EngineDir). The engine resolves its own resources —
// the shader root and the shading models in it, fonts, icons, the engine content — against the engine directory,
// which a host sets once with Common::Constants::Path::SetEngineDir (the editor from its executable's position or
// --engine-dir). A suite is a host: it sets the directory the build baked in (DESERT_TEST_ENGINE_DIR, an absolute
// path from Desert/Tests/premake5.lua) and never moves the working directory. A suite built without the define does
// not compile — there is no guessed root.

#include <Common/Core/Constants.hpp>

#include <filesystem>

namespace Desert::TestSupport
{
    // The checkout's engine directory (Editor/), absolute.
    inline std::filesystem::path EngineDir()
    {
        return std::filesystem::path( DESERT_TEST_ENGINE_DIR );
    }

    // Points the engine at the checkout's engine directory for this scope and restores the previous one after, so
    // a test that points it at a private copy (a staged package, a scratch shader root) cannot leak into the next.
    class EngineDirScope
    {
    public:
        EngineDirScope()
            : EngineDirScope( EngineDir() )
        {
        }

        explicit EngineDirScope( const std::filesystem::path& engineDir )
            : m_Previous( Common::Constants::Path::EngineDir() )
        {
            Common::Constants::Path::SetEngineDir( engineDir );
        }

        ~EngineDirScope()
        {
            Common::Constants::Path::SetEngineDir( m_Previous );
        }

        EngineDirScope( const EngineDirScope& )            = delete;
        EngineDirScope& operator=( const EngineDirScope& ) = delete;
        EngineDirScope( EngineDirScope&& )                 = delete;
        EngineDirScope& operator=( EngineDirScope&& )      = delete;

    private:
        std::filesystem::path m_Previous;
    };
} // namespace Desert::TestSupport
