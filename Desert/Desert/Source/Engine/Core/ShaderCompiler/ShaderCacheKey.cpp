#include "ShaderCacheKey.hpp"

#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <Engine/Core/ShaderCompiler/BindlessSetIndex.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Core/ShaderCompiler/ShaderTarget.hpp>
#include <Engine/Core/ShaderCompiler/ShadingModels/ShaderRootShadingModels.hpp>
#include <Engine/Graphic/ShaderProtocols/GeneratedProtocolIncludes.hpp>

#include <chrono>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Desert::Core
{
    namespace
    {
        constexpr uint64_t kFnvOffset = 1469598103934665603ull;
        constexpr uint64_t kFnvPrime  = 1099511628211ull;

        // Bumped when anything about how a stage is COMPILED changes (target environment, debug info,
        // warning policy) without the source changing. Without it a compiler-option change would keep
        // serving artifacts built under the old options.
        // v2 (RQ1): the DSL header moved from GLSL 450 to 460, which changes the language rules every
        // stage is compiled under. The assembled text already carries the new #version line, but a key
        // that moves only through text would let an artifact compiled under 450 rules survive any path
        // that hashes a stage without that header; the fingerprint names the language version outright.
        // The target environment is ShaderTarget.hpp's (the oldest accepted device), named from that constant so
        // raising the minimum moves every key without a hand edit here.
        // v3 (BINDLESS-TEX): every stage is compiled with DESERT_BINDLESS_SET defined.
        // v4 (BINDLESS-TEX): every stage gets all of kBindlessShaderDefines (set, both bindings, sampler count and
        // wrap count); their values are mixed in right after the fingerprint (MixCompileOptions), so moving any
        // of them moves every key.
        // v5 (MAT-BINDLESS): the parser's generated row changed without the authored text changing: every row
        // carries Common/BindlessTextures.glslh, and a template's 2D textures are BindlessTexture row members
        // instead of samplers (Core::Formats::IsRowTexture).
        const std::string& OptionsFingerprint()
        {
            static const std::string fingerprint =
                 std::format( "vulkan{}.{}|glsl460|v5", VK_API_VERSION_MAJOR( kShaderTargetApiVersion ),
                              VK_API_VERSION_MINOR( kShaderTargetApiVersion ) );
            return fingerprint;
        }

        void FnvMix( uint64_t& h, std::string_view data )
        {
            for ( unsigned char c : data )
            {
                h ^= c;
                h *= kFnvPrime;
            }
        }

        // The compile options every stage shares: the fingerprint and the engine-wide defines ShaderCompiler adds.
        void MixCompileOptions( uint64_t& key )
        {
            FnvMix( key, OptionsFingerprint() );
            for ( const BindlessShaderDefine& define : kBindlessShaderDefines )
                FnvMix( key, std::format( "|{}={}", define.Name, define.Value ) );
        }

        // The include walk, shared by the collector and the hash. `visited` carries across the whole
        // traversal, so a header pulled in by two different files is listed (and hashed) once — which
        // is also what stops a cycle from recursing forever.
        // Every include file read ONCE per process and re-read only when its size or write time moves:
        // 78 programs share ~40 headers, and the walk below used to read each header once per including
        // stage. The stat keeps hot reload honest — an edited header is a different entry.
        // A file served from a mounted pak has no write time; pak content cannot change under a process.
        //
        // (write time, size) alone is NOT an identity. A write time is only as fine as the filesystem's
        // clock: NTFS stamps from the kernel tick (~15.6 ms), FAT at 2 s, HFS+ at 1 s. Two same-length
        // edits inside one tick leave both unchanged, and the cache would serve the old text - Windows CI
        // caught exactly that (EditingAnIncludedHeaderMovesTheKey, the header rewritten within 1 ms).
        // The rule is git's "racy index" rule (Common::Utils::IsRacyWriteTime): an entry read while its file
        // was still fresh is never trusted by stat and is re-read on the next lookup. Once a re-read lands
        // after the window, the entry settles and stat serves it.

        struct CachedShaderFile
        {
            bool                            Exists   = false;
            bool                            Readable = false;
            std::string                     Text;
            uint64_t                        ContentHash = 0;
            bool                            OnDisk      = false;
            std::filesystem::file_time_type WriteTime{};
            uintmax_t                       Size = 0;
            bool                            Racy = false; // read while its write time was inside kRacyWriteWindow
        };

        std::mutex                                                               s_FileCacheMutex;
        std::unordered_map<std::string, std::shared_ptr<const CachedShaderFile>> s_FileCache;

        std::shared_ptr<const CachedShaderFile> ReadShaderFileCached( const std::filesystem::path& path )
        {
            // Sampled BEFORE the stat and the read: a write that lands after this instant carries a write
            // time at or past it, so the entry it produces is racy by construction.
            const auto        readBegan = std::filesystem::file_time_type::clock::now();
            std::error_code   timeError;
            std::error_code   sizeError;
            const auto        writeTime = std::filesystem::last_write_time( path, timeError );
            const auto        size      = std::filesystem::file_size( path, sizeError );
            const bool        onDisk    = !timeError && !sizeError;
            const std::string key       = path.generic_string();
            {
                std::lock_guard lock( s_FileCacheMutex );
                if ( const auto it = s_FileCache.find( key ); it != s_FileCache.end() )
                {
                    const CachedShaderFile& e = *it->second;
                    if ( e.OnDisk == onDisk &&
                         ( !onDisk || ( !e.Racy && e.WriteTime == writeTime && e.Size == size ) ) )
                        return it->second;
                }
            }
            auto entry       = std::make_shared<CachedShaderFile>();
            entry->OnDisk    = onDisk;
            entry->WriteTime = writeTime;
            entry->Size      = onDisk ? size : 0;
            entry->Racy      = onDisk && Common::Utils::IsRacyWriteTime( writeTime, readBegan );
            entry->Exists    = Common::Utils::FileSystem::Exists( path );
            if ( entry->Exists )
            {
                if ( const auto text = Common::Utils::FileSystem::ReadFileContent( path ); text )
                {
                    entry->Readable    = true;
                    entry->Text        = text.GetValue();
                    entry->ContentHash = kFnvOffset;
                    FnvMix( entry->ContentHash, entry->Text );
                }
            }
            std::lock_guard lock( s_FileCacheMutex );
            s_FileCache[key] = entry;
            return entry;
        }

        void WalkIncludes( const std::string& source, const std::filesystem::path& requestingFile,
                           const ShaderVariant& variant, std::unordered_set<std::string>& visited,
                           std::vector<std::filesystem::path>& out, int depth )
        {
            if ( depth > 32 )
                return;

            size_t pos = 0;
            while ( ( pos = source.find( "#include", pos ) ) != std::string::npos )
            {
                const size_t      lineEnd = source.find( '\n', pos );
                const std::string line =
                     source.substr( pos, lineEnd == std::string::npos ? std::string::npos : lineEnd - pos );
                pos += 8;

                const size_t qa = line.find_first_of( "\"<" );
                if ( qa == std::string::npos )
                    continue;
                const char   closer = line[qa] == '"' ? '"' : '>';
                const size_t qb     = line.find( closer, qa + 1 );
                if ( qb == std::string::npos )
                    continue;
                const std::string name = line.substr( qa + 1, qb - qa - 1 );

                const bool                  angled = line[qa] != '"';
                const std::filesystem::path full   = ( angled ? Common::Constants::Path::SHADERDIR_PATH / name
                                                              : requestingFile.parent_path() / name )
                                                        .lexically_normal();

                if ( !visited.insert( full.generic_string() ).second )
                    continue;

                // SUBSTITUTED BODIES ARE WALKED TOO, and this is not a nicety. A generated cloud medium
                // may include a header of its own; if the walk followed the file on disk instead, that
                // header would be absent from the key and from the hot-reload watch list, and editing it
                // would change nothing until a restart — the exact staleness this file exists to prevent,
                // reached through the new door instead of the old one. The path recorded is still the
                // include's own, because that is what a watcher can watch.
                if ( angled )
                {
                    const std::string requested =
                         std::filesystem::path( name ).lexically_normal().generic_string();
                    if ( const std::string* substituted = variant.Find( requested ) )
                    {
                        out.push_back( full );
                        WalkIncludes( *substituted, full, variant, visited, out, depth + 1 );
                        continue;
                    }
                    // A GENERATED PROTOCOL BLOCK (Common/CameraUB.glslh) has no file: its text comes from the C++
                    // struct, and is walked from that text like any header's.
                    if ( const std::string* generated =
                              Graphic::ShaderProtocols::FindGeneratedProtocolInclude( requested ) )
                    {
                        out.push_back( full );
                        WalkIncludes( *generated, full, variant, visited, out, depth + 1 );
                        continue;
                    }
                    // THE GENERATED SHADING-MODEL DISPATCH IS VIRTUAL (UE /Engine/Generated/): the loaded set's
                    // text, served by the includer and never written to disk. Its own includes (the contract, the
                    // models' code) are walked from that text, so editing them is a change like any header's.
                    if ( requested == ShadingModels::kGeneratedInclude )
                    {
                        out.push_back( full );
                        const auto  held   = ShadingModels::ShaderRootShadingModels();
                        const auto& models = *held;
                        if ( models.IsSuccess() )
                            WalkIncludes( models.GetValue().GeneratedGlsl, full, variant, visited, out,
                                          depth + 1 );
                        continue;
                    }
                }

                const auto file = ReadShaderFileCached( full );
                if ( !file->Exists )
                    continue;
                out.push_back( full );
                if ( file->Readable )
                    WalkIncludes( file->Text, full, variant, visited, out, depth + 1 );
            }
        }
    } // namespace

    std::vector<std::filesystem::path> CollectShaderIncludes( const std::string&           source,
                                                              const std::filesystem::path& requestingFile,
                                                              const ShaderVariant&         variant )
    {
        std::unordered_set<std::string>    visited;
        std::vector<std::filesystem::path> includes;
        WalkIncludes( source, requestingFile, variant, visited, includes, 0 );
        return includes;
    }

    std::string ShaderFileText( const std::filesystem::path& shaderFile )
    {
        const auto file = ReadShaderFileCached( shaderFile );
        return file->Readable ? file->Text : std::string{};
    }

    std::vector<std::filesystem::path> ShaderSourceFiles( const std::filesystem::path& shaderFile )
    {
        std::vector<std::filesystem::path> files{ shaderFile };
        const auto                         file = ReadShaderFileCached( shaderFile );
        if ( !file->Readable )
            return files;
        for ( auto& include : CollectShaderIncludes( file->Text, shaderFile ) )
            files.push_back( std::move( include ) );
        return files;
    }

    bool SpirvDebugInfoThisBuild()
    {
        // The one home of the policy. ShaderCompiler generates debug info exactly when this is true,
        // and the key fingerprints it below — if the two ever came from different places they could
        // disagree, and a same-key artifact would be served across configs with different binaries.
#ifdef DESERT_CONFIG_DEBUG
        return true;
#else
        return false;
#endif
    }

    bool SpirvDebugInfoForConfigName( std::string_view configName )
    {
        // Mirrors the #ifdef above: DESERT_CONFIG_DEBUG is defined for the "Debug" build
        // configuration and nothing else. Tests/Engine/ShaderCacheKey pins this mirror to
        // SpirvDebugInfoThisBuild() in both configs.
        return configName == "Debug";
    }

    namespace
    {
        // THE SHADING-MODEL LAYOUT (ShadingModelRegistry::IndexLayoutKey): a program that includes the generated
        // dispatch was compiled against one Guid->index layout, and a set that gains a model may move the others'
        // indices — so the layout is a key input of exactly the programs that include it. The generated text is
        // mixed too: the include is virtual, so the file loop below has no bytes of it to hash.
        void MixShadingModelLayout( uint64_t& key, const std::filesystem::path& include )
        {
            if ( !include.generic_string().ends_with( ShadingModels::kGeneratedInclude ) )
                return;
            const auto  held   = ShadingModels::ShaderRootShadingModels();
            const auto& models = *held;
            // A set that failed to load fails the compile itself (the includer serves its error); the key
            // only has to differ from every loaded layout's.
            FnvMix( key, "|shadingmodels:" );
            FnvMix( key, models.IsSuccess() ? std::string_view( models.GetValue().IndexLayoutKey )
                                            : std::string_view( models.GetError() ) );
            if ( models.IsSuccess() )
                FnvMix( key, models.GetValue().GeneratedGlsl );
        }
    } // namespace

    namespace
    {
        // THE NAME A KEY GIVES A SHADER FILE: its path under the mount it lives in, never where that mount sits
        // on this disk (UE keys shaders by virtual path, /Engine/Private/..., for the same reason). The editor
        // cooks a package's SPIR-V and shader maps from Editor/Resources/Shaders; the packaged runtime reads
        // the same bytes from <App>/Contents/Resources/Resources/Shaders. An absolute path in the key made every
        // key the cook wrote one the package never asked for: a fresh Bistro package missed 418 of 806 shader
        // maps, recompiled 546 stages at boot, and refused every ShippedByCook permutation by name (TAA,
        // DeferredLighting's shadow masks, the GI tracers). Engine shaders are named under the shader root,
        // other engine files under the engine directory, project files under the project's assets root (the
        // editor's Content/ is the package's Assets/); a file under none of them keeps its full path.
        std::string MountRelativeOrNull( const std::filesystem::path& file, const std::filesystem::path& root )
        {
            if ( root.empty() )
                return {};
            const std::filesystem::path relative =
                 file.lexically_normal().lexically_relative( root.lexically_normal() );
            if ( relative.empty() || *relative.begin() == ".." )
                return {};
            return relative.generic_string();
        }

        std::string ShaderKeyPath( const std::filesystem::path& file )
        {
            namespace Path = Common::Constants::Path;
            if ( std::string relative = MountRelativeOrNull( file, Path::SHADERDIR_PATH ); !relative.empty() )
                return "shaders:" + relative;
            if ( Path::HasProject() )
            {
                const auto&                 project = Path::CurrentProjectRoot();
                const std::filesystem::path assets  = project.AssetsRoot.is_absolute()
                                                           ? project.AssetsRoot
                                                           : project.ProjectDir / project.AssetsRoot;
                if ( std::string relative = MountRelativeOrNull( file, assets ); !relative.empty() )
                    return "project:" + relative;
            }
            if ( std::string relative = MountRelativeOrNull( file, Path::RESOURCE_PATH ); !relative.empty() )
                return "engine:" + relative;
            return file.lexically_normal().generic_string();
        }
    } // namespace

    namespace
    {
        // THE GENERATED PROTOCOL TEXT (GeneratedProtocolIncludes.hpp): the include is virtual, so the file loops
        // below have no bytes of it to hash — a C++ layout change must still move every key that includes it.
        void MixGeneratedProtocol( uint64_t& key, const std::filesystem::path& include )
        {
            const std::string relative =
                 include.lexically_normal()
                      .lexically_relative( Common::Constants::Path::SHADERDIR_PATH.lexically_normal() )
                      .generic_string();
            if ( const std::string* generated = Graphic::ShaderProtocols::FindGeneratedProtocolInclude( relative ) )
            {
                FnvMix( key, "|protocol:" );
                FnvMix( key, *generated );
            }
        }
    } // namespace

    uint64_t ComputeShaderCacheKey( Formats::ShaderStage stage, const std::string& source,
                                    const std::filesystem::path& requestingFile, const ShaderVariant& variant )
    {
        return ComputeShaderCacheKeyForProfile( stage, source, requestingFile, SpirvDebugInfoThisBuild(),
                                                variant );
    }

    uint64_t ComputeShaderCacheKeyForProfile( Formats::ShaderStage stage, const std::string& source,
                                              const std::filesystem::path& requestingFile, bool spirvDebugInfo,
                                              const ShaderVariant& variant )
    {
        uint64_t key = kFnvOffset;
        MixCompileOptions( key );
        if ( spirvDebugInfo )
        {
            FnvMix( key, "|debuginfo" ); // debug info changes the binary — keep configs apart
            // ...and names the file it was compiled from (OpSource/OpString carry the path shaderc was
            // given), so two files with the same stage text get different binaries. Without the path, the
            // artifact under one key depended on which file compiled it first: a serial build and a job
            // system build of the same programs disagreed (SHC1). Without debug info the path is not in
            // the binary, and identical text keeps sharing one entry.
            FnvMix( key, ShaderKeyPath( requestingFile ) );
        }
        key ^= static_cast<uint64_t>( stage );
        key *= kFnvPrime;
        FnvMix( key, source );

        // THE SUBSTITUTED BYTES, and nothing when there are none. The loop below hashes what is on DISK
        // at each included path — which for a substituted include is not what was compiled — so the
        // variant's own hash is the only thing that separates two materials whose media differ. Mixing
        // nothing for the default variant is what leaves every key already on disk unchanged.
        if ( const uint64_t variantHash = variant.Hash(); variantHash != 0 )
        {
            key ^= variantHash;
            key *= kFnvPrime;
        }

        // Path AND content of every include: the path so that moving a header to a different directory
        // is a change even when the bytes are identical, the content so that editing it is one too.
        for ( const auto& include : CollectShaderIncludes( source, requestingFile, variant ) )
        {
            FnvMix( key, ShaderKeyPath( include ) );
            MixShadingModelLayout( key, include );
            MixGeneratedProtocol( key, include );
            // A read that fails mixes nothing — byte-identical to the empty string the old untyped
            // read produced here, so existing cache keys stay valid.
            if ( const auto file = ReadShaderFileCached( include ); file->Readable )
                FnvMix( key, file->Text );
        }

        return key;
    }

    uint64_t ComputeShaderMapKey( const std::string& programSource, const std::filesystem::path& programPath,
                                  const std::string& passName, const bool spirvDebugInfo,
                                  const ShaderVariant& variant )
    {
        (void)ShadingModels::ShaderRootShadingModels(); // writes the generated include before the walk reads it
        uint64_t key = kFnvOffset;
        FnvMix( key, "shadermap|" );
        MixCompileOptions( key );
        FnvMix( key, spirvDebugInfo ? "|debuginfo" : "|nodebuginfo" );
        FnvMix( key, "|pass:" );
        // ONE KEY PER CELL: a surface template's default cell and its default program are one program
        // (IsSurfaceDefaultCell), so asking by either name finds the same map.
        FnvMix( key, Preprocess::IsSurfaceDefaultCell(
                          Preprocess::DShaderParser::MayDeclareSurface( programSource ), passName )
                          ? std::string_view()
                          : std::string_view( passName ) );
        FnvMix( key, "|" );
        const uint64_t variantHash = variant.Hash();
        FnvMix( key, std::string_view( reinterpret_cast<const char*>( &variantHash ), sizeof variantHash ) );
        FnvMix( key, programSource );

        // The raw text names every include its stages will pull in (a `#include` in a comment only adds
        // a file, never loses one); the parser's own injections are named here because the text does not.
        std::string scanned = programSource;
        for ( const std::string_view injected : Preprocess::kParserInjectedIncludes )
            scanned.append( "\n#include <" ).append( injected ).append( ">\n" );
        // A surface template's cells compile engine headers its text never names (DShaderParser.hpp).
        if ( Preprocess::DShaderParser::MayDeclareSurface( programSource ) )
            for ( const std::string& header : Preprocess::SurfaceTemplateIncludes() )
                scanned.append( std::format( "\n#include <{}>\n", header ) );
        // So does a generic program: every pass's vertex stage is the mesh vertex factory (DShaderParser.hpp).
        else if ( Preprocess::DShaderParser::MayDeclareGenericProgram( programSource ) )
            for ( const std::string& header : Preprocess::GenericProgramIncludes() )
                scanned.append( std::format( "\n#include <{}>\n", header ) );
        for ( const auto& include : CollectShaderIncludes( scanned, programPath, variant ) )
        {
            FnvMix( key, ShaderKeyPath( include ) );
            MixShadingModelLayout( key, include );
            MixGeneratedProtocol( key, include );
            const auto     file = ReadShaderFileCached( include );
            const uint64_t hash = file->Readable ? file->ContentHash : 0;
            FnvMix( key, std::string_view( reinterpret_cast<const char*>( &hash ), sizeof hash ) );
        }
        return key;
    }
} // namespace Desert::Core
