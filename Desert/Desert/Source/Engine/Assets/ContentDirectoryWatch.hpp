#pragma once

#include <Engine/Assets/ContentRegistry.hpp>

#include <Common/Content/ContentKinds.hpp>

#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace Desert::Assets
{
    // WHICH CONTENT FILES APPEARED OR WENT AWAY SINCE THE LAST LOOK — by directory modification time, so a
    // poll that finds nothing moved stats directories and opens none of them.
    //
    // WHY DIRECTORIES AND NOT FILES. Hot reload already watches the files it holds loaded objects for, and
    // an in-place edit of one of those reaches the registry through `ContentRegistry::Update` from there.
    // What no loaded object can report is a file that has no object yet: one dropped into a content folder
    // or written by a tool outside the editor. Adding or removing an entry moves the mtime of the directory
    // that holds it (POSIX and NTFS alike), which is UE's DirectoryWatcher question asked by polling: only a
    // directory that moved is listed, and only the files in it that are new or gone go to `Update`.
    //
    // The first `Poll` records the baseline — every directory under the content roots and nothing else — and
    // reports nothing: what is on disk at that moment is what `Gather` just read.
    class ContentDirectoryWatch
    {
    public:
        // Returns how many files were handed to `ContentRegistry::Update`.
        std::size_t Poll()
        {
            if ( !m_HasBaseline )
            {
                for ( const std::filesystem::path& root : Roots() )
                    RecordTree( root );
                m_HasBaseline = true;
                return 0;
            }

            std::vector<std::filesystem::path> moved;
            for ( auto& [dir, time] : m_Directories )
            {
                std::error_code ec;
                const auto      now = std::filesystem::last_write_time( dir, ec );
                if ( ec || now == time )
                    continue;
                time = now;
                moved.emplace_back( dir );
            }

            std::size_t updated = 0;
            for ( const std::filesystem::path& dir : moved )
                updated += Relist( dir );
            return updated;
        }

    private:
        static std::set<std::filesystem::path> Roots()
        {
            std::set<std::filesystem::path> roots;
            for ( const Common::Content::ContentKindSpec& spec : Common::Content::ContentKinds() )
            {
                if ( !spec.StatedOnly() && !spec.Root->empty() )
                    roots.insert( spec.Root->lexically_normal() );
            }
            return roots;
        }

        void RecordTree( const std::filesystem::path& root )
        {
            std::error_code ec;
            if ( !std::filesystem::is_directory( root, ec ) )
                return;
            RecordDirectory( root );
            for ( std::filesystem::recursive_directory_iterator it( root, ec ), end; !ec && it != end;
                  it.increment( ec ) )
            {
                if ( it->is_directory( ec ) )
                    RecordDirectory( it->path() );
            }
        }

        void RecordDirectory( const std::filesystem::path& dir )
        {
            std::error_code ec;
            const auto      time = std::filesystem::last_write_time( dir, ec );
            if ( !ec )
                m_Directories.emplace( dir.lexically_normal().generic_string(), time );
        }

        // Lists ONE directory that moved: every content file in it with no row is new, every row that names
        // a file in it that is gone was removed, and a new subdirectory is recorded with whatever it holds.
        std::size_t Relist( const std::filesystem::path& dir )
        {
            std::size_t     updated = 0;
            std::error_code ec;

            const bool exists = std::filesystem::is_directory( dir, ec );
            if ( exists )
            {
                for ( std::filesystem::directory_iterator it( dir, ec ), end; !ec && it != end;
                      it.increment( ec ) )
                {
                    const std::filesystem::path& entry = it->path();
                    if ( it->is_directory( ec ) )
                    {
                        if ( !m_Directories.contains( entry.lexically_normal().generic_string() ) )
                            updated += AdoptTree( entry );
                        continue;
                    }
                    if ( ContentRegistry::KindForFile( entry ) && !ContentRegistry::HasRow( entry ) )
                    {
                        ContentRegistry::Update( entry );
                        ++updated;
                    }
                }
            }

            for ( std::size_t k = 0; k < Common::Content::CONTENT_KIND_COUNT; ++k )
            {
                for ( const ContentRegistry::PickerRow& row :
                      ContentRegistry::Rows( static_cast<Common::Content::ContentKind>( k ) ) )
                {
                    if ( row.Path.parent_path().lexically_normal() != dir.lexically_normal() )
                        continue;
                    if ( std::filesystem::exists( row.Path, ec ) )
                        continue;
                    ContentRegistry::Update( row.Path );
                    ++updated;
                }
            }
            return updated;
        }

        // A directory that arrived whole (copied in, unzipped): record it and every directory under it, and
        // enter every content file it holds.
        std::size_t AdoptTree( const std::filesystem::path& dir )
        {
            RecordTree( dir );
            std::size_t     updated = 0;
            std::error_code ec;
            for ( std::filesystem::recursive_directory_iterator it( dir, ec ), end; !ec && it != end;
                  it.increment( ec ) )
            {
                if ( it->is_regular_file( ec ) && ContentRegistry::KindForFile( it->path() ) &&
                     !ContentRegistry::HasRow( it->path() ) )
                {
                    ContentRegistry::Update( it->path() );
                    ++updated;
                }
            }
            return updated;
        }

        std::unordered_map<std::string, std::filesystem::file_time_type> m_Directories;
        bool                                                             m_HasBaseline = false;
    };
} // namespace Desert::Assets
