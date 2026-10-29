#include "Version.hpp"

// THE GENERATED HEADER CANNOT BE MISSING, AND THIS FILE MUST NOT SURVIVE ITS ABSENCE.
//
// It used to. The include sat behind `__has_include` with a "0.0.0+unknown" fallback, and that guard
// was not free: ccache's direct mode keys an object on the headers the translation unit ACTUALLY
// included, so a header that appears later never invalidates the cached object. A tree built once
// before the generator had ever run kept reporting 0.0.0 until somebody deleted Version.o by hand —
// and that string is not cosmetic, it reaches the user through engines.json and the launcher sidebar.
//
// Generating the header is now part of the build — the Common project does it before this file can be
// compiled — so it is on disk by the time this TU is compiled, or the build has already failed. The
// guard therefore becomes a diagnostic and never a fallback: it produces no code path, it only says
// what to run. That is the configure step (`cmake --preset <host preset>`, which BuildMacOS.sh and
// BuildWindows.bat run), because a build tree configured before the version hook has no hook to fire.
#if !__has_include( "Version.gen.hpp" )
#error "Version.gen.hpp is missing. The build generates it: re-run cmake --preset <host preset>, then build again."
#endif
#include "Version.gen.hpp"

#include <cstdio>
#include <string>

namespace Common::Version
{
    std::optional<std::uint32_t> CommitCount()
    {
        // The generator emits DESERT_VERSION_COMMITS only when git could supply a count it trusts, so
        // "unknown" is the macro's ABSENCE rather than any particular value of it.
#ifdef DESERT_VERSION_COMMITS
        return DESERT_VERSION_COMMITS;
#else
        return std::nullopt;
#endif
    }

    const char* Full()
    {
        // Composed once, and the initialisation is thread-safe by the language rather than by luck: the
        // previous `static char[96]` guarded by `if ( s_Full[0] == '\0' )` let two threads format into
        // the same buffer at the same time. They would have written the same bytes, but this engine runs
        // a JobSystem and a benign race is still a race — and removing it cost nothing while the
        // function was being rewritten anyway.
        static const std::string s_Full = []() -> std::string
        {
            char text[96] = {};
            if ( const std::optional<std::uint32_t> commits = CommitCount() )
                std::snprintf( text, sizeof( text ), "%s.%u+%s%s", DESERT_VERSION_BASE, *commits,
                               DESERT_VERSION_HASH, DESERT_VERSION_DIRTY ? ".dirty" : "" );
            else
                // No build-number field at all rather than a zero one. "0.1+unknown" cannot be misread
                // as an ancient build the way "0.1.0+unknown" can, and it cannot be parsed into a
                // number that would then lose a comparison it was never able to enter.
                std::snprintf( text, sizeof( text ), "%s+%s%s", DESERT_VERSION_BASE, DESERT_VERSION_HASH,
                               DESERT_VERSION_DIRTY ? ".dirty" : "" );
            return text;
        }();
        return s_Full.c_str();
    }

    const char* Base()
    {
        return DESERT_VERSION_BASE;
    }

    const char* Hash()
    {
        return DESERT_VERSION_HASH;
    }

    const char* Branch()
    {
        return DESERT_VERSION_BRANCH;
    }

    bool Dirty()
    {
        return DESERT_VERSION_DIRTY != 0;
    }
} // namespace Common::Version
