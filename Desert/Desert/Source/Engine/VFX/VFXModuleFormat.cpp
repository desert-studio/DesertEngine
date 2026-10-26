#include <Engine/VFX/VFXModuleFormat.hpp>

#include <Engine/Assets/TextAssetHeaderCheck.hpp>

#include <Common/Content/ShaderAssetHeader.hpp>
#include <Common/Content/TextAssetHeader.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>

namespace Desert::VFX
{
    namespace
    {
        std::string_view Trim( std::string_view s )
        {
            const auto first = s.find_first_not_of( " \t\r\n" );
            if ( first == std::string_view::npos )
                return {};
            const auto last = s.find_last_not_of( " \t\r\n" );
            return s.substr( first, last - first + 1 );
        }

        /// `"<text>"` -> text; no escapes (a display string has no quote in it).
        Common::ResultStr<std::string> Quoted( std::string_view s, std::string_view what, std::string_view where )
        {
            s = Trim( s );
            if ( s.size() < 2 || s.front() != '"' || s.back() != '"' ||
                 s.substr( 1, s.size() - 2 ).find( '"' ) != std::string_view::npos )
                return Common::MakeFormattedError<std::string>( "{}: {} '{}' is not one \"quoted\" string", where,
                                                                what, s );
            return Common::MakeSuccess( std::string( s.substr( 1, s.size() - 2 ) ) );
        }
    } // namespace

    Common::ResultStr<VFXModuleDefinition> ParseVFXModule( const std::string_view text,
                                                           const std::string_view where )
    {
        using Result = VFXModuleDefinition;
        VFXModuleDefinition module;

        // 1. The header: kind, generation, GUID.
        const auto header = Common::Content::ReadShaderHeader( text );
        if ( !header )
            return Common::MakeFormattedError<Result>( "{}: {}", where, header.GetError() );
        const Common::Content::TextAssetHeaderSerialized& stated = header.GetValue();
        if ( auto checked = Assets::CheckStatedHeader( stated, Common::Content::ContentKind::VFXModule,
                                                      Assets::kVFXModuleSchemaTag,
                                                      static_cast<int>( Assets::kVFXModuleSchemaVersion ),
                                                      VFXModuleTextSubsystems() );
             !checked )
            return Common::MakeFormattedError<Result>( "{}: VFX module {}", where, checked.GetError() );
        if ( !stated.Dependencies.empty() )
            return Common::MakeFormattedError<Result>(
                 "{}: the header states {} Dependencies; a module references "
                 "no asset",
                 where, stated.Dependencies.size() );
        const auto guid = Common::Content::AssetGuidFromText( stated.Guid );
        if ( !guid || guid.GetValue().IsNull() )
            return Common::MakeFormattedError<Result>( "{}: the header GUID '{}' is not well-formed", where,
                                                       stated.Guid );
        module.Guid = guid.GetValue();

        // 2. `Module "<name>"` past the header and comment lines, then the block: from its `{` to the LAST `}`
        // of the file (the GLSL inside has braces of its own; nothing but whitespace may follow the block).
        const std::size_t moduleAt = text.find( "\nModule " );
        if ( moduleAt == std::string_view::npos )
            return Common::MakeFormattedError<Result>( "{}: no `Module \"<Name>\"` line", where );
        const std::size_t lineEnd = std::min( text.find( '\n', moduleAt + 1 ), text.size() );
        auto name = Quoted( text.substr( moduleAt + 8, lineEnd - moduleAt - 8 ), "Module name", where );
        if ( !name )
            return Common::MakeError<Result>( name.GetError() );
        module.DisplayName = name.ExtractValue();
        if ( module.DisplayName.empty() )
            return Common::MakeFormattedError<Result>( "{}: the Module name is empty", where );
        const std::size_t open  = text.find( '{', lineEnd );
        const std::size_t close = text.rfind( '}' );
        if ( open == std::string_view::npos || close == std::string_view::npos || close < open ||
             !Trim( text.substr( lineEnd, open - lineEnd ) ).empty() || !Trim( text.substr( close + 1 ) ).empty() )
            return Common::MakeFormattedError<Result>( "{}: module '{}' has no `{{ ... }}` block after its name",
                                                       where, module.DisplayName );
        const std::string_view inside = text.substr( open + 1, close - open - 1 );

        // 3. The metadata lines lead the block; the first other line starts the declarations (ParseModuleBlock).
        std::size_t pos = 0;
        while ( pos < inside.size() )
        {
            const std::size_t      end  = std::min( inside.find( '\n', pos ), inside.size() );
            const std::string_view line = Trim( inside.substr( pos, end - pos ) );
            const std::size_t      gap  = line.find_first_of( " \t" );
            const std::string_view word = line.substr( 0, gap );
            const std::string_view rest = gap == std::string_view::npos ? std::string_view{} : line.substr( gap );
            if ( line.empty() || line.starts_with( "//" ) )
            {
            }
            else if ( word == "Stage" )
                return Common::MakeFormattedError<Result>(
                     "{}: `Stage` is VFX-MODULE-ASSET's draft spelling; the stack groups are the `Usage` line", where );
            else if ( word == "Category" )
            {
                module.Category = std::string( Trim( rest ) );
                if ( module.Category.empty() || module.Category.find_first_of( " \t" ) != std::string::npos )
                    return Common::MakeFormattedError<Result>( "{}: Category '{}' is not one id", where,
                                                               module.Category );
            }
            else if ( word == "Description" )
            {
                auto description = Quoted( rest, "Description", where );
                if ( !description )
                    return Common::MakeError<Result>( description.GetError() );
                module.Description = description.ExtractValue();
            }
            else
                break;
            pos = end + 1;
        }
        if ( module.Category.empty() )
            return Common::MakeFormattedError<Result>( "{}: module '{}' states no Category", where,
                                                       module.DisplayName );

        const std::string declarations( pos < inside.size() ? inside.substr( pos ) : std::string_view{} );
        auto              block = ParseModuleBlock( declarations, where );
        if ( !block )
            return Common::MakeError<Result>( block.GetError() );
        module.Block = block.ExtractValue();
        return Common::MakeSuccess( std::move( module ) );
    }

