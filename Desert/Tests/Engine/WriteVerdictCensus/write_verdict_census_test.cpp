// "A WRITE THAT WAS NEVER CONFIRMED MUST NOT BE REPORTED AS A WRITE THAT HAPPENED."
//
// Д31-A was one instance of this: saving a scene reported green about a write that had not occurred, and the
// next "Save and Open" destroyed the scene. The fix — Common::Utils::FileSystem::WriteContentToFileAtomic —
// carries the reasoning as a comment at the site, and it is worth quoting because it is the whole of this
// gate:
//
//     // close() explicitly, BEFORE the verdict: it flushes, and a buffered failure (disk full, the
//     // volume going away) may only surface here. The destructor would swallow exactly that.
//
// That is not a stylistic preference. `std::ofstream::write()` hands bytes to the filebuf, and the filebuf
// decides when they reach the OS. A payload larger than the buffer flushes through and a failure is visible
// at the write; a payload smaller than the buffer sits in memory, and the failure surfaces at the FLUSH —
// which, without an explicit close(), is the DESTRUCTOR, running after the function has already returned
// success to its caller. So the familiar and apparently careful idiom
//
//     std::ofstream file( path, std::ios::binary | std::ios::trunc );
//     if ( !file ) return Error( "could not open" );
//     file.write( bytes.data(), bytes.size() );
//     if ( !file ) return Error( "could not write" );   // <-- catches SOME failures, by payload size
//     return BOOLSUCCESS;                               // <-- and reports green for the rest
//
// is a silent fallback under §1.4 that reads as its opposite: it is more convincing than no check at all,
// because the reviewer sees a check. The condition under which it lies is invisible in the source and
// depends on how many bytes the caller happened to have.
//
// WHAT WAS MEASURED when this gate was written (Д31-D), by running this matcher over
// Desert/Desert/Source, Desert/Common/Source, Editor/Source, Runtime/Source and Tools:
//
//     18 std::ofstream sites are written to.
//      3 of them close before deciding — WriteContentToFileAtomic, PakTool's Manifest, DesertHeaderTool.
//     15 do not.
//
// So the tree KNEW the rule — it was written down, with its reason, at the site that paid for it — and it
// was obeyed at one site in six. That ratio, not any single file, is why this is a gate and not a bug
// report.
//
// WHAT Д35 DID WITH THAT LIST, because a census that only counts is a census nobody acts on. All fifteen
// were closed, and not by fifteen copies of one patch: fifteen sites with one shape is a MISSING
// ABSTRACTION. Fourteen of them now call Common::Utils::FileSystem::WriteBytesToFileAtomic (the write
// primitive, which grew a byte-span spelling so a 64 MB voxel container did not have to be copied into a
// std::string to reach it) — so "wrote and did not check" stopped being expressible at those sites rather
// than merely stopped being done. PakWriter's three rows were one object lying three ways and became one
// invariant: one stream for the archive's whole life, closed in Finalize, and Finalize's count read after
// the close. The fifteenth, Tools/FbxMeshSplitter, links no engine code by design and keeps a local
// close-and-check — the argument is at the site.
//
// The register below is therefore down to its single KEEP row. See it for why the register is a
// std::array.
//
// THE RULE. A std::ofstream that is WRITTEN TO is closed explicitly, in the same block, before the function
// decides what to tell its caller. Streams that are never written to are not the subject (truncating a file
// to clear it writes nothing). Temporaries and by-reference parameters are not the subject either: a
// temporary belongs to the expression that made it, and a parameter belongs to whoever opened it.
//
// THE REGISTER. Everything the matcher finds today is listed below BY FILE AND BY ENCLOSING FUNCTION, with a
// verdict for each — because a gate that pins a COUNT is satisfied by editing the count, and a gate that
// pins a line number is broken by the first edit above it. The register is checked in both directions: a new
// unclosed write is red, and so is a register row whose site has been FIXED without the row being deleted.
// The second direction is the one that keeps a debt register from rotting into decoration.

