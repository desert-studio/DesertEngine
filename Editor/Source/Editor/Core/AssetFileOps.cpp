#include "AssetFileOps.hpp"

#include <Common/Content/ExternalEntitiesFolder.hpp>
#include <Common/Content/ImportRecord.hpp>
#include <Common/Core/Constants.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Editor::AssetFileOps
{
    namespace fs = std::filesystem;

    namespace
    {
        // A SOURCE'S IMPORT RECORD GOES WHERE THE SOURCE GOES (FIX8): it is the imported asset's identity, and a
        // record left behind would give the moved source a new identity at its next import. A copy does NOT take
        // it - a duplicate is a new asset and its first import writes its own.
        bool MoveRecordWith( const fs::path& from, const fs::path& to, std::string& error )
        {
            const fs::path  record = Common::Content::ImportRecordPathFor( from );
            std::error_code ec;
            if ( !fs::is_regular_file( record, ec ) )
                return true;
            const fs::path target = Common::Content::ImportRecordPathFor( to );
            fs::rename( record, target, ec );
            if ( ec )
            {
                error =
                     "The source moved, but its import record '" + record.string() + "' did not: " + ec.message();
                return false;
            }
            return true;
        }

        bool RecordTargetTaken( const fs::path& to, std::string& error )
        {
            std::error_code ec;
            if ( !fs::exists( Common::Content::ImportRecordPathFor( to ), ec ) )
                return false;
            error = "An import record '" + Common::Content::ImportRecordPathFor( to ).string() +
                    "' already exists there.";
            return true;
        }
        bool IsScene( const fs::path& path )
        {
            return path.extension() == Common::Constants::Extensions::SCENE_EXTENSION;
        }

        // A SCENE'S COMPANION FOLDERS GO WHERE THE SCENE GOES (WP-ASSET-OPS): (from, to) for each folder of
        // Common::Content::SceneCompanionDirectoriesOf(from) that is on disk; none for a file that is not a scene.
        std::vector<std::pair<fs::path, fs::path>> CompanionPairs( const fs::path& from, const fs::path& to )
        {
            std::vector<std::pair<fs::path, fs::path>> pairs;
            if ( !IsScene( from ) )
                return pairs;
            const auto      fromDirs = Common::Content::SceneCompanionDirectoriesOf( from );
            const auto      toDirs   = Common::Content::SceneCompanionDirectoriesOf( to );
            std::error_code ec;
            for ( std::size_t i = 0; i < fromDirs.size(); ++i )
                if ( fs::is_directory( fromDirs[i], ec ) )
                    pairs.emplace_back( fromDirs[i], toDirs[i] );
            return pairs;
        }

        // A copy's name is free when neither the file nor a companion folder named after it is on disk: a stale
        // entity folder left under that name would be adopted by the copy (and refused as unlisted files).
        bool NameTaken( const fs::path& candidate )
        {
            std::error_code ec;
            if ( fs::exists( candidate, ec ) )
                return true;
            if ( !IsScene( candidate ) )
                return false;
            for ( const fs::path& companion : Common::Content::SceneCompanionDirectoriesOf( candidate ) )
                if ( fs::exists( companion, ec ) )
                    return true;
            return false;
        }

        // UE's duplicate of a World Partition map (its external actor packages are duplicated with it): `src`
        // and every companion folder of it, ALL OR NOTHING - a failure removes what this copy made and names
        // the folder that did not copy.
        bool CopyWithCompanions( const fs::path& src, const fs::path& dst, std::string& error )
        {
            std::error_code ec;
            fs::copy( src, dst, fs::copy_options::recursive, ec );
            if ( ec )
            {
                error = ec.message();
                return false;
            }
            std::vector<fs::path> made = { dst };
            for ( const auto& [from, to] : CompanionPairs( src, dst ) )
            {
                fs::create_directories( to.parent_path(), ec );
                if ( !ec )
                    fs::copy( from, to, fs::copy_options::recursive, ec );
                if ( !ec )
                {
                    made.push_back( to );
                    continue;
                }
                error = std::format( "'{}' did not copy with its scene to '{}': {}; nothing was copied",
                                     from.string(), to.string(), ec.message() );
                std::error_code undo;
                fs::remove_all( to, undo );
                for ( const fs::path& path : made )
                    fs::remove_all( path, undo );
                return false;
            }
            return true;
        }
    } // namespace

    std::string UniqueName( const std::string& stem, const std::string& ext,
                            const std::function<bool( const std::string& )>& exists )
    {
        const std::string first = stem + ext;
        if ( !exists( first ) )
            return first;
        for ( int i = 2; i < 100000; ++i )
        {
            const std::string candidate = std::format( "{} {}{}", stem, i, ext );
            if ( !exists( candidate ) )
                return candidate;
        }
        return first; // unreachable in practice
    }

    bool Move( const std::string& src, const std::string& destDir, std::string& outNewPath, std::string& error )
    {
        const fs::path s = src;
        std::error_code ec;
        if ( !fs::exists( s, ec ) )
        {
            error = "Source no longer exists.";
            return false;
        }
        if ( fs::equivalent( s.parent_path(), fs::path( destDir ), ec ) )
        {
            error = "Already in this folder.";
            return false;
        }

        const fs::path dst = fs::path( destDir ) / s.filename();
        if ( fs::exists( dst, ec ) )
        {
            error = "A file with that name already exists here.";
            return false;
        }
        if ( RecordTargetTaken( dst, error ) )
            return false;

        fs::rename( s, dst, ec );
        if ( ec )
        {
            // Cross-device (e.g. moving off a mounted volume): rename fails, so copy then delete.
            std::error_code copyEc;
            fs::copy( s, dst, fs::copy_options::recursive, copyEc );
            if ( copyEc )
            {
                error = copyEc.message();
                return false;
            }
            fs::remove_all( s, ec );
        }
        outNewPath = dst.string();
        return MoveRecordWith( s, dst, error );
    }

    bool Rename( const std::string& src, const std::string& newFileName, std::string& outNewPath,
                 std::string& error )
    {
        if ( newFileName.empty() )
        {
            error = "Name cannot be empty.";
            return false;
        }
        const fs::path  s   = src;
        const fs::path  dst = s.parent_path() / newFileName;
        std::error_code ec;
        if ( fs::exists( dst, ec ) && !fs::equivalent( s, dst, ec ) )
        {
            error = "A file with that name already exists.";
            return false;
        }
        if ( !fs::equivalent( s, dst, ec ) && RecordTargetTaken( dst, error ) )
            return false;
        fs::rename( s, dst, ec );
        if ( ec )
        {
            error = ec.message();
            return false;
        }
        outNewPath = dst.string();
        return MoveRecordWith( s, dst, error );
    }

    bool Duplicate( const std::string& src, std::string& outNewPath, std::string& error )
    {
        const fs::path  s = src;
        std::error_code ec;
        if ( !fs::exists( s, ec ) )
        {
            error = "Source no longer exists.";
            return false;
        }
        const std::string name = UniqueName( s.stem().string(), s.extension().string(), [&]( const std::string& n )
                                             { return NameTaken( s.parent_path() / n ); } );
        const fs::path    dst  = s.parent_path() / name;
        if ( !CopyWithCompanions( s, dst, error ) )
            return false;
        outNewPath = dst.string();
        return true;
    }

    bool CopyInto( const std::string& src, const std::string& destDir, std::string& outNewPath,
                   std::string& error )
    {
        const fs::path  s = src;
        std::error_code ec;
        if ( !fs::exists( s, ec ) )
        {
            error = "Source no longer exists.";
            return false;
        }
        const std::string name = UniqueName( s.stem().string(), s.extension().string(), [&]( const std::string& n )
                                             { return NameTaken( fs::path( destDir ) / n ); } );
        const fs::path dst = fs::path( destDir ) / name;
        if ( !CopyWithCompanions( s, dst, error ) )
            return false;
        outNewPath = dst.string();
        return true;
    }

    bool Delete( const std::string& path, std::string& error )
    {
        std::error_code ec;
        // A scene's companion folders first, while the scene still names them; the `__ExternalEntities__` folder
        // the entity folder leaves empty goes too (only when empty).
        if ( IsScene( path ) )
            for ( const fs::path& companion : Common::Content::SceneCompanionDirectoriesOf( path ) )
            {
                fs::remove_all( companion, ec );
                if ( ec )
                {
                    error = std::format( "'{}' (a folder of the scene) was not deleted: {}", companion.string(),
                                         ec.message() );
                    return false;
                }
                if ( companion.parent_path().filename() == Common::Content::kExternalEntitiesFolder )
                    fs::remove( companion.parent_path(), ec ); // only when empty
            }
        fs::remove_all( path, ec );
        if ( ec )
        {
            error = ec.message();
            return false;
        }
        // The deleted source's identity goes with it; a source imported later under the same name is new.
        fs::remove( Common::Content::ImportRecordPathFor( path ), ec );
        return true;
    }
} // namespace Desert::Editor::AssetFileOps
