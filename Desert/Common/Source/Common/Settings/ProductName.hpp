#pragma once

#include <string>
#include <string_view>

namespace Common::Settings
{
    // What a product name becomes when it is also a DIRECTORY NAME.
    const char* const kDefaultProductDirectoryName = "DesertGame";

    // THE ONE RULE for turning a `.deproj` Name into a single path component (PKG1c). The packager names
    // the package folder with it and GameUserDirectory names the player's per-user folder with it, so a
    // game's two folders cannot disagree about its name. The rule is Windows', the strictest of the
    // three hosts, because a package built on macOS is shipped to Windows players and the per-user folder
    // must be creatable on every machine the same project runs on:
    //   - `< > : " / \ | ? *` and control characters (below 0x20, and 0x7F) become `_`;
    //   - trailing dots and spaces are removed (Windows strips them, so "Game." and "Game" collide);
    //   - empty, "." and ".." become kDefaultProductDirectoryName;
    //   - a reserved device name (CON, PRN, AUX, NUL, COM1-9, LPT1-9), in any case and with or without an
    //     extension, gets `_` after its stem ("con.txt" -> "con_.txt"), since opening one opens the device.
    // Everything else — spaces inside the name, Unicode — is kept: it is the name the player sees.
    std::string SanitizeProductName( std::string_view inName );
} // namespace Common::Settings
