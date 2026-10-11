#pragma once

// THE SOURCE TREES A CENSUS SCANS, IN ONE PLACE.
//
// Every census used to carry its own copy of { "Desert/Desert/Source", "Desert/Common/Source", ... }. When the UI
// framework left the engine for its own libraries (UI-FW-4: Desert/UI, Desert/Render2DCore, Desert/CoreReflection)
// some forty copies went on scanning the old tree and stopped seeing the code that had moved -- green, because a
// census over a tree that no longer holds the subject finds nothing to refuse.
//
// So the library trees are not listed at all: they are every `Desert/<Project>/Source` directory of the checkout.
// A new library under Desert/ is scanned by every census the day it is created, and a census adds only the trees
// outside Desert/ that are its own business (Editor/Source, Runtime/Source, Tools, Desert/Tests).

#include "scratch_dir.hpp"

#include <algorithm>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

namespace Desert::TestSupport
{
    // "Desert/<Project>/Source" for every library project of the checkout, Common included, sorted.
    inline const std::vector<std::string>& LibraryRoots()
    {
        static const std::vector<std::string> roots = []
        {
            std::vector<std::string> found;
            std::error_code          ec;
            for ( const auto& project : std::filesystem::directory_iterator( RepositoryRoot() / "Desert", ec ) )
                if ( project.is_directory() && std::filesystem::is_directory( project.path() / "Source" ) )
                    found.push_back( "Desert/" + project.path().filename().generic_string() + "/Source" );
            std::sort( found.begin(), found.end() );
            return found;
        }();
        return roots;
    }

    // LibraryRoots() without Desert/Common/Source: the engine and the libraries it is built on -- what a census
    // that used to name only Desert/Desert/Source means by "the engine".
    inline const std::vector<std::string>& EngineRoots()
    {
        static const std::vector<std::string> roots = []
        {
            std::vector<std::string> engine;
            for ( const std::string& root : LibraryRoots() )
                if ( root != "Desert/Common/Source" )
                    engine.push_back( root );
            return engine;
        }();
        return roots;
    }

    // LibraryRoots() followed by @p others (repository-relative).
    inline std::vector<std::string> LibraryRootsAnd( std::initializer_list<const char*> others )
    {
        std::vector<std::string> roots = LibraryRoots();
        roots.insert( roots.end(), others.begin(), others.end() );
        return roots;
    }

    // EngineRoots() followed by @p others (repository-relative).
    inline std::vector<std::string> EngineRootsAnd( std::initializer_list<const char*> others )
    {
        std::vector<std::string> roots = EngineRoots();
        roots.insert( roots.end(), others.begin(), others.end() );
        return roots;
    }

    // @p roots as absolute paths under @p repository.
    inline std::vector<std::filesystem::path> Under( const std::filesystem::path&    repository,
                                                     const std::vector<std::string>& roots )
    {
        std::vector<std::filesystem::path> paths;
        for ( const std::string& root : roots )
            paths.push_back( repository / root );
        return paths;
    }
} // namespace Desert::TestSupport
