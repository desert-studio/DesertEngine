// THE GATE FOR UNSPECIFIED ARGUMENT EVALUATION ORDER, AND IT IS A CENSUS BECAUSE NOTHING ELSE CAN BE.
//
// Read argument_order_scan.hpp for the mechanism and for why Г24's census could not have found the
// instance this suite was written for. In one line: that one opens one file and looks for one function
// NAME; the side effect here is spelled `std::move`, in a file it never opens.
//
// The live defect, `SceneRenderer::RegisterExternalPass`:
//
//     TrackRenderSystem( ExternalSystemKey( spec.Name ),
//                        std::make_shared<ExternalPassSystem>( this, std::move( spec ) ) );
//
// On clang the key is "External:<name>". On MSVC the move runs first, `spec.Name` is a moved-from string,
// and every external pass in the editor registers under the bare prefix "External:" — one key for all of
// them, each registration evicting the last, and `UnregisterExternalPass` never matching. Nothing about
// this machine can show it: clang's order happens to be the intended one, so a frame, a unit test and a
// full sweep are all green on both the defect and the fix.

#include "argument_order_scan.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace AO = Desert::Tests::ArgumentOrder;

namespace
{
    std::string Report( const std::vector<AO::Finding>& findings )
    {
        std::string out;
        for ( const AO::Finding& f : findings )
            out += "\n  " + f.File + ":" + std::to_string( f.Line ) + "  object '" + f.Object + "'\n      " +
                   f.Expression;
        return out;
    }
} // namespace

// ============================================================ 1. the census over the tree ===========

TEST( ArgumentOrder, NoArgumentListBothConsumesAnObjectAndReadsIt )
{
    const std::string root = AO::RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository from the working directory";

    AO::ScanCounts                 counts;
    const std::vector<AO::Finding> findings = AO::ScanRepository( root, counts );

    EXPECT_TRUE( findings.empty() )
         << findings.size() << " argument list(s) read an object that the same argument list moves away."
         << Report( findings )
         << "\n\nC++ does not order function arguments: clang evaluates them left to right, MSVC right to"
            " left, and both conform. Resolve the read into a named local FIRST — consecutive statements"
            " are sequenced, argument lists are not. No run on this machine can tell the two orders apart,"
            " which is why this is a census over the source text and not a test.";
}

// NOT VACUOUS, and this is the assertion Г24's census taught. A scan that stopped finding files, or a
// repository root that moved, would satisfy the census above by looking at nothing at all.
TEST( ArgumentOrder, TheCensusActuallyReadsTheTree )
{
    const std::string root = AO::RepoRoot();
    ASSERT_FALSE( root.empty() );

    AO::ScanCounts counts;
    (void)AO::ScanRepository( root, counts );

    EXPECT_GE( counts.Files, 900 ) << "the census walked only " << counts.Files
                                   << " source files. A root in SourceRoots() has been renamed, or the"
                                      " extension list no longer matches what the build compiles.";
    // 656 on the commit this was written against; the floor is set below it with room for churn, and it is
    // there because a scanner that stops recognising `std::move` satisfies the census above by looking at
    // nothing. Neither number may be raised to match a measurement — see the note in Desert's memory about
    // a gate that pins a COUNT being satisfiable by editing the count.
    EXPECT_GE( counts.MoveSites, 500 )
         << "the census looked at only " << counts.MoveSites
         << " std::move/std::forward call sites. Either the scanner stopped recognising them or it is"
            " now measuring nothing.";
}

