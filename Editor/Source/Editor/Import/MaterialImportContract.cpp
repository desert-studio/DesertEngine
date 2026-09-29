#include <Editor/Import/MaterialImportContract.hpp>

#include <algorithm>
#include <format>
#include <vector>

namespace Desert::Editor
{
    namespace
    {
        bool Takes( const SourceMaterial& material, const Common::Content::ShaderManifest& manifest )
        {
            if ( !manifest.DeclaresImport )
                return false;
            if ( !std::ranges::all_of( manifest.ImportRequires,
                                       [&]( const std::string& key ) { return material.Has( key ); } ) )
                return false;
            return material.Entries.empty() ||
                   std::ranges::any_of( manifest.Import, [&]( const Common::Content::ShaderImportRow& row )
                                        { return material.Has( row.SourceKey ); } );
        }

        std::string JoinNames( const std::vector<std::string>& names )
        {
            std::string out;
            for ( const auto& n : names )
                out += ( out.empty() ? "" : ", " ) + n;
            return out.empty() ? std::string( "none" ) : out;
        }
    } // namespace

    Common::ResultStr<std::size_t> ChooseImportTemplate( const SourceMaterial&           material,
                                                         std::span<const ImportTemplate> templates,
                                                         std::string_view                sourcePath )
    {
        std::vector<std::size_t> best;
        std::size_t              bestNeed = 0;
        for ( std::size_t i = 0; i < templates.size(); ++i )
        {
            if ( !Takes( material, templates[i].Manifest ) )
                continue;
            const std::size_t need = templates[i].Manifest.ImportRequires.size();
            if ( best.empty() || need > bestNeed )
            {
                best     = { i };
                bestNeed = need;
            }
            else if ( need == bestNeed )
                best.push_back( i );
        }
        if ( best.empty() )
        {
            std::vector<std::string> keys;
            for ( const auto& entry : material.Entries )
                keys.push_back( entry.first );
            return Common::MakeError<std::size_t>( std::format(
                 "[Import] material '{}' in '{}': no template's Import contract takes it (source keys: {})",
                 material.Name, sourcePath, JoinNames( keys ) ) );
        }
        if ( best.size() == 1 )
            return Common::MakeSuccess( best.front() );

        const auto isDefault = [&]( std::size_t i ) { return templates[i].Manifest.DefaultSurface; };
        if ( std::ranges::count_if( best, isDefault ) == 1 )
            return Common::MakeSuccess( *std::ranges::find_if( best, isDefault ) );
        std::vector<std::string> names;
        for ( const std::size_t i : best )
            names.push_back( templates[i].ShaderName );
        return Common::MakeError<std::size_t>( std::format(
             "[Import] material '{}' in '{}': templates {} take it equally and none is 'Default Surface'",
             material.Name, sourcePath, JoinNames( names ) ) );
    }
} // namespace Desert::Editor