#include "../SettingConsumers/setting_consumers_reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using namespace Desert::Tests::ConsumerText;

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Common/Source/Common/Utilities/FileSystem.cpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const fs::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    std::vector<fs::path> SourceFiles( const std::string& root )
    {
        std::vector<fs::path> files;
        for ( const char* dir :
              { "Desert/Desert/Source", "Desert/Common/Source", "Editor/Source", "Runtime/Source", "Tools" } )
        {
            const fs::path base = fs::path( root ) / dir;
            if ( !fs::exists( base ) )
                continue;
            for ( const auto& entry : fs::recursive_directory_iterator( base ) )
            {
                if ( !entry.is_regular_file() )
                    continue;
                const std::string ext = entry.path().extension().string();
                if ( ext == ".cpp" || ext == ".hpp" || ext == ".h" )
                    files.push_back( entry.path() );
            }
        }
        std::sort( files.begin(), files.end() );
        return files;
    }

    struct Finding
    {
        std::string File;     // repo-relative, forward slashes
        std::string Function; // the enclosing function — the register's key, because it survives edits
        std::string Stream;   // the variable's name, for the message
        int         Line = 0;
    };

    bool IsSpace( char c )
    {
        return std::isspace( static_cast<unsigned char>( c ) ) != 0;
    }

    int LineOf( const std::string& src, std::size_t at )
    {
        return 1 + static_cast<int>( std::count( src.begin(), src.begin() + at, '\n' ) );
    }

    // The `{` that opens the block @p at sits in, or npos. Walks back counting depth, so a nested block
    // between the declaration and the function body does not fool it.
    std::size_t EnclosingBrace( const std::string& s, std::size_t at )
    {
        int depth = 0;
        for ( std::size_t i = at; i-- > 0; )
        {
            if ( s[i] == '}' )
                ++depth;
            else if ( s[i] == '{' )
            {
                if ( depth == 0 )
                    return i;
                --depth;
            }
        }
        return std::string::npos;
    }

    // The name of the function whose body contains @p at. From the enclosing `{`, step back over the
    // parameter list and read the identifier in front of it, keeping a `Class::` qualifier when one is
    // there. Blocks that are not function bodies (an `if`, a loop) are stepped THROUGH, because the name we
    // want is the function's, not the block's.
    std::string EnclosingFunction( const std::string& s, std::size_t at )
    {
        std::size_t brace = at;
        for ( int hop = 0; hop < 8; ++hop )
        {
            brace = EnclosingBrace( s, brace );
            if ( brace == std::string::npos )
                return "<file scope>";

            std::size_t i        = brace;
            const auto  skipBack = [&s, &i]()
            {
                while ( i > 0 && IsSpace( s[i - 1] ) )
                    --i;
            };
            skipBack();
            // Trailing qualifiers sit between the `)` and the `{`. Without stepping over them the name read
            // back from a `... SavePipelineCache() const {` is "const", which is nobody's function.
            for ( bool moved = true; moved; )
            {
                moved = false;
                for ( const char* q : { "const", "noexcept", "override", "final" } )
                {
                    const std::size_t n = std::string( q ).size();
                    if ( i >= n && s.compare( i - n, n, q ) == 0 && ( i == n || !IsIdentChar( s[i - n - 1] ) ) )
                    {
                        i -= n;
                        skipBack();
                        moved = true;
                    }
                }
            }
            // A constructor's initialiser list (`: m_Path( pakPath ), m_Ok( false )`) sits here too. Each
            // entry is `name( ... )`, so stepping over the parens AND the name in front of them, then
            // looking for the `,` or `:` that introduced it, walks the whole list back to the signature.
            for ( int entry = 0; entry < 24 && i > 0 && s[i - 1] == ')'; ++entry )
            {
                int depth = 0;
                while ( i > 0 )
                {
                    --i;
                    if ( s[i] == ')' )
                        ++depth;
                    else if ( s[i] == '(' && --depth == 0 )
                        break;
                }
                const std::size_t afterParen = i;
                skipBack();
                while ( i > 0 && ( IsIdentChar( s[i - 1] ) || s[i - 1] == ':' ) )
                    --i;
                skipBack();
                if ( i > 0 && ( s[i - 1] == ':' || s[i - 1] == ',' ) )
                {
                    while ( i > 0 && ( s[i - 1] == ':' || s[i - 1] == ',' || IsSpace( s[i - 1] ) ) )
                        --i;
                    continue; // another initialiser, or the signature's `)` now directly behind us
                }
                i = afterParen; // not an initialiser list: this was the parameter list itself
                break;
            }

            std::size_t end = i;
            while ( end > 0 && IsSpace( s[end - 1] ) )
                --end;
            std::size_t begin = end;
            while ( begin > 0 && ( IsIdentChar( s[begin - 1] ) || s[begin - 1] == ':' ) )
                --begin;
            const std::string name = s.substr( begin, end - begin );
            if ( !name.empty() && name != "if" && name != "for" && name != "while" && name != "switch" &&
                 name != "else" && name != "do" && name != "catch" )
                return name;
            brace = begin; // a control block, not a function: keep walking outwards
        }
        return "<unknown>";
    }

    bool HasCall( const std::string& w, const std::string& name, const std::string& method )
    {
        for ( std::size_t at : WordPositions( w, name ) )
        {
            std::size_t i = SkipSpace( w, at + name.size() );
            if ( i >= w.size() || w[i] != '.' )
                continue;
            i = SkipSpace( w, i + 1 );
            if ( w.compare( i, method.size(), method ) == 0 )
                return true;
        }
        return false;
    }

    // Is @p name written to inside @p w? Three spellings, all of them in this tree: `name <<`,
    // `name.write(` and `name` handed to a helper that writes (`WritePod( out, x )`).
    bool IsWrittenTo( const std::string& w, const std::string& name )
    {
        if ( HasCall( w, name, "write" ) )
            return true;
        for ( std::size_t at : WordPositions( w, name ) )
        {
            const std::size_t i = SkipSpace( w, at + name.size() );
            if ( i + 1 < w.size() && w[i] == '<' && w[i + 1] == '<' )
                return true;
            // passed by reference to something that writes: `Helper( out, ... )`
            if ( i < w.size() && ( w[i] == ',' || w[i] == ')' ) )
            {
                std::size_t j = at;
                while ( j > 0 && IsSpace( w[j - 1] ) )
                    --j;
                if ( j > 0 && ( w[j - 1] == '(' || w[j - 1] == ',' ) )
                    return true;
            }
        }
        return false;
    }

    std::vector<Finding> UnclosedWritesIn( const std::string& path, const std::string& code )
    {
        std::vector<Finding> out;
        for ( std::size_t at : WordPositions( code, "ofstream" ) )
        {
            std::size_t i = SkipSpace( code, at + 8 );
            // A temporary (`std::ofstream( p ).close()`) belongs to its expression; a reference or pointer
            // (`std::ofstream& out`) belongs to whoever opened it. Neither is this gate's subject.
            if ( i >= code.size() || code[i] == '(' || code[i] == '&' || code[i] == '*' )
                continue;
            const std::string name = IdentAt( code, i );
            if ( name.empty() )
                continue;

            // The window is the rest of the enclosing block: where the stream is usable, and therefore
            // where its close() would have to be.
            std::size_t j     = i + name.size();
            int         depth = 0;
            std::size_t end   = code.size();
            for ( std::size_t k = j; k < code.size(); ++k )
            {
                if ( code[k] == '{' )
                    ++depth;
                else if ( code[k] == '}' )
                {
                    if ( depth == 0 )
                    {
                        end = k;
                        break;
                    }
                    --depth;
                }
            }
            const std::string window = code.substr( j, end - j );

            if ( !IsWrittenTo( window, name ) )
                continue;
            if ( HasCall( window, name, "close" ) )
                continue;

            out.push_back( { path, EnclosingFunction( code, at ), name, LineOf( code, at ) } );
        }
        return out;
    }

    // --- The register ---------------------------------------------------------------------------------
    //
    // Every site the matcher finds today, keyed by file and enclosing function — NOT by line, which the next
    // edit invalidates, and NOT by a count, which the next edit satisfies. Each row carries the verdict, and
    // the verdict is the deliverable: this census was asked for decisions, not sightings.
    struct KnownSite
    {
        const char* File;
        const char* Function;
        const char* Verdict;
    };

    // THE REGISTER IS A std::array AND NOT A C ARRAY, and the reason is that it MUST BE ABLE TO REACH
    // ZERO ROWS. `constexpr KnownSite kRegister[] = {};` is not C++: a zero-length array is a GNU
    // extension clang accepts silently and MSVC rejects with C2466. This project shipped that defect to
    // `dev` twice in one day (И14), both times from a census whose whole GOAL was an empty register — so
    // the type that holds a debt list has to be able to express the list's own success. `std::to_array`
    // also derives the count from the rows, which is why nothing below states a number: a gate pinned to
    // a COUNT is satisfied by editing the count.
    //
    // WHAT USED TO BE HERE. Fifteen FIX rows — PakWriter's three, the four cloud asset Saves,
    // SequencerPanel::SaveClipToDisk, the two identical copies of WriteJsonToFile,
    // BlendImporter::WriteConvertScript, the two bakers, PakTool::Extract and FbxMeshSplitter::WriteObj.
    // Д35 closed all fifteen and deleted their rows, which is what the backwards gate below obliges: a
    // row whose site is fixed is a false statement and goes red until it is removed.
    //
    // The last KEEP went with AF7: VulkanLogicalDevice::SavePipelineCache (void, best-effort ofstream) became
    // WritePipelineCache, which returns a BoolResultStr and stores the blob through Common::DDC::Put, so there
    // is no unchecked stream left to keep and the register has reached its success condition — zero rows.
    constexpr std::array<KnownSite, 0> kRegister{};

    // THE SAME REGISTER WITH NO ROWS IN IT, and it is not decoration: it is the compile-time half of the
    // paragraph above. It is what `kRegister` would be again if a new KEEP is ever added and retired, and this line
    // is what proves today — on every compiler the suite builds on, MSVC included — that the empty case
    // is a legal C++ type and that the two gates below still work over it. The tests use it; it is not a
    // declaration nobody reads.
    constexpr std::array<KnownSite, 0> kEmptyRegister{};

    std::string Norm( std::string p )
    {
        std::replace( p.begin(), p.end(), '\\', '/' );
        const std::size_t at = p.find( "./" );
        if ( at == 0 )
            p = p.substr( 2 );
        for ( const char* root : { "Desert/", "Editor/", "Runtime/", "Tools/" } )
        {
            const std::size_t r = p.find( root );
            if ( r != std::string::npos && ( r == 0 || p[r - 1] == '/' ) )
                return p.substr( r );
        }
        return p;
    }

    // Over a SPAN, not over kRegister directly, so the very same code runs against kEmptyRegister. A
    // gate that has only ever been exercised on a non-empty register is a gate nobody has checked can
    // survive the day its work is finished.
    bool InRegister( std::span<const KnownSite> reg, const Finding& f )
    {
        for ( const KnownSite& k : reg )
            if ( Norm( f.File ) == k.File && f.Function == k.Function )
                return true;
        return false;
    }

    // Rows that no longer describe a live site — the debt was paid, or the function was renamed. Either
    // way the row is now false.
    std::string StaleRows( std::span<const KnownSite> reg, const std::vector<Finding>& found )
    {
        std::string stale;
        for ( const KnownSite& k : reg )
        {
            const bool live = std::any_of( found.begin(), found.end(), [&k]( const Finding& f )
                                           { return Norm( f.File ) == k.File && f.Function == k.Function; } );
            if ( !live )
                stale += std::string( "\n    " ) + k.File + "  in " + k.Function + "()";
        }
        return stale;
    }

    std::vector<Finding> SweepTree()
    {
        const std::string root = RepoRoot();
        if ( root.empty() )
            return {};
        std::vector<Finding> all;
        for ( const fs::path& file : SourceFiles( root ) )
        {
            const std::string raw = ReadAll( file );
            if ( raw.find( "ofstream" ) == std::string::npos )
                continue;
            for ( const Finding& f : UnclosedWritesIn( file.string(), StripCommentsAndLiterals( raw ) ) )
                all.push_back( f );
        }
        return all;
    }
} // namespace