// THE REACH IS THE HALF Г24 GOT WRONG, so it is asserted rather than assumed. A source root that is
// renamed and quietly stops being scanned reproduces exactly that failure: a green census over a tree it
// no longer covers.
TEST( ArgumentOrder, EverySourceRootItClaimsToCoverExists )
{
    const std::string root = AO::RepoRoot();
    ASSERT_FALSE( root.empty() );

    for ( const std::string& tree : AO::SourceRoots() )
        EXPECT_TRUE( std::filesystem::is_directory( std::filesystem::path( root ) / tree ) )
             << "SourceRoots() names '" << tree
             << "', which is not a directory. The census is silently covering less than it claims.";

    // And the two files that carry the known instances of this defect's history must be inside the reach,
    // by path: the emitter Г24 fixed, and the renderer this suite was written for.
    for ( const char* covered : { "Desert/Desert/Source/Engine/Graphic/SceneRenderer.cpp",
                                  "Editor/Source/Editor/Panels/NodeGraph/ShaderGraph.cpp" } )
    {
        bool inside = false;
        for ( const std::string& tree : AO::SourceRoots() )
            inside = inside || std::string( covered ).rfind( tree, 0 ) == 0;
        EXPECT_TRUE( inside ) << covered << " is outside every root this census walks.";
    }
}

// ============================================================ 2. the scanner sees the shape =========
//
// A census over files is only worth its green when the scanner has been shown to go RED. These are the
// forms in one string each, so the proof does not depend on a defect being left in the tree.

TEST( ArgumentOrder, TheScannerFindsTheShapeItIsFor )
{
    struct Case
    {
        const char* Name;
        const char* Code;
    };

    const Case positives[] = {
         { "the live defect, moved two calls deep",
           "void f() { Track( Key( spec.Name ), std::make_shared<P>( this, std::move( spec ) ) ); }" },
         { "read and move as two direct arguments",
           "void f() { m.insert_or_assign( info.Name, std::move( info ) ); }" },
         { "the read on the right of the move", "void f() { g( std::move( a ), a.size() ); }" },
         { "std::forward is the same hazard",
           "template <class T> void f( T&& t ) { g( t.Name, std::forward<T>( t ) ); }" },
    };

    for ( const Case& c : positives )
    {
        const std::vector<AO::Finding> found = AO::ScanText( c.Code, "<inline>" );
        EXPECT_EQ( found.size(), 1u ) << "the scanner did not see: " << c.Name << "\n  " << c.Code
                                      << Report( found );
    }

    const Case negatives[] = {
         { "a braced list is sequenced left to right",
           "void f() { return Medium{ std::move( m.Body ), m.Properties }; }" },
         { "one argument is one expression",
           "template <class... A> void f( A&&... a ) { g( std::forward<decltype( a )>( a )... ); }" },
         { "a conditional sequences its own condition", "void f() { g( inst ? std::move( inst ) : Make() ); }" },
         { "different objects", "void f() { g( name, std::move( system ) ); }" },
         { "a lone move", "void f() { v.push_back( std::move( x ) ); }" },
         { "a statement between the read and the move",
           "void f() { const auto key = Key( spec.Name ); Track( key, std::move( spec ) ); }" },
         { "a block is not an argument list",
           "void f() { auto cmd = std::move( m_Undo.back() ); m_Undo.pop_back(); }" },
    };

    for ( const Case& c : negatives )
    {
        const std::vector<AO::Finding> found = AO::ScanText( c.Code, "<inline>" );
        EXPECT_TRUE( found.empty() ) << "the scanner reported a form that the standard sequences: " << c.Name
                                     << "\n  " << c.Code << Report( found );
    }
}

// The blanker is shared with the settings census (Д33) rather than written a second time, and this pins
// the one property this scanner needs from it: a `;` or a `(` inside a string must not cut an expression.
TEST( ArgumentOrder, TextInsideALiteralIsNotCode )
{
    const std::vector<AO::Finding> found =
         AO::ScanText( "void f() { Log( \"spec.Name; (\", std::move( spec ) ); }", "<inline>" );
    EXPECT_TRUE( found.empty() ) << "a literal mentioning the object was read as a sibling argument"
                                 << Report( found );

    const std::vector<AO::Finding> real =
         AO::ScanText( "void f() { Log( \"text\", spec.Name, std::move( spec ) ); }", "<inline>" );
    EXPECT_EQ( real.size(), 1u ) << "blanking the literal also blanked the real read" << Report( real );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