    std::string WriteVFXModule( const VFXModuleDefinition& module )
    {
        std::string out = Common::Content::WriteShaderHeaderLine( Common::Content::MakeTextHeader(
             Common::Content::ContentKind::VFXModule, module.Guid, VFXModuleTextSubsystems() ) );
        std::format_to( std::back_inserter( out ), "Module \"{}\"\n{{\n    Category {}\n", module.DisplayName,
                        module.Category );
        if ( !module.Description.empty() )
            std::format_to( std::back_inserter( out ), "    Description \"{}\"\n", module.Description );
        out += "    Usage";
        for ( const VFXStage stage :
              { VFXStage::EmitterSpawn, VFXStage::EmitterUpdate, VFXStage::ParticleSpawn, VFXStage::ParticleUpdate } )
            if ( module.AllowsStage( stage ) )
                std::format_to( std::back_inserter( out ), " {}", StageName( stage ) );
        out += '\n';
        for ( const VFXModuleDecl& a : module.Block.Attributes )
            std::format_to( std::back_inserter( out ), "    Attribute {} {}\n", a.Name, GlslTypeName( a.Type ) );
        for ( const VFXModuleInputDecl& in : module.Block.Inputs )
        {
            std::format_to( std::back_inserter( out ), "    Input {} {} =", in.Name, GlslTypeName( in.Type ) );
            if ( in.BindingRequired )
                out += " Binding";
            else
                for ( uint32_t c = 0; c < Assets::Serialization::ComponentCount( in.Type ); ++c )
                    std::format_to( std::back_inserter( out ), " {}", in.Default[static_cast<int>( c )] );
            out += '\n';
        }
        // The body carries its own leading blank or comment lines (ParseModuleBlock keeps them with the GLSL).
        out += module.Block.Body;
        if ( !out.ends_with( '\n' ) )
            out += '\n';
        out += "}\n";
        return out;
    }