// --- The gate, forwards ---------------------------------------------------------------------------------
TEST( WriteVerdictCensus, NoNewWriteDecidesBeforeItHasFlushed )
{
    ASSERT_FALSE( RepoRoot().empty() ) << "could not locate the repository root from the working directory";

    std::string unlisted;
    for ( const Finding& f : SweepTree() )
        if ( !InRegister( kRegister, f ) )
            unlisted += "\n    " + Norm( f.File ) + ":" + std::to_string( f.Line ) + "  in " + f.Function +
                        "()  — stream '" + f.Stream + "'";

    EXPECT_TRUE( unlisted.empty() )
         << "a std::ofstream is written to and never closed before the function decides what to report."
         << unlisted
         << "\n  `write()` fills the filebuf; whether the bytes reached the disk is only known after the "
            "flush, and without an explicit close() that flush is the DESTRUCTOR — which runs after the "
            "success has already been returned. Whether the failure is caught therefore depends on the "
            "payload size, which is invisible at the site."
         << "\n  Do one of: call Common::Utils::FileSystem::WriteContentToFileAtomic / "
            "WriteBytesToFileAtomic (which also makes the write survivable — the whole tree writes files "
            "this way and there is no second primitive), or, if the code genuinely cannot link Common, "
            "close() explicitly and test the stream AFTER the close, as Tools/FbxMeshSplitter's "
            "WriteWholeFile does and says why."
         << "\n  If the site genuinely claims nothing to anybody, add it to kRegister with the argument "
            "spelled out: file, enclosing function, and a verdict naming the owning task and FIX or KEEP.";
}

