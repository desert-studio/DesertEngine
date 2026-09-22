// PakTool — CLI for the Desert .dpak archive format (Common::Utils::PakFile, the exact code the
// Runtime mounts). Lets scripts/CI build and inspect content archives without booting the editor.
//
//   PakTool create   <out.dpak> <srcDir> [--prefix P]  pack every file under srcDir (keys relative
//                                                      to it, optionally prefixed "P/...")
//   PakTool append   <archive.dpak> <srcDir> [--prefix P]  add those files to an EXISTING v3 archive
//                                                      without rewriting a byte of it (see
//                                                      PakWriter::Mode::Append) — a cook operation,
//                                                      never a delivery one
//   PakTool list     <archive.dpak>                    every entry with its content size, its stored
//                                                      size, its offset and its codec, then the
//                                                      format version
//   PakTool extract  <archive.dpak> <outDir>           unpack all entries into outDir
//   PakTool manifest <archive.dpak|srcDir> <out.txt>   record what this release hands out
//                                                      [--prefix P]
//   PakTool patch    <base.manifest> <new.dpak> <patch.dpak>
//                                                      patch pak = entries of new that are absent or
//                                                      changed vs the manifest, PLUS the list of keys
//                                                      the manifest had and new does not. Mount it
//                                                      AFTER the base to apply the update.
//
// `patch` REPLACED `diff`, and the difference is not the spelling. `diff` compared two whole archives,
// so publishing an update meant keeping every shipped .dpak for ever (318 MB per version here), and it
// could not express a DELETION at all — its own code counted removals and printed them as "not
// representable in an overlay patch". A manifest of the same tree is 247 KiB, 0.076 % of it, so a
// release keeps the manifest of every version and no old archives; and deletions ride inside the patch
// as a reserved entry (Common/Utilities/PakFile.hpp, kDeletedEntriesKey). `diff` had no caller anywhere
// — not CI, not the Package scripts, not a test — so it is gone rather than kept beside its
// replacement. See Docs/Architecture/P3_CONTENT_MANIFEST.md.

#include <ToolMain.hpp>

