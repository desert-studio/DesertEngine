#include <Common/Core/DestructorGuard.hpp>
#include "ShaderIncluder.hpp"

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>

#include <format>
#include <memory>
#include <string>
#include <utility>

namespace Desert::Core
{
    namespace
    {
        // THE OWNER OF ONE INCLUDE RESULT'S BYTES. shaderc is handed raw pointers into these two strings
        // and reads them until ReleaseInclude, so this object is what keeps them alive — and it is a named
        // type rather than a std::pair because "the source name and the content, owned together" is the
        // whole invariant, and a pair says nothing about which member is which.
        //
        // NOTHING HERE MAY MOVE AFTER THE POINTERS ARE TAKEN. That is the defect this replaces: the
        // previous code captured c_str() and then moved the strings, which was valid only while they were
        // long enough to live on the heap. The strings land here first, once, and the pointers are taken
        // from their final home.
        struct IncludeBytes
        {
            std::string Name;
            std::string Content;
        };
    } // namespace

    ShaderIncluder::ShaderIncluder( const Common::Filepath& basePath, ShaderVariant variant )
         : m_BasePath( basePath ), m_Variant( std::move( variant ) )
    {
    }

    ShaderIncluder::~ShaderIncluder()
    try
    {
        // SAID OUT LOUD RATHER THAN ASSERTED. shaderc's contract is to release every result it is given,
        // so this is zero after every compile — but if it ever is not, the bytes of that many include
        // bodies have leaked and this object is the only one that can tell. A hard failure here would take
        // the editor down for a leak, which trades a small loss for a total one.
        if ( m_LiveResults != 0 )
        {
            LOG_WARN( "[ShaderIncluder] {} include result(s) were never released ({}). Each one owns the "
                      "bytes of an included header, so that is a leak of exactly that many.",
                      m_LiveResults, m_BasePath.string() );
        }
    }
    DESERT_DESTRUCTOR_GUARD( "~ShaderIncluder" )

    shaderc_include_result* ShaderIncluder::MakeResult( std::string name, std::string content )
    {
        // THE BYTES FIRST, THE POINTERS SECOND, and the order is the fix. `owned` is the only copy from
        // here on: the strings are moved into it, and everything shaderc is told about them is read out of
        // it afterwards.
        auto owned     = std::make_unique<IncludeBytes>();
        owned->Name    = std::move( name );
        owned->Content = std::move( content );

        auto result = std::make_unique<shaderc_include_result>();

        result->source_name        = owned->Name.c_str();
        result->source_name_length = owned->Name.length();
        result->content            = owned->Content.c_str();
        result->content_length     = owned->Content.length();
        result->user_data          = owned.release();

        ++m_LiveResults;
        return result.release();
    }

    shaderc_include_result* ShaderIncluder::GetInclude( const char* requested_source, shaderc_include_type type,
                                                        const char* requesting_source, size_t include_depth )
    {
        DESERT_VERIFY( include_depth < 32, "Shader include recursion detected" );

        std::filesystem::path fullPath;

        if ( type == shaderc_include_type_relative )
        {
            // #include "file"
            std::filesystem::path baseDir = std::filesystem::path( requesting_source ).parent_path();

            fullPath = ( baseDir / requested_source ).lexically_normal();
        }
        else
        {
            // #include <file>
            std::filesystem::path shaderRoot = Common::Constants::Path::SHADERDIR_PATH;

            fullPath = ( shaderRoot / requested_source ).lexically_normal();

            // THE VARIANT IS ASKED BEFORE THE FILE SYSTEM, and only for an angle include: an angle path
            // is written against the shader root and is therefore the same string for every file that
            // includes it, which is what makes it addressable by a caller. A quoted include is relative
            // to whoever wrote it and names nothing stable, so it is never substituted.
            //
            // Line-preserving translation still applies, because a substituted body is Desert shader
            // text like any other and generated code uses the same layout sugar.
            const std::string requested =
                 std::filesystem::path( requested_source ).lexically_normal().generic_string();
            if ( const std::string* substituted = m_Variant.Find( requested ) )
                return MakeResult( fullPath.string(), Preprocess::DShaderParser::TranslateSugar( *substituted ) );
        }

        // FileSystem is VFS-aware: shader includes resolve from disk in dev and from the mounted
        // .dpak in a packaged game.
        if ( !Common::Utils::FileSystem::Exists( fullPath ) )
        {
            return CreateErrorIncludeResult( std::format( "Cannot open include file: {}", fullPath.string() ) );
        }

        // Exists() passed but the read can still fail (racing delete, truncated pak): that used to
        // inline an EMPTY header silently — now it is the same named refusal as an unknown include.
        auto rawInclude = Common::Utils::FileSystem::ReadFileContent( fullPath );
        if ( !rawInclude )
        {
            return CreateErrorIncludeResult( rawInclude.GetError() );
        }

        // Translate the Desert layout sugar so shared `.glslh` headers can use the SAME vocabulary as the
        // stage blocks (the compiler inlines includes AFTER stage assembly, so headers must be translated
        // here). Line-preserving, so #line-based include error mapping stays exact.
        return MakeResult( fullPath.string(),
                           Preprocess::DShaderParser::TranslateSugar( rawInclude.ExtractValue() ) );
    }

    shaderc_include_result* ShaderIncluder::CreateErrorIncludeResult( const std::string& error )
    {
        // shaderc's convention for a failed include: an EMPTY source name, and the message as the content.
        // It goes through the same owner as a successful one — the message used to be a `new std::string`
        // that nothing ever deleted, so every unresolved include leaked its own diagnostic.
        return MakeResult( std::string{}, error );
    }

    void ShaderIncluder::ReleaseInclude( shaderc_include_result* data )
    {
        if ( !data )
            return;

        // UNCONDITIONALLY, and that is a second small fix. This used to free nothing at all unless
        // `user_data` was non-null, so a result without one leaked the result itself — and after the
        // change above every result HAS one, which makes the old guard a guard against a state that can
        // no longer happen while still being able to leak.
        delete static_cast<IncludeBytes*>( data->user_data );
        delete data;

        if ( m_LiveResults > 0 )
            --m_LiveResults;
    }
} // namespace Desert::Core
