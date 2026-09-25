// "One list decides which texture source format wins; nobody else keeps a copy."
//
// The defect this exists to prevent, measured on 2026-09-06: TWO hand-written priority lists existed —
// AssimpImporter's extension fallback and FbxMeshSplitter's FormatRank — and they had already drifted.
// The importer ranked `.jpg` above `.tga`, so a lossy JPEG sitting next to a model silently beat the
// lossless TGA of the same stem; the splitter preferred the TGA. Both were locally plausible; only the
// relation between them was wrong, which is this project's most-repeated defect shape.
//
// The list now lives in Editor/Import/TextureSourceFormats.hpp alone. This suite asserts three things:
//
//   1. The ORDER's own invariants, at compile time: lossless before lossy before extended-range, `.tga`
//      first. Reordering the header against its stated rationale fails the build, not a code review.
//   2. Both known consumers actually include the shared header — a list with a copy instead of a reader
//      is exactly the two-list state again, spelled differently.
//   3. The census: no OTHER source file (comments stripped) contains two or more image-extension
//      literals unless it is listed here as an exclusion WITH ITS REASON. A membership check ("is this
//      an image?") is legitimate and order-free; a new file that collects several extensions fails this
//      test until somebody decides which of the two it is. That decision is the point.

#include <Editor/Import/TextureSourceFormats.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using Desert::Editor::kTextureSourceExtensionCount;
using Desert::Editor::kTextureSourceExtensions;
using Desert::Editor::TextureSourceFormatRank;

// --- 1. The order itself, compile-time. Lossless (.tga/.png/.bmp) < lossy (.jpg/.jpeg) < extended
// range (.exr/.hdr); .tga first by the owner's decision recorded in the header. If any of these fires,
// somebody reordered the list — the header says a reason must be written there when that happens.
static_assert( TextureSourceFormatRank( ".tga" ) == 0, "TGA is the preferred source format" );
static_assert( TextureSourceFormatRank( ".tga" ) < TextureSourceFormatRank( ".jpg" ) );
static_assert( TextureSourceFormatRank( ".png" ) < TextureSourceFormatRank( ".jpg" ) );
static_assert( TextureSourceFormatRank( ".bmp" ) < TextureSourceFormatRank( ".jpg" ) );
static_assert( TextureSourceFormatRank( ".jpg" ) < TextureSourceFormatRank( ".exr" ) );
static_assert( TextureSourceFormatRank( ".jpeg" ) < TextureSourceFormatRank( ".exr" ) );
static_assert( TextureSourceFormatRank( ".jpeg" ) < TextureSourceFormatRank( ".hdr" ) );
static_assert( TextureSourceFormatRank( ".dds" ) == kTextureSourceExtensionCount, "unknown ext -> count" );
static_assert( TextureSourceFormatRank( ".TGA" ) == kTextureSourceExtensionCount,
               "the rank function expects a lower-cased extension; callers lower-case first" );

namespace
{
    // Files that legitimately name several image extensions in CODE. Each is a membership statement
    // ("is this an image at all?"), where order carries no meaning — not a second priority list.
    struct Exclusion
    {
        const char* Path;
        const char* Why;
    };

    constexpr Exclusion kExclusions[] = {
         { "Editor/Source/Editor/Import/TextureSourceFormats.hpp", "the one list itself" },
         { "Editor/Source/Editor/Panels/AssetReferences/AssetReferencesPanel.cpp",
           "kLeafExts: which asset files are LEAVES of the reference graph (never referrers). Set "
           "membership only; iteration order never decides between two files" },
         { "Editor/Source/Editor/Panels/Clouds/CloudLayoutPanel.cpp",
           "an ||-chain answering 'can this dropped file be a cloud mask?' plus the open-file dialog's "
           "display filter. Both are membership; nothing competes with anything" },
         // Обе стороны сессии нашли эту строку независимо и в один час — тимлид сводом, задача О4 по
         // дороге. Один и тот же вывод дважды, из разных мест, — довод в пользу того, что перепись
         // задаёт правильный вопрос: задача О3 дала панели шума импорт и экспорт листа срезов
         // (8618daba) и не объявила два появившихся диалога здесь.
         { "Editor/Source/Editor/Panels/Clouds/CloudNoiseVolumePanel.cpp",
           "the open-file dialog's display filter for a noise SLICE SHEET, and the save dialog's. The "
           "sheet is read whole by stbi and its pixels ARE the voxels, so no format outranks another "
           "here — the same membership-only case as CloudLayoutPanel one row up" },
         { "Editor/Source/Editor/Splash/SplashImage.hpp",
           "fixed paths of three committed files (the splash's source JPEG, its cooked .tex and the "
           "application icon PNG). Each is named whole; nothing chooses between two formats of one stem" },
         { "Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.cpp",
           "the extension -> FileType icon map and the import dialog's display filter. Membership and "
           "presentation; two same-stem files both simply appear in the tree" },
    };