#include <Common/Utilities/ContentManifest.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/PakFile.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace
{
    // Create and append are ONE function taking the mode, not two that look alike: the walk, the key
    // derivation, the prefix rule and the refusal on a failed add must be the same for both, and two
    // copies of them is how the second one drifts.
    int Pack( const fs::path& out, const fs::path& srcDir, const std::string& prefix,
              Common::Utils::PakWriter::Mode mode )
    {
        std::error_code ec;
        if ( !fs::is_directory( srcDir, ec ) )
        {
            std::fprintf( stderr, "PakTool: not a directory: %s\n", srcDir.string().c_str() );
            return 1;
        }

        Common::Utils::PakWriter writer( out, mode );
        if ( !writer.IsOpen() )
        {
            std::fprintf( stderr, "PakTool: cannot %s %s\n",
                          mode == Common::Utils::PakWriter::Mode::Append ? "append to" : "create",
                          out.string().c_str() );
            return 1;
        }

        size_t    added = 0;
        uintmax_t bytes = 0;
        for ( auto it = fs::recursive_directory_iterator( srcDir, ec );
              it != fs::recursive_directory_iterator(); it.increment( ec ) )
        {
            if ( ec || !it->is_regular_file() )
                continue;
            const fs::path rel = fs::relative( it->path(), srcDir, ec );
            std::string    key = rel.generic_string();
            if ( !prefix.empty() )
                key = prefix + "/" + key;
            if ( !writer.AddFile( key, it->path() ) )
            {
                std::fprintf( stderr, "PakTool: failed to add %s\n", it->path().string().c_str() );
                return 1;
            }
            ++added;
            bytes += fs::file_size( it->path(), ec );
        }

        const size_t records = writer.Finalize();
        if ( records == 0 )
        {
            std::fprintf( stderr, "PakTool: finalize failed (empty archive?)\n" );
            return 1;
        }
        // The archive's total is printed beside the batch's, because on an append they differ and the
        // difference is the only thing that says the existing entries survived.
        std::printf( "PakTool: %zu file(s), %ju bytes -> %s (%zu entr%s in the archive)\n", added,
                     (uintmax_t)bytes, out.string().c_str(), records, records == 1 ? "y" : "ies" );
        return 0;
    }

    int List( const fs::path& pakPath )
    {
        Common::Utils::PakReader reader( pakPath );
        if ( !reader.IsOpen() )
        {
            // WITH THE REASON. This tool is what a developer runs to find out why a shipped archive
            // will not mount, and "cannot open" answered that question with the question.
            std::fprintf( stderr, "PakTool: cannot open %s: %s\n", pakPath.string().c_str(),
                          reader.OpenError().c_str() );
            return 1;
        }
        // Both sizes and the codec, because "why is this archive that big" and "why is this entry slow
        // to read" are the two questions this listing is run to answer, and one size column cannot
        // distinguish an entry that did not compress from one that was not worth compressing.
        for ( const auto& key : reader.KeysWithPrefix( "" ) )
            std::printf( "%10ju %10ju %10ju  %-5s  %s\n", (uintmax_t)reader.EntrySize( key ).value_or( 0 ),
                         (uintmax_t)reader.EntryStoredSize( key ).value_or( 0 ),
                         (uintmax_t)reader.EntryOffset( key ).value_or( 0 ),
                         reader.EntryCodec( key ) == Common::Utils::PakCodec::LZ4 ? "lz4" : "store",
                         key.c_str() );
        // Deletions are printed EXPLICITLY because they are invisible everywhere else: the reserved
        // entry is hidden from every content accessor on purpose, so a patch that removes ten files and
        // adds none would otherwise list as an empty archive.
        for ( const auto& key : reader.DeletedKeys() )
            std::printf( "%10s  %s\n", "DELETED", key.c_str() );
        std::printf( "PakTool: %zu entr%s, %zu deletion(s), format v%u in %s\n", reader.EntryCount(),
                     reader.EntryCount() == 1 ? "y" : "ies", reader.DeletedKeys().size(),
                     static_cast<unsigned>( reader.Version() ), pakPath.string().c_str() );
        return 0;
    }

    int Extract( const fs::path& pakPath, const fs::path& outDir )
    {
        Common::Utils::PakReader reader( pakPath );
        if ( !reader.IsOpen() )
        {
            std::fprintf( stderr, "PakTool: cannot open %s: %s\n", pakPath.string().c_str(),
                          reader.OpenError().c_str() );
            return 1;
        }

        std::error_code ec;
        size_t          written = 0;
        for ( const auto& key : reader.KeysWithPrefix( "" ) )
        {
            const auto data = reader.Read( key );
            if ( !data )
            {
                std::fprintf( stderr, "PakTool: failed to read entry %s\n", key.c_str() );
                return 1;
            }
            const fs::path dst = outDir / fs::path( key );
            fs::create_directories( dst.parent_path(), ec );
            // Through the write primitive (Д35). This loop used to write through a local std::ofstream
            // that was never closed before `if ( !out )`, so a full destination volume left short files
            // behind and the command still printed "extracted N entries" and exited 0 — an extraction
            // that reports its own count as proof is exactly where a silent truncation is invisible.
            if ( const auto ok = Common::Utils::FileSystem::WriteContentToFileAtomic( dst, *data ); !ok )
            {
                std::fprintf( stderr, "PakTool: failed to write %s: %s\n", dst.string().c_str(),
                              ok.GetError().c_str() );
                return 1;
            }
            ++written;
        }
        std::printf( "PakTool: extracted %zu entr%s -> %s\n", written, written == 1 ? "y" : "ies",
                     outDir.string().c_str() );
        return 0;
    }

    // A manifest of whatever the argument is — an archive or the tree an archive is built from. Both
    // spellings must produce the SAME manifest for the same content; that equality is the whole reason
    // a release can record its manifest from either side and later diff the other against it, and it is
    // pinned by ContentManifest.ATreeAndThePakBuiltFromItAgree.
    int Manifest( const fs::path& source, const fs::path& outPath, const std::string& prefix )
    {
        std::error_code                ec;
        Common::Utils::ContentManifest manifest;

        if ( fs::is_directory( source, ec ) )
        {
            auto built = Common::Utils::ContentManifest::FromDirectory( source, prefix );
            if ( !built )
            {
                std::fprintf( stderr, "PakTool: %s\n", built.GetError().c_str() );
                return 1;
            }
            manifest = built.ExtractValue();
        }
        else
        {
            Common::Utils::PakReader reader( source );
            if ( !reader.IsOpen() )
            {
                std::fprintf( stderr, "PakTool: cannot open %s: %s\n", source.string().c_str(),
                              reader.OpenError().c_str() );
                return 1;
            }
            manifest = Common::Utils::ContentManifest::FromPak( reader );
        }

        const std::string text = manifest.Serialize();
        fs::create_directories( outPath.parent_path(), ec );
        // Through the write primitive. This site was already honest — it closed before deciding, and the
        // Д31-D census quoted it as the model — but "correct because somebody remembered" is the thing
        // that census exists to stop being the standard. There is one way to write a file in this tree.
        if ( const auto ok = Common::Utils::FileSystem::WriteContentToFileAtomic( outPath, text ); !ok )
        {
            std::fprintf( stderr, "PakTool: failed to write %s: %s\n", outPath.string().c_str(),
                          ok.GetError().c_str() );
            return 1;
        }
        std::printf( "PakTool: manifest of %zu entr%s (%zu bytes) -> %s\n", manifest.Count(),
                     manifest.Count() == 1 ? "y" : "ies", text.size(), outPath.string().c_str() );
        return 0;
    }

    // The patch itself is built by Common::Utils::BuildPatchPak — this is the CLI around it. The loop
    // used to live here, inside `main()`'s process, which put the only consumer of a recorded manifest
    // somewhere no test could reach without spawning a binary it cannot depend on having been built.
    int Patch( const fs::path& baseManifest, const fs::path& newPath, const fs::path& outPath )
    {
        const auto built = Common::Utils::BuildPatchPak( baseManifest, newPath, outPath );
        if ( !built )
        {
            std::fprintf( stderr, "PakTool: %s\n", built.GetError().c_str() );
            return 1;
        }

        // "Nothing changed" is reported as ITS OWN outcome rather than as a patch of size zero: the
        // exit code is 0 either way, so the line below is the only thing that tells a release script
        // whether there is a file to upload.
        if ( !built.GetValue().Written )
        {
            std::printf( "PakTool: no differences — no patch written\n" );
            return 0;
        }

        const auto& diff = built.GetValue().Diff;
        std::printf( "PakTool: patch %s — %zu added, %zu changed, %zu deleted\n", outPath.string().c_str(),
                     diff.Added.size(), diff.Changed.size(), diff.Removed.size() );
        return 0;
    }

    int Usage()
    {
        std::fprintf( stderr, "Usage:\n"
                              "  PakTool create   <out.dpak> <srcDir> [--prefix P]\n"
                              "  PakTool append   <archive.dpak> <srcDir> [--prefix P]\n"
                              "  PakTool list     <archive.dpak>\n"
                              "  PakTool extract  <archive.dpak> <outDir>\n"
                              "  PakTool manifest <archive.dpak|srcDir> <out.txt> [--prefix P]\n"
                              "  PakTool patch    <base.manifest> <new.dpak> <patch.dpak>\n" );
        return 2;
    }
} // namespace

