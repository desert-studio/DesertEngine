#pragma once

#include <Common/Core/Constants.hpp>
#include <Common/Core/DeveloperOnlyShaders.hpp>
#include <Common/Project/ProjectFormat.hpp>

#include <algorithm>
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
    // path — that equality is what makes a runtime lookup of e.g. "Engine/Content/Fonts/Roboto.ttf"
    // resolve to the archive key the packager wrote.
    struct PackagedTree
    {
        const std::filesystem::path* Tree;   // the live constant — follows SetProjectRoot remaps
        const char*                  PakKey; // archive key prefix the tree's files are stored under
        // The project's asset tree: raw mesh sources are dropped and every texture asset is staged in its
        // cooked form (StageCookedTextureAsset) -- the editor-only sources stay on the packaging machine.
        bool StripRawMeshSources;
    };

    // AssetsRoot the regenerated .deproj declares. Opening it in the packaged game remaps ASSETS_PATH
    // to <package>/Assets/, which is why that tree's PakKey is this string and not its dev-time path.
    inline constexpr const char* kPackagedAssetsRoot = "Assets";

    // Every tree is STAGED into Saved/Cooked/<Platform>/<PakKey>/ by the cook (GamePackager.cpp
    // StageShippedContent) and the archive is packed from that one tree only — never from these sources.
    inline std::array<PackagedTree, 5> PackagedContentTrees()
    {
        namespace P = Common::Constants::Path;
        return { {
             // Project assets, raw mesh sources stripped — the runtime reads cooked meshes only.
             { &P::ASSETS_PATH, kPackagedAssetsRoot, /*StripRawMeshSources=*/true },
             { &P::COOKED_PATH, "Cooked", false },
             // Engine content (Engine/Content, owner 2026-09-28: a package is built from the project and the
             // engine's runtime content, never from editor content). Keyed by its path under the engine root,
             // so the packaged game — whose engine root is its own folder — reads the same relative spelling.
             // Editor/Content (the editor passes' shaders, the gizmo billboards) is NOT a row: no tree here
             // knows it, so nothing stages, cooks or registers it for a package.
             { &P::SHADERDIR_PATH, "Engine/Content/Shaders", false },
             { &P::FONTS_PATH, "Engine/Content/Fonts", false },
             { &P::ICONS_PATH, "Engine/Content/Icons", false },
        } };
    }

    // Whether @p file lies inside one of the census trees above — the only content a package can carry. Read by
    // the stager, the shader cook and the shipped registry, so the three cannot disagree about what ships: a file
    // outside every tree (Editor/Content above all) is left out by where it lives, not by a list of names.
    inline bool IsInPackagedTree( const std::filesystem::path& file )
    {
        const std::filesystem::path normal = file.lexically_normal();
        return std::ranges::any_of( PackagedContentTrees(),
                                    [&]( const PackagedTree& tree )
                                    {
                                        const std::filesystem::path rel =
                                             normal.lexically_relative( tree.Tree->lexically_normal() );
                                        return !rel.empty() && *rel.begin() != "..";
                                    } );
    }

    // Whether a package whose runtime is (not) built with DESERT_DEV_INSTRUMENTS leaves @p file out: every
    // file outside every census tree, and — for a Shipping runtime only — the shader programs nothing but the
    // developer-instrument pipelines load (Common/Core/DeveloperOnlyShaders.hpp). The stager, the shader
    // cook and the shipped registry all ask this one predicate, so the archive, its SPIR-V cook and the rows
    // the runtime binds cannot disagree about which programs a configuration carries.
    inline bool IsLeftOutOfPackage( const std::filesystem::path& file, bool developerInstruments )
    {
        if ( !IsInPackagedTree( file ) )
            return true;
        return !developerInstruments && file.extension() == ".shader" &&
               Common::IsDeveloperOnlyShaderProgram( file.stem().string() );
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