// --- The gate, backwards: a debt register that cannot rot ------------------------------------------------
//
// A register whose rows are never re-checked becomes a list of things that USED to be true, and then it is
// read as licence. So every row must still describe a real site: if somebody fixes CloudTypeAsset and leaves
// the row, this goes red and tells them to delete it. That is also what makes the FIX verdicts above a
// worklist rather than prose — closing one is a two-line diff plus deleting its row.
TEST( WriteVerdictCensus, EveryRegisterRowStillDescribesARealSite )
{
    ASSERT_FALSE( RepoRoot().empty() ) << "could not locate the repository root from the working directory";
    const std::vector<Finding> found = SweepTree();

    const std::string stale = StaleRows( kRegister, found );

    EXPECT_TRUE( stale.empty() ) << "a register row no longer matches any site — the debt was paid, or the "
                                    "function was renamed. Either way the row is now false and must go."
                                 << stale;
}

TEST( WriteVerdictCensus, EveryRegisterRowNamesTheTaskThatOwnsIt )
{
    for ( const KnownSite& k : kRegister )
    {
        const std::string verdict = k.Verdict;
        EXPECT_NE( verdict.find( "Д31-D" ), std::string::npos )
             << k.File << " in " << k.Function << "(): the row's verdict names no owning task. An exception "
             << "with nobody's name on it is unreadable in a month — that is why ConfigOwnership checks the "
             << "same thing about its own debt register.";
        EXPECT_TRUE( verdict.find( "FIX" ) != std::string::npos || verdict.find( "KEEP" ) != std::string::npos )
             << k.File << " in " << k.Function << "(): a row must decide — FIX or KEEP. A sighting without a "
             << "verdict is what this census exists not to produce.";
    }
}

