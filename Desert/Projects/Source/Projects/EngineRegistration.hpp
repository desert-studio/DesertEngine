#pragma once

#include <Common/Core/ResultStr.hpp>

#include <string>

namespace Desert::Project
{
    // Records THIS engine build in `~/.desertengine/engines.json`, so the launcher can find it.
    //
    // The launcher's only route to an engine today is `DESERT_ROOT`, exported by a run script that
    // lives in the ENGINE's repository. After L3 the launcher is a different repository with a
    // different launch path, and that variable is not set for it; a launcher started from a `.app`
    // would have no way at all to know where an engine is. So the engine writes down where it is,
    // once per start, and the launcher reads it.
    //
    // Idempotent by root: registering the same tree again refreshes its version rather than adding
    // a line (Common::Engine::RegisterInstall owns that rule, and both hosts go through it).
    //
    // Returns the reason on failure instead of logging one. The caller is the process entry point
    // and can say it on stderr before there is a window to say it in — an engine that could not
    // register is an engine the launcher will not list, which is a thing the person starting it
    // needs to be told rather than a line in a log they are not reading.
    //
    // `configDirectory` is passed in rather than read from ProjectContext, and that is not a style
    // choice: reaching for the process-wide config directory would make this function reachable by
    // no test that is not willing to write into the developer's real `~/.desertengine`. As a
    // parameter it needs only Common, so the shared-format suite compiles it and points it at a
    // temp directory.
    [[nodiscard]] Common::BoolResultStr RegisterThisEngine( const std::string& configDirectory,
                                                            const std::string& engineRoot );
} // namespace Desert::Project
