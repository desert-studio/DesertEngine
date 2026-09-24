#include "ShaderCacheKey.hpp"

#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>

#include <chrono>
#include <memory>
#include <mutex>
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
        constexpr const char* kOptionsFingerprint = "vulkan1.1|v1";

        void FnvMix( uint64_t& h, std::string_view data )
        {
            for ( unsigned char c : data )
            {
                h ^= c;
                h *= kFnvPrime;
            }
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
        // The rule is git's "racy index" rule: an entry read while its file was still fresh - written
        // within kRacyWindow of the moment the read began - is never trusted by stat and is re-read on
        // the next lookup. Once a re-read lands after the window, the entry settles and stat serves it.
        constexpr std::chrono::seconds kRacyWindow{ 2 };

        struct CachedShaderFile
        {
            bool                            Exists   = false;
            bool                            Readable = false;
            std::string                     Text;
            uint64_t                        ContentHash = 0;
            bool                            OnDisk      = false;
            std::filesystem::file_time_type WriteTime{};
            uintmax_t                       Size = 0;
            bool                            Racy = false; // read while its write time was inside kRacyWindow
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
            entry->Racy      = onDisk && writeTime + kRacyWindow >= readBegan;
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
        // Mirrors the #ifdef above: DESERT_CONFIG_DEBUG is defined for the "Debug" premake
        // configuration and nothing else. Tests/Engine/ShaderCacheKey pins this mirror to
        // SpirvDebugInfoThisBuild() in both configs.
        return configName == "Debug";
    }

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
        FnvMix( key, kOptionsFingerprint );
        if ( spirvDebugInfo )
            FnvMix( key, "|debuginfo" ); // debug info changes the binary — keep configs apart
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
            FnvMix( key, include.generic_string() );
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
        uint64_t key = kFnvOffset;
        FnvMix( key, "shadermap|" );
        FnvMix( key, kOptionsFingerprint );
        FnvMix( key, spirvDebugInfo ? "|debuginfo" : "|nodebuginfo" );
        FnvMix( key, "|pass:" );
        FnvMix( key, passName );
        FnvMix( key, "|" );
        const uint64_t variantHash = variant.Hash();
        FnvMix( key, std::string_view( reinterpret_cast<const char*>( &variantHash ), sizeof variantHash ) );
        FnvMix( key, programSource );

        // The raw text names every include its stages will pull in (a `#include` in a comment only adds
        // a file, never loses one); the parser's own injections are named here because the text does not.
        std::string scanned = programSource;
        for ( const std::string_view injected : Preprocess::kParserInjectedIncludes )
            scanned.append( "\n#include <" ).append( injected ).append( ">\n" );
        for ( const auto& include : CollectShaderIncludes( scanned, programPath, variant ) )
        {
            FnvMix( key, include.generic_string() );
            const auto     file = ReadShaderFileCached( include );
            const uint64_t hash = file->Readable ? file->ContentHash : 0;
            FnvMix( key, std::string_view( reinterpret_cast<const char*>( &hash ), sizeof hash ) );
        }
        return key;
    }
} // namespace Desert::Core
