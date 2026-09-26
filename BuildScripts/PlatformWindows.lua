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

    -- THE STATIC CRT (/MT, /MTd), WORKSPACE-WIDE, BECAUSE THE PLAYER HAS TO START ON A CLEAN MACHINE.
    -- With /MD the packaged Runtime.exe imports VCRUNTIME140.dll, MSVCP140.dll and friends, and a Windows
    -- install that never ran the Visual C++ Redistributable refuses to start it with a system dialog the
    -- game cannot catch or explain. /MT links that code into the executable instead.
    --
    -- WHY EVERY PROJECT AND NOT ONLY THE RUNTIME. All modules in one link must agree on the CRT (MSVC
    -- stamps each object with /FAILIFMISMATCH:RuntimeLibrary=...), and Common, Desert, GLFW, Jolt, Lua,
    -- Optick, MeshOptimizer and ReflectCpp are ONE project each, shared by the Runtime, the Editor, the
    -- tools and every test suite. Giving the Runtime its own CRT would mean a second copy of each of those
    -- projects per configuration; one setting here is the whole change, and the Editor loses nothing by
    -- carrying its CRT inside its own binary. Projects must NOT set `staticruntime` themselves on Windows —
    -- a project-level "off" silently wins over this line (GLFW and ImGuiNodeEditor used to say "off").
    --
    -- PREBUILT LIBRARIES ARE THE ONE THING THIS LINE CANNOT REACH. The Vulkan SDK's shaderc and
    -- spirv-cross .lib files (Desert/Dependencies.lua) were compiled by LunarG with whatever CRT LunarG
    -- chose; if that is /MD the link fails with LNK2038 and those libraries have to be built from source
    -- like Assimp and ReflectCpp. vulkan-1.lib is an import library (no CRT) and is unaffected.
    -- scripts/CI/CheckWindowsDlls.sh is the proof on the shipped binary.
    staticruntime "On"

filter {}