    // The repository root, found by walking up from wherever the test binary was started — the same
    // approach the UI-element census uses, so no suite has to be run from one exact directory.
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Editor/Source/Editor/Import/TextureSourceFormats.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const std::filesystem::path& path )
    {
        std::ifstream in( path, std::ios::binary );
        if ( !in )
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // Remove // and /* */ comments so that PROSE about extensions (AssimpImporter narrates the gothic
    // FBX asking for an .exr) does not count as a list. String literals are kept: a real extension list
    // is string literals. Good enough for a census — this is not a C++ lexer, and a `//` inside a
    // string literal would only ever HIDE code from the census on the same line, never invent a hit.
    std::string StripComments( const std::string& src )
    {
        std::string out;
        out.reserve( src.size() );
        for ( std::size_t i = 0; i < src.size(); )
        {
            if ( src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/' )
            {
                while ( i < src.size() && src[i] != '\n' )
                    ++i;
            }
            else if ( src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*' )
            {
                i += 2;
                while ( i + 1 < src.size() && !( src[i] == '*' && src[i + 1] == '/' ) )
                    ++i;
                i = ( i + 1 < src.size() ) ? i + 2 : src.size();
            }
            else
            {
                out.push_back( src[i++] );
            }
        }
        return out;
    }

    // How many DISTINCT known image extensions the (comment-stripped) source mentions.
    std::size_t DistinctExtensionMentions( const std::string& code )
    {
        std::size_t n = 0;
        for ( const char* ext : kTextureSourceExtensions )
        {
            if ( code.find( ext ) != std::string::npos )
                ++n;
        }
        return n;
    }
} // namespace

// --- 2. Both consumers read the one list. If a consumer stops including it, it either stopped caring
// about formats (delete its row here) or grew its own copy (the defect; wire it back).
TEST( TextureSourceFormatCensus, BothConsumersIncludeTheSharedList )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the working directory";

    const char* consumers[] = {
         "Editor/Source/Editor/Import/Assimp/AssimpImporter.cpp",
         "Tools/FbxMeshSplitter/FbxMeshSplitter.cpp",
    };
    for ( const char* consumer : consumers )
    {
        const std::string source = ReadFile( root + consumer );
        ASSERT_FALSE( source.empty() ) << consumer << " could not be read";
        EXPECT_NE( source.find( "#include <Editor/Import/TextureSourceFormats.hpp>" ), std::string::npos )
             << consumer << " chooses between texture source formats but does not include the shared "
             << "priority list — it has either grown a private copy or stopped choosing. Decide which.";
    }
}

// --- 3. The census: nobody else keeps a list. Two or more distinct extension literals in one file's
// code is a list until an exclusion row says otherwise.
TEST( TextureSourceFormatCensus, NoSecondExtensionListExists )
{
    namespace fs = std::filesystem;

    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the working directory";

    std::set<std::string> excluded;
    for ( const Exclusion& x : kExclusions )
    {
        EXPECT_GT( std::string( x.Why ).size(), 15u ) << x.Path << " is excluded without a reason";
        excluded.insert( x.Path );
    }

    const char* kRoots[] = { "Editor/Source", "Desert/Desert/Source", "Desert/Common/Source", "Tools", "Runtime" };

    std::set<std::string> offenders; // files with >=2 mentions, for the ghost check below
    for ( const char* scanRoot : kRoots )
    {
        std::error_code                  ec;
        fs::recursive_directory_iterator it( root + scanRoot, ec ), end;
        for ( ; !ec && it != end; it.increment( ec ) )
        {
            const fs::path& p = it->path();
            const auto      e = p.extension();
            if ( e != ".cpp" && e != ".hpp" && e != ".h" )
                continue;

            const std::size_t mentions = DistinctExtensionMentions( StripComments( ReadFile( p ) ) );
            if ( mentions < 2 )
                continue;

            // Path relative to the repo root, forward slashes, for comparison against the rows above.
            std::string rel = fs::path( p ).lexically_normal().generic_string();
            const auto  at  = rel.find( scanRoot );
            if ( at != std::string::npos )
                rel = rel.substr( at );

            offenders.insert( rel );
            EXPECT_TRUE( excluded.count( rel ) == 1 )
                 << rel << " names " << mentions << " distinct image extensions in code. If it CHOOSES "
                 << "between formats, it must read Editor/Import/TextureSourceFormats.hpp instead of "
                 << "keeping a copy (two copies of this priority have already drifted once). If it only "
                 << "asks 'is this an image?', add an exclusion row with that reason to this test.";
        }
    }

    // Ghost check: every exclusion still describes a real file that still mentions several extensions.
    // Without this a stale row could hide the next second list behind a long-deleted first one.
    for ( const Exclusion& x : kExclusions )
        EXPECT_TRUE( offenders.count( x.Path ) == 1 )
             << x.Path << " is excluded here but no longer names two image extensions in its code (or "
             << "is gone) — the row is stale and should go.";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
