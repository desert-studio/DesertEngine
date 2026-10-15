#include <Engine/VFX/VFXModuleAsset.hpp>

#include <Common/Content/ShaderAssetHeader.hpp>
#include <Common/Content/TextAssetHeader.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>

namespace Desert::VFX
{
    namespace
    {
        struct StageName
        {
            std::string_view Word;
            VFXModuleStage   Stage;
        };

        constexpr std::array kStageNames{
             StageName{ "EmitterUpdate", VFXModuleStage::EmitterUpdate },
             StageName{ "ParticleSpawn", VFXModuleStage::ParticleSpawn },
             StageName{ "ParticleUpdate", VFXModuleStage::ParticleUpdate },
        };

        std::string_view Trim( std::string_view s )
        {
            const auto first = s.find_first_not_of( " \t\r" );
            if ( first == std::string_view::npos )
                return {};
            const auto last = s.find_last_not_of( " \t\r" );
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
        if ( stated.Kind != kVFXModuleKindName )
            return Common::MakeFormattedError<Result>( "{}: the header states Kind '{}', not '{}'", where,
                                                       stated.Kind, kVFXModuleKindName );
        const auto version = stated.Versions.find( std::string( kVFXModuleSchemaTagName ) );
        if ( version == stated.Versions.end() || version->second != kVFXModuleSchemaVersion )
            return Common::MakeFormattedError<Result>(
                 "{}: the header states {} v{}; this engine reads v{} only", where, kVFXModuleSchemaTagName,
                 version == stated.Versions.end() ? 0u : version->second, kVFXModuleSchemaVersion );
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
            {
                if ( module.Stages != 0 )
                    return Common::MakeFormattedError<Result>( "{}: a second Stage line", where );
                std::istringstream words{ std::string( rest ) };
                std::string        stage;
                while ( words >> stage )
                {
                    const auto it = std::find_if( kStageNames.begin(), kStageNames.end(),
                                                  [&]( const StageName& s ) { return s.Word == stage; } );
                    if ( it == kStageNames.end() )
                        return Common::MakeFormattedError<Result>(
                             "{}: Stage '{}' is not EmitterUpdate, ParticleSpawn or ParticleUpdate", where,
                             stage );
                    module.Stages |= static_cast<uint8_t>( it->Stage );
                }
                if ( module.Stages == 0 )
                    return Common::MakeFormattedError<Result>( "{}: the Stage line names no stage", where );
            }
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
        if ( module.Stages == 0 )
            return Common::MakeFormattedError<Result>( "{}: module '{}' states no Stage", where,
                                                       module.DisplayName );
        if ( module.Category.empty() )
            return Common::MakeFormattedError<Result>( "{}: module '{}' states no Category", where,
                                                       module.DisplayName );

        const std::string declarations( pos < inside.size() ? inside.substr( pos ) : std::string_view{} );
        auto              block = ParseModuleBlock( declarations, where, !module.IsCpuModule() );
        if ( !block )
            return Common::MakeError<Result>( block.GetError() );
        module.Block = block.ExtractValue();
        if ( module.IsCpuModule() && ( !module.Block.Attributes.empty() || !Trim( module.Block.Body ).empty() ||
                                       module.AllowsStage( VFXModuleStage::ParticleUpdate ) ) )
            return Common::MakeFormattedError<Result>( "{}: module '{}' is a CPU module (Stage EmitterUpdate): it "
                                                       "declares Inputs only - no Attribute, no "
                                                       "GLSL, no ParticleUpdate stage",
                                                       where, module.DisplayName );
        return Common::MakeSuccess( std::move( module ) );
    }

    std::string WriteVFXModule( const VFXModuleDefinition& module )
    {
        Common::Content::TextAssetHeaderSerialized header;
        header.Kind = std::string( kVFXModuleKindName );
        header.Guid = Common::Content::AssetGuidToText( module.Guid );
        header.Versions.emplace( std::string( kVFXModuleSchemaTagName ), kVFXModuleSchemaVersion );

        std::string out = Common::Content::WriteShaderHeaderLine( header );
        std::format_to( std::back_inserter( out ), "Module \"{}\"\n{{\n    Stage", module.DisplayName );
        for ( const StageName& s : kStageNames )
            if ( module.AllowsStage( s.Stage ) )
                std::format_to( std::back_inserter( out ), " {}", s.Word );
        std::format_to( std::back_inserter( out ), "\n    Category {}\n", module.Category );
        if ( !module.Description.empty() )
            std::format_to( std::back_inserter( out ), "    Description \"{}\"\n", module.Description );
        for ( const VFXModuleDecl& a : module.Block.Attributes )
            std::format_to( std::back_inserter( out ), "    Attribute {} {}\n", a.Name, GlslTypeName( a.Type ) );
        for ( const VFXModuleDecl& in : module.Block.Inputs )
            std::format_to( std::back_inserter( out ), "    Input {} {}\n", in.Name, GlslTypeName( in.Type ) );
        if ( !module.Block.Body.empty() )
            std::format_to( std::back_inserter( out ), "\n{}", module.Block.Body );
        if ( !out.ends_with( '\n' ) )
            out += '\n';
        out += "}\n";
        return out;
    }
} // namespace Desert::VFX
