#pragma once

#include <Common/Core/Constants.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Desert::Editor::Autosave
{
    // WHERE THE EDITOR'S RECOVERY COPIES LIVE: <Project>/Saved/Autosaves/, NEVER UNDER THE ASSETS ROOT.
    //
    // UE keeps autosaves in <Project>/Saved/Autosaves for the reason that bit us: an autosave is not
    // project content. While they lived in <Assets>/Scenes/Autosave/ every content scan saw them — the
    // asset registry folded their GUIDs (a copy of Starter carries Starter's scene ID), the scene-version
    // gate read an old-schema copy as a corpus scene, and six censuses grew a "skip Autosave/" branch. A
    // file that is in the wrong folder is fixed by the folder, not by every reader learning to look away.
    //
    // Saved/ is the machine-local tree (gitignored as a whole, like Saved/Cooked and Saved/Crashes), so
    // nothing here is committed, scanned, cooked or packed.
    //
    // HEADER-ONLY AND DISK-FREE except LatestIn(): the path relation is stated over Constants alone so the
    // AutosavePaths suite can hold it without the editor, a device or the migrator.

    // The two kinds of recovery copy. Both end in "_autosave" — LatestIn() and SceneFor() match on it.
    inline constexpr std::string_view kPeriodicSuffix   = "_autosave";
    inline constexpr std::string_view kDeviceLostSuffix = "_devicelost_autosave";

    // A scene that has never been saved has no path to mirror; its copies go under this folder by name.
    inline constexpr std::string_view kUntitledFolder = "Untitled";

    // <Project>/Saved/Autosaves. With no project open (the sandbox) ProjectDir is "", so this is the
    // working-directory-relative Saved/Autosaves — the same rule Saved/Cooked follows.
    inline std::filesystem::path Dir()
    {
        return ( Common::Constants::Path::CurrentProjectRoot().ProjectDir / "Saved" / "Autosaves" )
             .lexically_normal();
    }

    namespace Detail
    {
        inline std::filesystem::path Absolute( const std::filesystem::path& p )
        {
            std::error_code       ec;
            std::filesystem::path abs = std::filesystem::absolute( p, ec );
            return ( ec ? p : abs ).lexically_normal();
        }

        // `p` relative to `root`, or nullopt when `p` does not lie under it.
        inline std::optional<std::filesystem::path> Under( const std::filesystem::path& root,
                                                           const std::filesystem::path& p )
        {
            const std::filesystem::path rel = Absolute( p ).lexically_relative( Absolute( root ) );
            if ( rel.empty() || *rel.begin() == ".." )
                return std::nullopt;
            return rel;
        }

        inline std::string FileSafe( std::string name )
        {
            for ( auto& ch : name )
                if ( ch == ' ' || ch == '/' || ch == '\\' || ch == ':' )
                    ch = '_';
            return name.empty() ? std::string( "Scene" ) : name;
        }
    } // namespace Detail

    // The recovery copy for the scene open at `scenePath` (empty for a never-saved scene, which is then
    // named by `sceneName`). A scene under the assets root is MIRRORED — <Assets>/Scenes/Levels/A.desce
    // autosaves to Saved/Autosaves/Scenes/Levels/A_autosave.desce — so two scenes with one name in two
    // folders never share a copy, and SceneFor() can answer the way back.
    inline std::filesystem::path PathFor( const std::filesystem::path& scenePath, std::string_view sceneName,
                                          std::string_view suffix )
    {
        const std::string ext( Common::Constants::Extensions::SCENE_EXTENSION );
        if ( !scenePath.empty() )
        {
            if ( const auto rel = Detail::Under( Common::Constants::Path::ASSETS_PATH, scenePath ) )
                return Dir() / rel->parent_path() / ( rel->stem().string() + std::string( suffix ) + ext );
        }
        return Dir() / kUntitledFolder /
               ( Detail::FileSafe( std::string( sceneName ) ) + std::string( suffix ) + ext );
    }

    // THE WAY BACK: the scene a recovery copy stands for, so a recovered scene opens BOUND TO ITS ORIGINAL
    // and Ctrl+S writes the user's file rather than the copy. nullopt = `autosave` is not a recovery copy
    // at all; an EMPTY path = it is the copy of a never-saved scene (Save As picks the file).
    inline std::optional<std::filesystem::path> SceneFor( const std::filesystem::path& autosave )
    {
        const auto rel = Detail::Under( Dir(), autosave );
        if ( !rel || autosave.extension() != Common::Constants::Extensions::SCENE_EXTENSION )
            return std::nullopt;

        std::string stem    = autosave.stem().string();
        bool        matched = false;
        // The longer suffix first: "_devicelost_autosave" also ends in "_autosave".
        for ( const std::string_view suffix : { kDeviceLostSuffix, kPeriodicSuffix } )
        {
            if ( stem.size() > suffix.size() && stem.ends_with( suffix ) )
            {
                stem.resize( stem.size() - suffix.size() );
                matched = true;
                break;
            }
        }
        if ( !matched )
            return std::nullopt;

        if ( *rel->begin() == kUntitledFolder )
            return std::filesystem::path{};
        return Common::Constants::Path::ASSETS_PATH / rel->parent_path() /
               ( stem + std::string( Common::Constants::Extensions::SCENE_EXTENSION ) );
    }

    // The newest recovery copy anywhere under `dir` (recursive: copies mirror the scene tree), or empty.
    inline std::filesystem::path LatestIn( const std::filesystem::path& dir )
    {
        namespace fs = std::filesystem;
        fs::path           newest;
        fs::file_time_type newestTime{};
        std::error_code    ec;
        for ( fs::recursive_directory_iterator it( dir, ec ), end; !ec && it != end; it.increment( ec ) )
        {
            const fs::path& p = it->path();
            if ( !it->is_regular_file( ec ) || p.extension() != Common::Constants::Extensions::SCENE_EXTENSION ||
                 !p.stem().string().ends_with( kPeriodicSuffix ) )
                continue;
            const auto t = fs::last_write_time( p, ec );
            if ( ec )
            {
                ec.clear();
                continue;
            }
            if ( newest.empty() || t > newestTime )
            {
                newest     = p;
                newestTime = t;
            }
        }
        return newest;
    }
} // namespace Desert::Editor::Autosave
