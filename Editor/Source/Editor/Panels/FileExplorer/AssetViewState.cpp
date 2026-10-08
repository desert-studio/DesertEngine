#include "AssetViewState.hpp"

#include <Editor/Panels/FileExplorer/ContentBrowserUtils.hpp>
#include <Editor/Panels/FileExplorer/DirectoryInformation.hpp>

#include <algorithm>
#include <filesystem>
#include <string>

namespace Desert::Editor
{
    std::vector<std::size_t> BuildDisplayOrder( const DirectoryInformation* dir, const AssetViewState& state,
                                                bool showHidden )
    {
        std::vector<std::size_t> order;
        if ( !dir )
            return order;
        const auto& children = dir->Children;
        order.reserve( children.size() );

        const std::string search = ContentBrowserUtils::ToLowerCopy( state.SearchBuf );
        for ( std::size_t i = 0; i < children.size(); ++i )
        {
            const auto* c = children[i];
            if ( !showHidden && c->Hidden )
                continue;
            // Type filter (folders always shown so you can still navigate).
            if ( state.TypeFilter >= 0 && c->IsFile && static_cast<int>( c->Type ) != state.TypeFilter )
                continue;
            if ( !search.empty() )
            {
                const std::string name =
                     ContentBrowserUtils::ToLowerCopy( std::filesystem::path( c->AssetPath ).filename().string() );
                if ( name.find( search ) == std::string::npos )
                    continue;
            }
            order.push_back( i );
        }

        using SortMode      = AssetViewState::SortMode;
        const SortMode mode = state.Sort;
        const bool     desc = state.SortDescending;
        // THE SORT KEY IS MADE ONCE PER ENTRY, not twice per comparison: this runs every frame, and building a
        // path and a lower-cased copy inside the comparator was ~n log n allocations a frame — 18 % of the
        // editor thread in a folder of 240 materials once the tiles themselves were cheap (THUMB3, sampled).
        std::vector<std::string> names( children.size() );
        for ( const std::size_t i : order )
            names[i] = ContentBrowserUtils::ToLowerCopy(
                 std::filesystem::path( children[i]->AssetPath ).filename().string() );
        std::sort( order.begin(), order.end(),
                   [&]( std::size_t a, std::size_t b )
                   {
                       const auto* ca = children[a];
                       const auto* cb = children[b];
                       if ( ca->IsFile != cb->IsFile )
                           return !ca->IsFile; // folders always first, regardless of sort
                       int cmp = 0;
                       switch ( mode )
                       {
                           case SortMode::DateModified:
                               cmp = static_cast<int>( ca->LastWriteTime > cb->LastWriteTime ) -
                                     static_cast<int>( ca->LastWriteTime < cb->LastWriteTime );
                               break;
                           case SortMode::Type:
                               cmp = static_cast<int>( ca->Type ) - static_cast<int>( cb->Type );
                               break;
                           case SortMode::Size:
                               cmp = static_cast<int>( ca->FileSize > cb->FileSize ) -
                                     static_cast<int>( ca->FileSize < cb->FileSize );
                               break;
                           case SortMode::Name:
                           default:
                               break;
                       }
                       if ( cmp == 0 ) // Name mode + tiebreak: case-insensitive filename
                           cmp = names[a].compare( names[b] );
                       return desc ? cmp > 0 : cmp < 0;
                   } );
        return order;
    }
} // namespace Desert::Editor
