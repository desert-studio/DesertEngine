#include "ModuleTable.hpp"

#include <Common/Json/Json.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <utility>

namespace Desert::HeaderTool
{
    namespace
    {
        // <build>/DesertModules.json as cmake/DesertModules.cmake writes it.
        struct ModuleRow
        {
            std::string              Name;
            std::string              Folder;
            std::vector<std::string> Deps;
        };
        struct PlacementRow
        {
            std::string Module;
            std::string Pattern;
        };
        struct TableDocument
        {
            std::string                        RepoRoot;
            std::string                        LegacyRoot;
            std::vector<ModuleRow>             Modules;
            std::vector<PlacementRow>          Placement;
            std::map<std::string, std::string> LegacyFiles;
            std::map<std::string, std::string> LegacyLibraries;
        };
    } // namespace

    Common::ResultStr<ModuleTable> ModuleTable::Load( const std::filesystem::path& tableFile )
    {
        auto read = ::Common::Json::ReadFile<TableDocument>( tableFile );
        if ( !read )
            return Common::MakeError<ModuleTable>( std::format( "module table {}", read.GetError() ) );
        TableDocument document = read.ExtractValue();
        const auto    refuse   = [&]( const std::string& why ) {
            return Common::MakeError<ModuleTable>( std::format( "module table {}: {}", tableFile.string(), why ) );
        };
        if ( document.Modules.empty() || document.Placement.empty() || document.LegacyRoot.empty() ||
             document.RepoRoot.empty() )
            return refuse( "lacks Modules, Placement, LegacyRoot or RepoRoot" );

        ModuleTable table;
        table.m_LegacyRoot = std::move( document.LegacyRoot );
        table.m_RepoRoot   = std::filesystem::path( document.RepoRoot );
        for ( ModuleRow& row : document.Modules )
        {
            if ( table.Find( row.Name ) != nullptr )
                return refuse( std::format( "module '{}' is listed twice", row.Name ) );
            for ( const std::string& dep : row.Deps )
                if ( table.Find( dep ) == nullptr )
                    return refuse(
                         std::format( "'{}' depends on '{}', which is not listed before it", row.Name, dep ) );
            table.m_Modules.push_back( { std::move( row.Name ), std::move( row.Folder ), std::move( row.Deps ) } );
        }
        for ( PlacementRow& row : document.Placement )
        {
            if ( table.Find( row.Module ) == nullptr )
                return refuse( std::format( "placement row names '{}', which is not a module", row.Module ) );
            try
            {
                std::regex regex( row.Pattern, std::regex::ECMAScript );
                table.m_Placement.push_back(
                     { std::move( row.Module ), std::move( row.Pattern ), std::move( regex ) } );
            }
            catch ( const std::regex_error& error )
            {
                return refuse(
                     std::format( "placement row of '{}': {} ({})", row.Module, error.what(), row.Pattern ) );
            }
        }
        table.m_BuildPlacement  = std::move( document.LegacyFiles );
        table.m_LegacyLibraries = std::move( document.LegacyLibraries );
        return Common::MakeSuccess( std::move( table ) );
    }

    const ModuleInfo* ModuleTable::Find( std::string_view name ) const
    {
        const auto it = std::find_if( m_Modules.begin(), m_Modules.end(),
                                      [&]( const ModuleInfo& module ) { return module.Name == name; } );
        return it == m_Modules.end() ? nullptr : &*it;
    }

    std::set<std::string> ModuleTable::Closure( std::string_view name ) const
    {
        std::set<std::string>    seen;
        std::vector<std::string> pending{ std::string( name ) };
        while ( !pending.empty() )
        {
            const ModuleInfo* module = Find( pending.back() );
            pending.pop_back();
            if ( module == nullptr )
                continue;
            for ( const std::string& dep : module->Deps )
                if ( seen.insert( dep ).second )
                    pending.push_back( dep );
        }
        return seen;
    }

    std::string ModuleTable::ModuleOf( std::string_view repoRelative ) const
    {
        const auto under = [&]( std::string_view root )
        {
            return repoRelative.size() > root.size() && repoRelative.starts_with( root ) &&
                   repoRelative[root.size()] == '/';
        };
        for ( const ModuleInfo& module : m_Modules )
            if ( under( module.Folder ) )
                return module.Name;
        if ( under( m_LegacyRoot ) )
            return ModuleOfLegacy( repoRelative.substr( m_LegacyRoot.size() + 1 ) );
        return {};
    }

    std::string ModuleTable::ModuleOfLegacy( std::string_view legacyRelative ) const
    {
        const std::string path( legacyRelative );
        for ( const ModulePlacement& row : m_Placement )
            if ( std::regex_search( path, row.Regex ) )
                return row.Module;
        return {};
    }
} // namespace Desert::HeaderTool
