#pragma once

#include <Common/Core/Constants.hpp>
#include <Common/Project/ProjectFormat.hpp>

#include <array>
#include <filesystem>
#include <string>

namespace Desert::Editor
{
    // THE census of what a packaged game is made of. Both packaging entry points (PackageGame and
    // BuildContentPak) iterate THIS list — never a hand-typed sequence of per-tree calls —
    // because the hand-typed sequence is exactly how fonts and icons were left out: Constants.hpp
    // declared FONTS_PATH and ICONS_PATH, the runtime services scanned them, and the packager packed
    // three other trees. A packaged game then had no .ttf at all and the first frame with text died
    // trying to read one.
    //
    // The relation this list carries is asserted by Desert/Tests/Editor/PackagedContent: every root
    // the font/icon services scan (Engine/Runtime/Services/ServiceScanRoots.hpp) must be a tree in
    // this census, and every never-remapped resource tree's PakKey must be the tree's own relative
    // path — that equality is what makes a runtime lookup of e.g. "Resources/Fonts/Roboto.ttf"
    // resolve to the archive key the packager wrote.
    struct PackagedTree
    {
        const std::filesystem::path* Tree;   // the live constant — follows SetProjectRoot remaps
        const char*                  PakKey; // archive key prefix the tree's files are stored under
        bool                         StripRawMeshSources;
    };

    // AssetsRoot the regenerated .deproj declares. Opening it in the packaged game remaps ASSETS_PATH
    // to <package>/Assets/, which is why that tree's PakKey is this string and not its dev-time path.
    inline constexpr const char* kPackagedAssetsRoot = "Assets";

    inline std::array<PackagedTree, 5> PackagedContentTrees()
    {
        namespace P = Common::Constants::Path;
        return { {
             // Project assets, raw mesh sources stripped — the runtime reads cooked meshes only.
             { &P::ASSETS_PATH, kPackagedAssetsRoot, /*StripRawMeshSources=*/true },
             { &P::COOKED_PATH, "Cooked", false },
             // Engine resources are never remapped, so their keys ARE their dev-time relative paths.
             { &P::SHADERDIR_PATH, "Resources/Shaders", false },
             { &P::FONTS_PATH, "Resources/Fonts", false },
             { &P::ICONS_PATH, "Resources/Icons", false },
        } };
    }

    // THE DESCRIPTOR A PACKAGE SHIPS, derived from the project's own rather than copied verbatim: the
    // packed content lives under `kPackagedAssetsRoot` whatever the dev tree called it, so a
    // DefaultScene that pointed inside the OLD assets root has to be rebased onto the new one or the
    // shipped game boots to a path that is not in the archive.
    //
    // Everything else travels UNCHANGED, and that is a fix rather than an incidental: this used to
    // build a fresh ProjectFile out of three fields, so Description, EngineVersion and every key
    // another build owns (ForeignKeys) were dropped by the act of packaging. A package is the same
    // product, not a reduced one.
    //
    // Pure — a ProjectFile in, a ProjectFile out, no filesystem — so the rebasing rule is assertable
    // without building a package (Desert/Tests/Editor/PackagedContent).
    inline Common::Project::ProjectFile PackagedDescriptor( const Common::Project::ProjectFile& source )
    {
        Common::Project::ProjectFile packaged = source;
        packaged.AssetsRoot                   = kPackagedAssetsRoot;
        if ( !packaged.DefaultScene.empty() && !source.AssetsRoot.empty() &&
             packaged.DefaultScene.rfind( source.AssetsRoot, 0 ) == 0 )
        {
            packaged.DefaultScene = kPackagedAssetsRoot + packaged.DefaultScene.substr( source.AssetsRoot.size() );
        }
        return packaged;
    }
} // namespace Desert::Editor
