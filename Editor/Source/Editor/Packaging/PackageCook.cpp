#include "PackageCook.hpp"

#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Core/ShaderCompiler/ShaderCacheKey.hpp>
#include <Engine/Core/ShaderCompiler/ShaderCompiler.hpp>
#include <Engine/Core/ShaderCompiler/ShaderPreprocess/ShaderPreprocessor.hpp>
#include <Engine/Core/ShaderCompiler/ShaderSpirvCache.hpp>
#include <Engine/Runtime/Services/ServiceScanRoots.hpp>
#include <Engine/Text/FontCache.hpp>
#include <Engine/Vector/IconBake.hpp>

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>

#include <Editor/Import/TextureImporter.hpp>

#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Desert::Editor
{
    namespace fs = std::filesystem;

    namespace
    {
        // The same enumeration + extension filter AssetPreloader::PreloadShaders uses (lowercased
        // extension over ListFilesRecursive of the live SHADERDIR_PATH): the cook must see exactly
        // the set of shaders the runtime will register, or a shader the runtime compiles at startup
        // is one the cook silently skipped.
        std::vector<fs::path> ShippedShaderFiles()
        {
            std::vector<fs::path> out;
            for ( const auto& candidate :
                  Common::Utils::FileSystem::ListFilesRecursive( Common::Constants::Path::SHADERDIR_PATH ) )
            {
                std::string ext = candidate.extension().string();
                std::transform( ext.begin(), ext.end(), ext.begin(), ::tolower );
                if ( ext == ".shader" )
                    out.push_back( candidate );
            }
            return out;
        }

        void CookShaders( bool spirvDebugInfo, CookStats& stats )
        {
            namespace Preprocess = Core::Preprocess;

            for ( const fs::path& file : ShippedShaderFiles() )
            {
                // Ф3 made the primitive return a ResultStr, so a miss is a named refusal instead of an
                // empty string indistinguishable from an empty file. Policy here is unchanged - count it
                // and move on - but the reason now reaches the log.
                const auto contentRead = Common::Utils::FileSystem::ReadFileContent( file );
                if ( !contentRead || contentRead.GetValue().empty() )
                {
                    ++stats.Failures; // the primitive already logged a miss; an empty file is silent
                    continue;
                }
                const std::string& content = contentRead.GetValue();

                // Pre-check with the parser proper: PreProcessProgramPass aborts (DESERT_VERIFY) on
                // an unparsable file, and a broken .shader must fail THIS shader's cook, not the
                // whole packaging run.
                if ( const auto parsed = Preprocess::DShaderParser::Parse( content ); !parsed.IsSuccess() )
                {
                    LOG_ERROR( "[PackageCook] {} does not parse and was not cooked: {}", file.string(),
                               parsed.GetError() );
                    ++stats.Failures;
                    continue;
                }

                // The default program plus every named pass — the same set ShaderService::Register
                // turns into programs at startup.
                std::vector<std::string> passes = { "" };
                const auto meta = Preprocess::ShaderPreprocess::ParseProgramMetaForPass( content, "" );
                passes.insert( passes.end(), meta.PassNames.begin(), meta.PassNames.end() );

                for ( const std::string& passName : passes )
                {
                    const auto stages =
                         Preprocess::ShaderPreprocess::PreProcessProgramPass( content, file, passName );
                    for ( const auto& [stage, source] : stages )
                    {
                        const uint64_t key =
                             Core::ComputeShaderCacheKeyForProfile( stage, source, file, spirvDebugInfo );
                        if ( Core::TryLoadCachedSpirv( key ) )
                        {
                            ++stats.ShadersCached;
                            continue;
                        }
                        // Compiles under the SAME key (same inputs, same profile) and stores it.
                        if ( !Core::ShaderCompiler::CompileGLSLToSPIRVForProfile( stage, source, file.string(),
                                                                                  spirvDebugInfo )
                                   .IsSuccess() )
                        {
                            ++stats.Failures; // the compiler logged file/stage/diagnostic
                            continue;
                        }
                        // A compile whose artifact did not reach the disk is not a cooked artifact:
                        // the pak would ship nothing under this key and the player would pay the
                        // compile. The store is best-effort for the runtime and mandatory here.
                        if ( !Core::TryLoadCachedSpirv( key ) )
                        {
                            LOG_ERROR( "[PackageCook] {} [{}] compiled but its artifact did not reach {} — "
                                       "the package would ship a shader the runtime must recompile",
                                       file.string(), static_cast<int>( stage ),
                                       Core::SpirvCachePathForKey( key ).string() );
                            ++stats.StoreFailures;
                            continue;
                        }
                        ++stats.ShadersCompiled;
                    }
                }
            }
        }

        void CookFonts( CookStats& stats )
        {
            for ( const fs::path* root : Runtime::FontScanRoots() )
            {
                for ( const auto& p : Common::Utils::FileSystem::ListFilesRecursive( *root ) )
                {
                    if ( p.extension() != ".ttf" ) // FontService::EnsurePreloaded's own filter
                        continue;

                    // Ф3 made the read primitives return a ResultStr, so a miss is a NAMED refusal rather
                    // than an empty vector indistinguishable from an empty file. This site wants the
                    // same policy it always had - count the failure and move on to the next font - but
                    // it now says WHY in the log instead of swallowing the reason.
                    const auto ttfRead = Common::Utils::FileSystem::ReadByteFileContent( p );
                    if ( !ttfRead )
                    {
                        LOG_ERROR( "[PackageCook] '{}' could not be read, not baking it: {}", p.generic_string(),
                                   ttfRead.GetError() );
                        ++stats.Failures;
                        continue;
                    }
                    const auto& ttf = ttfRead.GetValue();
                    if ( ttf.empty() )
                    {
                        LOG_ERROR( "[PackageCook] '{}' is an empty file, not baking it.", p.generic_string() );
                        ++stats.Failures;
                        continue;
                    }

                    // The bake the runtime asks for on a text-bearing first frame: the default size,
                    // ASCII only. A scene using glyphs beyond ASCII re-bakes once at runtime (a
                    // different glyph set is a different key by design); the shipped default covers
                    // the common case and the default font.
                    const uint64_t  key = Text::FontCacheKey( ttf, Text::kDefaultBakePixelHeight, {} );
                    Text::BakedFont existing;
                    if ( Text::TryLoadBakedFont( Text::FontCachePath( key ), existing ) )
                    {
                        ++stats.FontsCached;
                        continue;
                    }

                    const Text::BakedFont baked = Text::BakeFontForCache( ttf, Text::kDefaultBakePixelHeight, {} );
                    if ( !baked.Valid() )
                    {
                        LOG_ERROR( "[PackageCook] {} could not be baked into an SDF atlas", p.string() );
                        ++stats.Failures;
                        continue;
                    }
                    if ( !Text::StoreBakedFont( Text::FontCachePath( key ), baked ) )
                    {
                        LOG_ERROR( "[PackageCook] {} baked but its atlas did not reach {} — the package "
                                   "would ship a font the runtime must rebake",
                                   p.string(), Text::FontCachePath( key ).string() );
                        ++stats.StoreFailures;
                        continue;
                    }
                    ++stats.FontsBaked;
                }
            }
        }

        void CookIcons( CookStats& stats )
        {
            for ( const fs::path* root : Runtime::IconScanRoots() )
            {
                for ( const auto& p : Common::Utils::FileSystem::ListFilesRecursive( *root ) )
                {
                    if ( p.extension() != ".svg" ) // IconService::EnsurePreloaded's own filter
                        continue;

                    const auto svgRead = Common::Utils::FileSystem::ReadByteFileContent( p );
                    if ( !svgRead )
                    {
                        LOG_ERROR( "[PackageCook] '{}' could not be read, not baking it: {}", p.generic_string(),
                                   svgRead.GetError() );
                        ++stats.Failures;
                        continue;
                    }
                    const auto& svg = svgRead.GetValue();
                    if ( svg.empty() )
                    {
                        LOG_ERROR( "[PackageCook] '{}' is an empty file, not baking it.", p.generic_string() );
                        ++stats.Failures;
                        continue;
                    }

                    const uint64_t    key = Vector::IconCacheKey( svg );
                    Vector::BakedIcon existing;
                    if ( Vector::TryLoadBakedIcon( Vector::IconCachePath( key ), existing ) )
                    {
                        ++stats.IconsCached;
                        continue;
                    }

                    const Vector::BakedIcon baked = Vector::BakeIconSdf( svg.data(), svg.size() );
                    if ( !baked.Valid() )
                    {
                        LOG_ERROR( "[PackageCook] {} has no filled shapes the icon importer understands",
                                   p.string() );
                        ++stats.Failures;
                        continue;
                    }
                    if ( !Vector::StoreBakedIcon( Vector::IconCachePath( key ), baked ) )
                    {
                        LOG_ERROR( "[PackageCook] {} baked but its SDF did not reach {} — the package "
                                   "would ship an icon the runtime must rebake",
                                   p.string(), Vector::IconCachePath( key ).string() );
                        ++stats.StoreFailures;
                        continue;
                    }
                    ++stats.IconsBaked;
                }
            }
        }
        void CookTextures( CookStats& stats )
        {
            // THE EDITOR'S IMPORTER, NOT A SECOND COOK. Decode, mip chain, the BC7 gates, the intent
            // file, the freshness key and the path formula all live in TextureImporter; a packager copy
            // of any of them would be a second opinion on what a shipped texture is. Each failure was
            // logged where it happened, with the file's name.
            // Every texture asset the package carries gets its platform data into the DDC here; the
            // Texture bucket is then staged like every other (StageCookedEntries), and the game reads it
            // there -- it has no builder, so an entry missing now is a load error in the game.
            TextureImporter importer;
            for ( const fs::path& source : LooseTextureSources() )
            {
                switch ( importer.Cook( source ).Outcome )
                {
                    case TextureCookOutcome::Cooked:
                        ++stats.TexturesCooked;
                        break;
                    case TextureCookOutcome::Fresh:
                        ++stats.TexturesCached;
                        break;
                    case TextureCookOutcome::Failed:
                        ++stats.Failures;
                        break;
                    case TextureCookOutcome::Unwritten:
                        ++stats.StoreFailures;
                        break;
                }
            }

            // THE ROWS GO WHERE THE RUNTIME READS THEM. The texture cook entered every `.tex` into the
            // content registry as it went (WriteCookedBytes on a write, NoteFile on a fresh one), and the
            // player's preload asks the registry for textures rather than listing a directory — so a row
            // that stays in this process's memory is a texture the package carries and the game never
            // loads. A registry that could not be written is counted as an unwritten artifact for the
            // same reason a SPIR-V blob is.
            if ( Assets::ContentRegistry::Dirty() )
            {
                if ( const auto saved = Assets::ContentRegistry::Save(); !saved )
                {
                    LOG_ERROR( "[PackageCook] the cooked textures' registry rows were not written: {} — the "
                               "package would carry textures its runtime cannot find",
                               saved.GetError() );
                    ++stats.StoreFailures;
                }
            }
        }

        // Copies the DDC buckets a game reads into Saved/Cooked/<Platform>/ under the same relative layout.
        // WHOLE BUCKETS, not a list of what this pass touched: a fixture or an earlier cook's entry under a
        // key the runtime will ask for is exactly as valid (the key is its inputs), and the pipeline blob
        // is keyed by the driver, which no cook can enumerate. Thumbnails are the editor's alone and stay.
        void StageCookedEntries( CookStats& stats )
        {
            // AF4h: ImportedMeshSource is the envelope an FBX (etc.) import derives - no longer written
            // beside its source - so a packaged game can resolve a static mesh's source data the same way
            // the editor does, through the DDC, without shipping raw .fbx content (GamePackager's
            // IsRawMeshSource already excludes those).
            constexpr std::string_view kShippedBuckets[] = { "ShaderCache",       "FontCache",     "IconCache",
                                                             "EnvironmentCache",  "PipelineCache", "Texture",
                                                             "ImportedMeshSource" };
            const fs::path             cooked            = Common::DDC::PlatformCookedDir();
            const fs::path             ddcRoot           = Common::DDC::Root();
            std::error_code            ec;
            fs::remove_all( cooked, ec );
            fs::create_directories( cooked, ec );

            for ( const std::string_view bucket : kShippedBuckets )
            {
                const fs::path dir = Common::DDC::BucketDir( bucket );
                if ( !fs::is_directory( dir, ec ) )
                    continue;
                for ( const auto& file : fs::recursive_directory_iterator( dir, ec ) )
                {
                    if ( !file.is_regular_file( ec ) )
                        continue;
                    const fs::path target = cooked / file.path().lexically_normal().lexically_relative( ddcRoot );
                    fs::create_directories( target.parent_path(), ec );
                    fs::copy_file( file.path(), target, fs::copy_options::overwrite_existing, ec );
                    if ( ec )
                    {
                        LOG_ERROR( "[PackageCook] could not stage {} into {}: {}", file.path().string(),
                                   target.string(), ec.message() );
                        ++stats.StoreFailures;
                    }
                }
            }
        }
    } // namespace

    CookStats CookContentCaches( bool spirvDebugInfo )
    {
        CookStats stats;
        std::error_code wipe;
        fs::remove_all( CookedTextureAssetStage(), wipe );
        CookShaders( spirvDebugInfo, stats );
        CookFonts( stats );
        CookIcons( stats );
        CookTextures( stats );
        StageCookedEntries( stats );

        LOG_INFO( "[PackageCook] shaders {} compiled / {} cached, fonts {} baked / {} cached, icons {} "
                  "baked / {} cached, textures {} cooked / {} cached, {} failure(s), {} unwritten",
                  stats.ShadersCompiled, stats.ShadersCached, stats.FontsBaked, stats.FontsCached,
                  stats.IconsBaked, stats.IconsCached, stats.TexturesCooked, stats.TexturesCached, stats.Failures,
                  stats.StoreFailures );
        if ( stats.StoreFailures > 0 )
        {
            // Loud on its own line: this one is never normal, and a package built over it ships a
            // cache with holes in it that only a player's slow startup would ever reveal.
            LOG_ERROR( "[PackageCook] {} cooked artifact(s) could not be written — the package will ship "
                       "an incomplete cache and the game will rebuild them at every start",
                       stats.StoreFailures );
        }
        return stats;
    }

    fs::path CookedTextureAssetStage()
    {
        return Common::Constants::Path::CurrentProjectRoot().ProjectDir / "Saved" / "CookedAssets" /
               std::string( Common::DDC::CookPlatformName() );
    }

    Common::ResultStr<fs::path> StageCookedTextureAsset( const fs::path& editorAsset, const fs::path& relative )
    {
        const auto raw = Common::Utils::FileSystem::ReadFileContent( editorAsset );
        if ( !raw.IsSuccess() )
            return Common::MakeError<fs::path>( raw.GetError() );
        const auto cooked = Assets::CookTextureAssetForRuntime( std::span<const std::byte>(
             reinterpret_cast<const std::byte*>( raw.GetValue().data() ), raw.GetValue().size() ) );
        if ( !cooked.IsSuccess() )
            return Common::MakeFormattedError<fs::path>( "texture asset '{}' cannot be cooked for the package: {}",
                                                         editorAsset.string(), cooked.GetError() );
        const fs::path  target = CookedTextureAssetStage() / relative;
        std::error_code ec;
        fs::create_directories( target.parent_path(), ec );
        const auto written = Common::Utils::FileSystem::WriteBytesToFileAtomic(
             target, std::span<const std::byte>( reinterpret_cast<const std::byte*>( cooked.GetValue().data() ),
                                                 cooked.GetValue().size() ) );
        if ( !written.IsSuccess() )
            return Common::MakeFormattedError<fs::path>( "the cooked texture asset '{}' was not written: {}",
                                                         target.string(), written.GetError() );
        return Common::MakeSuccess( target );
    }
} // namespace Desert::Editor
