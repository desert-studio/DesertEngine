#include <Editor/Core/StartupRefusal.hpp>

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace Desert::Editor
{
    void RefuseToStart( const int exitCode, const std::string& message )
    {
        std::fprintf( stderr, "%s\n", message.c_str() );
        std::fflush( stderr );
#ifdef _WIN32
        OutputDebugStringA( ( message + "\n" ).c_str() );
        if ( GetConsoleWindow() == nullptr )
            MessageBoxA( nullptr, message.c_str(), "Desert Editor could not start", MB_OK | MB_ICONERROR );
#endif
        std::exit( exitCode );
    }
} // namespace Desert::Editor