static int RunTool( int argc, char** argv )
{
    if ( argc < 3 )
        return Usage();

    const std::string cmd = argv[1];
    if ( ( cmd == "create" || cmd == "append" ) && argc >= 4 )
    {
        std::string prefix;
        for ( int i = 4; i < argc - 1; ++i )
            if ( std::strcmp( argv[i], "--prefix" ) == 0 )
                prefix = argv[i + 1];
        return Pack( argv[2], argv[3], prefix,
                     cmd == "append" ? Common::Utils::PakWriter::Mode::Append
                                     : Common::Utils::PakWriter::Mode::Create );
    }
    if ( cmd == "list" )
        return List( argv[2] );
    if ( cmd == "extract" && argc >= 4 )
        return Extract( argv[2], argv[3] );
    if ( cmd == "manifest" && argc >= 4 )
    {
        std::string prefix;
        for ( int i = 4; i < argc - 1; ++i )
            if ( std::strcmp( argv[i], "--prefix" ) == 0 )
                prefix = argv[i + 1];
        return Manifest( argv[2], argv[3], prefix );
    }
    if ( cmd == "patch" && argc >= 5 )
        return Patch( argv[2], argv[3], argv[4] );

    return Usage();
}

// The entry point, one line. Anything this tool throws is named on stderr with the tool's own name
// instead of reaching std::terminate, which would print the exception's TYPE and nothing else — see
// Tools/Shared/ToolMain.hpp.
int main( int argc, char** argv )
{
    return Desert::Tools::RunMain( "PakTool", argc, argv, &RunTool );
}