// --- The register must be able to be EMPTY ---------------------------------------------------------------
//
// A debt register's success condition is that it runs out of rows, and this project has twice shipped a
// register that could not express that: `constexpr T reg[] = {}` compiles on clang as a GNU extension and
// is rejected by MSVC (C2466). The type is a std::array for that reason, and this test is what makes the
// claim more than a comment — it runs both gates' logic over a register of length zero, on every compiler
// this suite is built with.
TEST( WriteVerdictCensus, TheRegisterCanBeEmptyAndBothGatesStillWork )
{
    static_assert( kEmptyRegister.empty(), "a zero-row register must be a legal value of the register type" );
    static_assert( std::size( kEmptyRegister ) == 0 );

    // Backwards: no rows means nothing can have gone stale, whatever the sweep found.
    EXPECT_EQ( StaleRows( kEmptyRegister, SweepTree() ), "" );
    EXPECT_EQ( StaleRows( kEmptyRegister, {} ), "" );

    // Forwards: with no rows, every finding is unlisted — which is exactly what a finished register must
    // do to the next unchecked write somebody adds.
    const Finding anything{ "Editor/Source/Whatever.cpp", "Save", "out", 1 };
    EXPECT_FALSE( InRegister( kEmptyRegister, anything ) );
    // A one-row register built here, because the real one is empty: the lookup must still see a row.
    constexpr auto oneRow = std::to_array<KnownSite>(
         { { "Editor/Source/Whatever.cpp", "Save", "Д31-D/KEEP: fixture row for the lookup itself" } } );
    EXPECT_TRUE( InRegister( oneRow, anything ) )
         << "the same lookup must still recognise a row that IS there — an always-false InRegister would "
            "pass the line above for the wrong reason";
}

