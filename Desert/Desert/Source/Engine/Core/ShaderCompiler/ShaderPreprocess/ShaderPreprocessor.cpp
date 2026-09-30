#include <Engine/Core/ShaderCompiler/ShaderPreprocess/ShaderPreprocessor.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>

#include <format>

namespace Desert::Core::Preprocess
{
    // Every shader program is a single-file Desert Shader Language document (Shader "Name" { ... }).
    // The legacy multi-file '#pragma program/use_stage/param/state' format was fully migrated and its
    // parser removed — a non-DSL source is refused, naming the file and pointing at the migration.

    namespace
    {
        Common::ResultStr<DShaderParseResult> ParseNamed( const std::string& source, const std::string& context )
        {
            if ( !DShaderParser::IsDShader( source ) )
                return Common::MakeError<DShaderParseResult>(
                     std::format( "{}: not a DSL shader. The legacy #pragma format is no longer supported — "
                                  "rewrite as Shader \"Name\" {{ ... }}.",
                                  context ) );
            auto parsed = DShaderParser::Parse( source );
            if ( !parsed.IsSuccess() )
                return Common::MakeError<DShaderParseResult>(
                     std::format( "{}: {}", context, parsed.GetError() ) );
            return Common::MakeSuccess( parsed.ExtractValue() );
        }

        // The pass program's metadata out of a parse: same params/domain, the pass's own render state, and
        // no sub-passes of its own (so the ShaderService doesn't recurse when registering).
        Common::ResultStr<Core::Formats::ShaderProgramMeta>
        MetaForPass( const DShaderParseResult& parsed, const std::string& passName, const std::string& context )
        {
            const auto* pass = parsed.FindPass( passName );
            // A MEDIUM-ONLY SHADER HAS NO PASSES, and that is legal: it is a program FRAGMENT compiled into
            // other programs (ShaderProgramMeta::MediumSource); its metadata is the whole of what it has.
            if ( pass == nullptr && parsed.Meta.IsMediumProgram() )
            {
                Core::Formats::ShaderProgramMeta whole = parsed.Meta;
                return Common::MakeSuccess( std::move( whole ) );
            }
            if ( pass == nullptr )
                return Common::MakeError<Core::Formats::ShaderProgramMeta>(
                     std::format( "{}: the shader has no pass named '{}'", context, passName ) );
            Core::Formats::ShaderProgramMeta meta = parsed.Meta;
            meta.State                            = pass->State;
            // The default cell of a surface template IS the default program (IsSurfaceDefaultCell), so it keeps
            // the default program's metadata whole: the two names answer one shader map, byte for byte.
            if ( !passName.empty() && !IsSurfaceDefaultCell( !parsed.Surface.Cells.empty(), passName ) )
                meta.PassNames.clear();
            return Common::MakeSuccess( std::move( meta ) );
        }
    } // namespace

    Common::ResultStr<std::unordered_map<Desert::Core::Formats::ShaderStage, std::string>>
    ShaderPreprocess::PreProcessProgramPass( const std::string& source, const std::filesystem::path& basePath,
                                             const std::string& passName )
    {
        auto preprocessed = PreProcessPass( source, basePath, passName );
        if ( !preprocessed.IsSuccess() )
            return Common::MakeError<std::unordered_map<Core::Formats::ShaderStage, std::string>>(
                 preprocessed.GetError() );
        return Common::MakeSuccess( std::move( preprocessed.ExtractValue().Stages ) );
    }

    Common::ResultStr<Core::Formats::ShaderProgramMeta>
    ShaderPreprocess::ParseProgramMetaForPass( const std::string& source, const std::filesystem::path& basePath,
                                               const std::string& passName )
    {
        const std::string context = basePath.generic_string();
        const auto        parsed  = ParseNamed( source, context );
        if ( !parsed.IsSuccess() )
            return Common::MakeError<Core::Formats::ShaderProgramMeta>( parsed.GetError() );
        return MetaForPass( parsed.GetValue(), passName, context );
    }

    Common::ResultStr<ShaderPreprocess::PreprocessedPass>
    ShaderPreprocess::PreProcessPass( const std::string& source, const std::filesystem::path& basePath,
                                      const std::string& passName )
    {
        const std::string context = basePath.generic_string();
        const auto        parsed  = ParseNamed( source, context );
        if ( !parsed.IsSuccess() )
            return Common::MakeError<PreprocessedPass>( parsed.GetError() );
        auto meta = MetaForPass( parsed.GetValue(), passName, context );
        if ( !meta.IsSuccess() )
            return Common::MakeError<PreprocessedPass>( meta.GetError() );
        PreprocessedPass out;
        out.Meta = meta.ExtractValue();
        if ( const auto* pass = parsed.GetValue().FindPass( passName ) )
            out.Stages = pass->Stages;
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Core::Preprocess
