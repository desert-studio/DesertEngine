#include <Editor/Import/MaterialImportContract.hpp>

#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>

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

    Common::ResultStr<ImportTemplate> ReadImportTemplate( std::string_view source, std::string locator )
    {
        ImportTemplate out;
        out.Locator         = std::move( locator );
        const auto manifest = Common::Content::ReadShaderManifest( source );
        if ( !manifest )
            return Common::MakeError<ImportTemplate>(
                 std::format( "'{}': {}", out.Locator, manifest.GetError() ) );
        out.Manifest = manifest.GetValue();
        // A shader without an Import block is not a template; the caller skips it unread (legacy-format and
        // engine-internal shaders need not satisfy the DSL parser here).
        if ( !out.Manifest.DeclaresImport )
            return Common::MakeSuccess( std::move( out ) );
        const auto name = Common::Content::ReadShaderDeclaredName( source );
        if ( !name )
            return Common::MakeError<ImportTemplate>( std::format( "'{}': {}", out.Locator, name.GetError() ) );
        out.ShaderName    = name.GetValue();
        const auto header = Common::Content::ReadShaderHeader( source );
        if ( !header )
            return Common::MakeError<ImportTemplate>( std::format( "'{}': {}", out.Locator, header.GetError() ) );
        out.Guid          = header.GetValue().Guid;
        const auto parsed = Core::Preprocess::DShaderParser::Parse( std::string( source ) );
        if ( !parsed )
            return Common::MakeError<ImportTemplate>( std::format( "'{}': {}", out.Locator, parsed.GetError() ) );
        for ( const auto& param : parsed.GetValue().Meta.Params )
            if ( param.IsTexture )
                out.TextureProperties.insert( param.Name );
        return Common::MakeSuccess( std::move( out ) );
    }

    bool ImportedTextureSlot::NeedsPacking() const
    {
        if ( Parts.empty() )
            return false;
        if ( !std::ranges::all_of( Parts, [&]( const ImportedTexturePart& p )
                                   { return p.Source == Parts.front().Source; } ) )
            return true;
        if ( TemplateChannels.empty() )
            return false;
        std::string given;
        for ( const auto& part : Parts )
            given += part.Channels.empty() ? std::string( "rgba" ) : part.Channels;
        return !std::ranges::all_of( TemplateChannels,
                                     [&]( char c ) { return given.find( c ) != std::string::npos; } );
    }

    TemplateFill FillFromTemplate( const SourceMaterial& material, const ImportTemplate& chosen )
    {
        TemplateFill               fill;
        std::set<std::string_view> read( chosen.Manifest.ImportRequires.begin(),
                                         chosen.Manifest.ImportRequires.end() );
        const auto                 slotOf = [&]( const std::string& property ) -> ImportedTextureSlot&
        {
            for ( auto& slot : fill.Textures )
                if ( slot.Slot == property )
                    return slot;
            ImportedTextureSlot slot{ property, {}, {}, {} };
            bool                whole = false;
            for ( const auto& row : chosen.Manifest.Import )
                if ( row.Property == property )
                {
                    whole = whole || row.Channels.empty();
                    for ( const char c : row.Channels )
                        if ( slot.TemplateChannels.find( c ) == std::string::npos )
                            slot.TemplateChannels += c;
                }
            if ( whole )
                slot.TemplateChannels.clear();
            return fill.Textures.emplace_back( std::move( slot ) );
        };
        for ( const auto& row : chosen.Manifest.Import )
        {
            const auto entry = material.Entries.find( row.SourceKey );
            if ( entry == material.Entries.end() )
                continue;
            read.insert( entry->first );
            if ( chosen.TextureProperties.contains( row.Property ) )
            {
                if ( entry->second.Texture )
                {
                    ImportedTextureSlot& slot = slotOf( row.Property );
                    if ( slot.Parts.empty() )
                        slot.Sampler = entry->second.Sampler;
                    slot.Parts.push_back( { *entry->second.Texture, row.Channels } );
                }
            }
            else if ( entry->second.Value && std::ranges::none_of( fill.Params, [&]( const ImportedParam& p )
                                                                   { return p.Name == row.Property; } ) )
                fill.Params.push_back( { row.Property, *entry->second.Value } );
        }
        for ( const auto& [key, entry] : material.Entries )
            if ( !read.contains( key ) )
                fill.UnreadKeys.push_back( key );
        return fill;
    }
} // namespace Desert::Editor
