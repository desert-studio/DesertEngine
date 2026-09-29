#pragma once

#include <Common/Core/Constants.hpp>

#include <array>
#include <filesystem>

namespace Desert::Runtime
{
    // The roots the font and icon services enumerate when they fill their pickers and resolve saved
    // handles. A header of its own — NOT a detail inside each service's .cpp — for two reasons:
    //
    //   1. The services' headers pull the graphics stack, and the packaging census test must compile
    //      without it. The test asserts that every root listed here is a tree the game packager puts
    //      into Content.dpak (see Editor/Packaging/PackagedContentTrees.hpp) — the fonts and icons
    //      were once scanned but never packaged, so a packaged game drew no text at all.
    //   2. Pointers into Common::Constants::Path, not copies: a project remap (SetProjectRoot) is
    //      followed automatically, and the census can assert IDENTITY with the packager's list
    //      instead of comparing two spellings of one directory.
    inline std::array<const std::filesystem::path*, 2> FontScanRoots()
    {
        // This project's Assets tree (drop a .ttf into the project) plus the shared engine
        // Engine/Content/Fonts built-ins (Roboto, Noto, ...).
        return { &Common::Constants::Path::ASSETS_PATH, &Common::Constants::Path::FONTS_PATH };
    }

    inline std::array<const std::filesystem::path*, 2> IconScanRoots()
    {
        // This project's Assets tree (drop an .svg in) plus the shared engine icon set.
        return { &Common::Constants::Path::ASSETS_PATH, &Common::Constants::Path::ICONS_PATH };
    }
} // namespace Desert::Runtime
