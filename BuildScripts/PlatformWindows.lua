-- Windows-wide workspace settings.

filter "system:windows"
    architecture "x64"
    systemversion "latest"
    -- NOMINMAX workspace-wide: <windows.h> defines min/max as MACROS, which then mangle any
    -- `std::max(...)` / `std::numeric_limits<T>::max()` reached through a later include — reflect-cpp's
    -- Result.hpp and Literal.hpp break this way (C2589 "'(' illegal token on right side of '::'").
    -- It was previously worked around with a #define in individual .cpp files, which only holds until
    -- the next translation unit picks up the same headers in a different order.
    defines { "NOMINMAX" }

    -- /utf-8 sets BOTH the source and the execution charset to UTF-8. Without it MSVC reads a source
    -- file in the machine's ANSI codepage and re-encodes narrow literals into it, so "aé中" comes out a
    -- different number of bytes than it went in — which is exactly how Utf8.DecodesThreeAndFourByteSequences
    -- failed on Windows and nowhere else. It is not only the tests: this codebase writes non-ASCII in
    -- comments and literals throughout, and every one of them is at the mercy of whatever codepage the
    -- build machine happens to have. Clang and GCC are UTF-8 by default, which is why this only ever
    -- showed on MSVC.
    buildoptions { "/utf-8" }

-- /utf-8 makes the BUILD UTF-8; this manifest makes the RUNNING process UTF-8 (Windows 10 1903+): the
-- process's ANSI codepage becomes CP_UTF8, so argv, getenv, fopen and every Win32 *A function speak the
-- same UTF-8 the engine's strings already are. Without it argv arrives in the machine's ANSI codepage:
-- DesertCtl forwarded a scenario's Cyrillic frame name «кадр_bistro_day» as invalid UTF-8 and the editor
-- refused the request as "not JSON". Every executable gets it, so no tool or editor reads argv differently.
filter { "system:windows", "kind:ConsoleApp or WindowedApp" }
    files { path.join( _MAIN_SCRIPT_DIR, "BuildScripts/Windows/Utf8CodePage.manifest" ) }

filter {}