    Common::BoolResultStr ValidateVFXModule( const VFXModuleDefinition& module )
    {
        const std::string where  = std::format( "VFX module '{}'", module.DisplayName );
        auto              parsed = ParseVFXModule( WriteVFXModule( module ), where );
        if ( !parsed )
            return Common::MakeError<bool>( parsed.GetError() );
        if ( parsed.GetValue() != module )
            return Common::MakeFormattedError<bool>( "{}: its text does not read back as the same module (a name or "
                                                     "description with a quote or a line break?)",
                                                     where );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VFXModuleLibrary::AddDirectory(
         const std::filesystem::path&                                      dir,
         const std::function<std::string( const std::filesystem::path& )>& pathOf )
    {
        std::error_code ec;
        if ( !std::filesystem::is_directory( dir, ec ) )
            return Common::MakeFormattedError<bool>( "VFX module library '{}' is not a directory", dir.string() );
        std::vector<std::filesystem::path> files;
        for ( auto it = std::filesystem::recursive_directory_iterator( dir, ec );
              !ec && it != std::filesystem::recursive_directory_iterator(); it.increment( ec ) )
            if ( it->is_regular_file() && it->path().extension() == kVFXModuleExtension )
                files.push_back( it->path() );
        if ( ec )
            return Common::MakeFormattedError<bool>( "cannot list '{}': {}", dir.string(), ec.message() );
        std::sort( files.begin(), files.end() );
        for ( const std::filesystem::path& file : files )
        {
            std::ifstream      in( file, std::ios::binary );
            std::ostringstream text;
            text << in.rdbuf();
            if ( !in )
                return Common::MakeFormattedError<bool>( "cannot read VFX module '{}'", file.string() );
            auto parsed = ParseVFXModule( text.str(), file.string() );
            if ( !parsed )
                return Common::MakeError<bool>( parsed.GetError() );
            const auto key = std::make_pair( parsed.GetValue().Guid.Hi, parsed.GetValue().Guid.Lo );
            if ( const auto it = m_Modules.find( key ); it != m_Modules.end() )
                return Common::MakeFormattedError<bool>( "VFX modules '{}' and '{}' state the same GUID {}",
                                                         it->second.Path, file.string(),
                                                         Common::Content::AssetGuidToText( parsed.GetValue().Guid ) );
            Set( parsed.ExtractValue(), pathOf( file ) );
        }
        return BOOLSUCCESS;
    }

    void VFXModuleLibrary::Set( VFXModuleDefinition module, std::string path )
    {
        const auto key  = std::make_pair( module.Guid.Hi, module.Guid.Lo );
        m_Modules[key] = Entry{ std::move( module ), std::move( path ) };
    }

    Common::ResultStr<VFXModuleDefinition> VFXModuleLibrary::Resolve( const Common::Content::AssetGuid& guid ) const
    {
        const auto it = m_Modules.find( std::make_pair( guid.Hi, guid.Lo ) );
        if ( it == m_Modules.end() )
            return Common::MakeFormattedError<VFXModuleDefinition>( "no VFX module states GUID {}",
                                                                    Common::Content::AssetGuidToText( guid ) );
        return Common::MakeSuccess( it->second.Module );
    }

    std::vector<Assets::AssetGuidRef> VFXModuleLibrary::List() const
    {
        std::vector<Assets::AssetGuidRef> out;
        for ( const auto& [key, entry] : m_Modules )
            out.push_back( { Common::Content::AssetGuidToText( entry.Module.Guid ), entry.Path } );
        std::sort( out.begin(), out.end(), []( const auto& a, const auto& b ) { return a.Path < b.Path; } );
        return out;
    }

    std::optional<Assets::AssetGuidRef> VFXModuleLibrary::FindByName( const std::string_view name ) const
    {
        std::optional<Assets::AssetGuidRef> found;
        for ( const auto& [key, entry] : m_Modules )
        {
            if ( entry.Module.DisplayName != name )
                continue;
            if ( found )
                return std::nullopt;
            found = Assets::AssetGuidRef{ Common::Content::AssetGuidToText( entry.Module.Guid ), entry.Path };
        }
        return found;
    }
} // namespace Desert::VFX