// --- The gate can see the thing it bans ------------------------------------------------------------------
//
// A source-text gate that matches nothing looks exactly like a source-text gate that works, and this project
// has shipped both. So the matcher runs against the spellings that were REALLY in this tree — not invented
// ones — and against the shapes that must stay legal, because a gate that also flags honest code is a gate
// somebody deletes the first time it stops them.
TEST( WriteVerdictCensus, TheMatcherFindsTheFormItBansAndLeavesHonestWritesAlone )
{
    const auto flagged = []( const char* src )
    { return !UnclosedWritesIn( "x.cpp", StripCommentsAndLiterals( src ) ).empty(); };

    // The four-line idiom that stands in eleven files: open, check, write, check, report success.
    EXPECT_TRUE( flagged( "Result Save() {\n"
                          "  std::ofstream file( p, std::ios::binary | std::ios::trunc );\n"
                          "  if ( !file ) return Error();\n"
                          "  file.write( b.data(), b.size() );\n"
                          "  if ( !file ) return Error();\n"
                          "  return BOOLSUCCESS;\n}" ) )
         << "the checked-but-unflushed form — CloudTypeAsset, CloudLayoutAsset and three more — was missed";

    // SequencerPanel's spelling: operator<< and no post-write check whatsoever.
    EXPECT_TRUE( flagged( "std::string Save() {\n"
                          "  std::ofstream out( path, std::ios::binary );\n"
                          "  if ( !out ) return {};\n"
                          "  out << rfl::json::write( data );\n"
                          "  return path.string();\n}" ) )
         << "the operator<< form with no post-write check at all was missed";

    // PakFile's spelling: the stream handed to a helper that writes through it.
    EXPECT_TRUE( flagged( "size_t Finalize() {\n"
                          "  std::ofstream out( m_Path, std::ios::binary );\n"
                          "  if ( !out ) return 0;\n"
                          "  WritePod<uint32_t>( out, n );\n"
                          "  return out ? n : 0;\n}" ) )
         << "a stream written through a by-reference helper was missed — PakFile::Finalize's shape";

    // And the honest forms, which must stay legal.
    EXPECT_FALSE( flagged( "Result Save() {\n"
                           "  std::ofstream out( temp, std::ios::binary | std::ios::trunc );\n"
                           "  if ( !out ) return Error();\n"
                           "  out << content;\n"
                           "  out.close();\n"
                           "  if ( !out ) return Error();\n"
                           "  return BOOLSUCCESS;\n}" ) )
         << "WriteContentToFileAtomic's own shape must pass — it is the model this gate points people at";
    EXPECT_FALSE( flagged( "void Clear() { std::ofstream( k_LogFile, std::ios::trunc ).close(); }" ) )
         << "a temporary that truncates and writes nothing is not this gate's subject";
    EXPECT_FALSE( flagged( "void WritePod( std::ofstream& out, const T& v ) { out.write( p, n ); }" ) )
         << "a by-reference parameter belongs to whoever opened it; flagging it would ban every helper";
    EXPECT_FALSE( flagged( "void Touch() {\n"
                           "  std::ofstream file( path );\n"
                           "  if ( !file ) LOG_ERROR( \"no\" );\n}" ) )
         << "a stream that is never written to has nothing to flush and nothing to lie about";

    // The enclosing-function key must be the FUNCTION, not the `if` the declaration happens to sit in —
    // otherwise every register row would read "if" and the register would key on nothing.
    const auto found =
         UnclosedWritesIn( "x.cpp", StripCommentsAndLiterals( "int Extract() {\n"
                                                              "  for ( auto& k : keys ) {\n"
                                                              "    std::ofstream out( dst, std::ios::binary );\n"
                                                              "    out.write( d, n );\n"
                                                              "  }\n"
                                                              "  return 0;\n}" ) );
    ASSERT_EQ( found.size(), 1u ) << "PakTool::Extract's shape — a write inside a loop — was missed";
    EXPECT_EQ( found[0].Function, "Extract" )
         << "the key must name the function; a loop or an if between the declaration and the signature must "
            "be walked through, or the register keys on 'for' and stops distinguishing sites";

    // Both of these read back the WRONG name before they were handled, and both were caught by running the
    // matcher over the tree rather than by reading it — which is the argument for keeping them here.
    const auto keyOf = []( const char* src ) -> std::string
    {
        const auto f = UnclosedWritesIn( "x.cpp", StripCommentsAndLiterals( src ) );
        return f.empty() ? std::string( "<nothing found>" ) : f[0].Function;
    };
    EXPECT_EQ( keyOf( "PakWriter::PakWriter( const fs::path& p ) : m_Path( p ), m_Ok( false ) {\n"
                      "  std::ofstream out( m_Path, std::ios::binary );\n"
                      "  out.write( kMagic, 4 );\n}" ),
               "PakWriter::PakWriter" )
         << "a constructor's initialiser list must be walked back over — reading the name in front of the "
            "LAST initialiser keys PakFile's row on 'm_Path', which is a member, not a function";
    EXPECT_EQ( keyOf( "void VulkanLogicalDevice::SavePipelineCache() const {\n"
                      "  std::ofstream out( path, std::ios::binary );\n"
                      "  out.write( d, n );\n}" ),
               "VulkanLogicalDevice::SavePipelineCache" )
         << "trailing qualifiers must be stepped over — otherwise every const method's row is keyed on the "
            "word 'const', and all of them collide with each other";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
