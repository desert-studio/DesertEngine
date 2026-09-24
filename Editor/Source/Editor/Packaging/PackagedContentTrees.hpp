#pragma once

#include <Common/Content/DerivedDataCache.hpp>
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
        // The project's asset tree: raw mesh sources are dropped and every texture asset is packed in its
        // cooked form (StageCookedTextureAsset) -- the editor-only sources stay on the packaging machine.
        bool                         StripRawMeshSources;
        // Where the files are READ from when packing. Equal to *Tree for every tree but one: the packager's
        // own cook output (Saved/Cooked/<Platform>) is packed under the key of the directory the runtime
        // LOOKS in (COOKED_PATH), because a DDC reader's fallback is DDC::PackagedPath — COOKED_PATH/<rel>.
        std::filesystem::path Source;
    };

    // AssetsRoot the regenerated .deproj declares. Opening it in the packaged game remaps ASSETS_PATH
    // to <package>/Assets/, which is why that tree's PakKey is this string and not its dev-time path.
    inline constexpr const char* kPackagedAssetsRoot = "Assets";

    inline std::array<PackagedTree, 6> PackagedContentTrees()
    {
        namespace P = Common::Constants::Path;
        std::array<PackagedTree, 6> trees = { {
             // Project assets, raw mesh sources stripped — the runtime reads cooked meshes only.
             { &P::ASSETS_PATH, kPackagedAssetsRoot, /*StripRawMeshSources=*/true },
             { &P::COOKED_PATH, "Cooked", false },
             // Engine resources are never remapped, so their keys ARE their dev-time relative paths.
             { &P::SHADERDIR_PATH, "Resources/Shaders", false },
             { &P::FONTS_PATH, "Resources/Fonts", false },
             { &P::ICONS_PATH, "Resources/Icons", false },
             // The packager's cook of the DerivedDataCache (PackageCook.cpp StageCookedEntries).
             { &P::COOKED_PATH, "Cooked", false, Common::DDC::PlatformCookedDir() },
        } };
        for ( PackagedTree& tree : trees )
            if ( tree.Source.empty() )
                tree.Source = *tree.Tree;
        return trees;
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
