// WHO OWNS THE BYTES AN `#include` HANDS TO shaderc — the relation Г19 exists to pin.
//
// `shaderc_include_result` carries RAW POINTERS plus lengths, and shaderc reads them long after
// GetInclude has returned, so something has to keep the bytes alive until ReleaseInclude. Until
// 2026-09-08 ShaderIncluder took `c_str()` from two heap strings and THEN moved those strings into the
// pair it stored in `user_data`. That is correct only while the move leaves the source's buffer where it
// was:
//
//   * a LONG string is heap-allocated, the move steals the pointer, and the captured `c_str()` happens to
//     stay valid. Every shipped shader path and every shader body is long, which is why nothing was seen.
//   * a SHORT string lives inside the object (small-string optimisation), the move COPIES it and clears
//     the source — so the captured pointer reads an EMPTY buffer while `content_length` still reports the
//     original size. The compile succeeds with the include silently blank.
//
// CORRECTNESS RESTED ON THE LENGTH OF THE DATA, NOT ON OWNERSHIP. So the load-bearing test here is the
// SHORT one: a few bytes of content, the case the old arrangement got wrong, asserted to come back
// verbatim. A test that included a real header would have passed over the defect for the same reason the
// engine did.
//
// Both heap strings also leaked on every include, and the diagnostic string of a FAILED include leaked
// too. Leaks are not observable from a test on this platform — LeakSanitizer is unsupported on macOS and
// `detect_leaks` is off in CI — so the second relation asserted here is the one that stands in for it:
// every result handed out is handed back, and after the change the result IS the only owner of its bytes,
// so "released" and "freed" are one event. ShaderIncluder::LiveIncludeResults is that count.
//
// Nothing here compiles a shader. The includer is driven directly, which is what lets the short case be
// constructed at all — shaderc would never ask for a three-byte header out of the shipped tree.

#include <Engine/Core/ShaderCompiler/Includer/ShaderIncluder.hpp>

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    // A temporary directory of this test's own, so the shipped shader tree is neither read nor written.
    // `shaderc_include_type_relative` resolves against the REQUESTING file's directory, which is what
    // makes a self-contained fixture possible: no engine paths, no VFS mount, no shader root.
    class TempTree
    {
    public:
        TempTree()
        {
            m_Root = std::filesystem::temp_directory_path() /
                     ( "desert_includer_" + std::to_string( static_cast<unsigned long long>( ::getpid() ) ) );
            std::filesystem::create_directories( m_Root );
        }

        ~TempTree()
        {
            std::error_code ec;
            std::filesystem::remove_all( m_Root, ec );
        }

        std::filesystem::path Write( const std::string& name, const std::string& content ) const
        {
            const std::filesystem::path path = m_Root / name;
            std::ofstream               out( path, std::ios::binary );
            out.write( content.data(), static_cast<std::streamsize>( content.size() ) );
            return path;
        }

        std::filesystem::path Requester() const
        {
            return m_Root / "main.shader";
        }

    private:
        std::filesystem::path m_Root;
    };

    // What shaderc would read out of the result: the pointer AND the length it was told, together. Read as
    // a pair on purpose — the defect this suite pins produced a valid pointer to an empty buffer beside a
    // non-zero length, and either half alone looks fine.
    std::string ContentOf( const shaderc_include_result* result )
    {
        if ( !result || !result->content )
            return {};
        return std::string( result->content, result->content_length );
    }

    std::string NameOf( const shaderc_include_result* result )
    {
        if ( !result || !result->source_name )
            return {};
        return std::string( result->source_name, result->source_name_length );
    }
} // namespace

// ── THE CASE THE OLD ARRANGEMENT GOT WRONG ─────────────────────────────────────────────────────────────
//
// Content SHORTER than every implementation's small-string threshold (libc++ holds 22 chars inline). Six
// bytes here, and the sugar translator leaves them alone, so what comes back must be exactly what was
// written. Before Г19 the pointer read an empty inline buffer while the length still said six.
TEST( ShaderIncluderOwnership, AnIncludeShorterThanTheSmallStringThresholdComesBackVerbatim )
{
    const TempTree    tree;
    const std::string tiny = "int a;";
    ASSERT_LT( tiny.size(), 23u ) << "the fixture is no longer short enough to exercise the defect";
    tree.Write( "tiny.glslh", tiny );

    Desert::Core::ShaderIncluder includer( tree.Requester() );

    shaderc_include_result* result =
         includer.GetInclude( "tiny.glslh", shaderc_include_type_relative, tree.Requester().string().c_str(), 0 );
    ASSERT_NE( result, nullptr );

    EXPECT_EQ( ContentOf( result ), tiny )
         << "the content shaderc was given is not the content of the file. A pointer taken before the "
            "string was moved into its owner reads an empty small-string buffer, and the include compiles "
            "as blank with nothing in the log.";
    EXPECT_EQ( result->content_length, tiny.size() );

    includer.ReleaseInclude( result );
    EXPECT_EQ( includer.LiveIncludeResults(), 0u );
}

// ── AND THE CASE THAT ALWAYS WORKED, so the fix is not a swap of one failure for another ───────────────
TEST( ShaderIncluderOwnership, AnIncludeLongerThanTheThresholdStillComesBackVerbatim )
{
    const TempTree tree;
    // Deliberately past the inline threshold and past it by a lot, since a heap string is the case that
    // used to survive by accident.
    const std::string big( 4096, 'x' );
    tree.Write( "big.glslh", big );

    Desert::Core::ShaderIncluder includer( tree.Requester() );

    shaderc_include_result* result =
         includer.GetInclude( "big.glslh", shaderc_include_type_relative, tree.Requester().string().c_str(), 0 );
    ASSERT_NE( result, nullptr );

    EXPECT_EQ( ContentOf( result ), big );
    EXPECT_EQ(
         NameOf( result ),
         ( std::filesystem::path( tree.Requester() ).parent_path() / "big.glslh" ).lexically_normal().string() )
         << "the source name is what shaderc puts in every diagnostic about this header, so a wrong or "
            "empty one turns a named error into an anonymous one";

    includer.ReleaseInclude( result );
    EXPECT_EQ( includer.LiveIncludeResults(), 0u );
}

// ── A FAILED INCLUDE IS OWNED THE SAME WAY ─────────────────────────────────────────────────────────────
//
// shaderc's convention for a failure is an empty source name and the message as the content. It used to
// be a `new std::string` nothing ever deleted, so every unresolved include leaked its own diagnostic —
// and an unresolved include is exactly what happens while somebody is editing a shader.
TEST( ShaderIncluderOwnership, AMissingIncludeReportsItselfAndIsReleasedLikeAnyOther )
{
    const TempTree               tree;
    Desert::Core::ShaderIncluder includer( tree.Requester() );

    shaderc_include_result* result = includer.GetInclude( "nothing_here.glslh", shaderc_include_type_relative,
                                                          tree.Requester().string().c_str(), 0 );
    ASSERT_NE( result, nullptr );

    EXPECT_TRUE( NameOf( result ).empty() ) << "shaderc reads an empty source name as 'this is an error'";
    EXPECT_NE( ContentOf( result ).find( "Cannot open include file" ), std::string::npos )
         << "the failure content is what the compiler prints, so it has to name the situation: got '"
         << ContentOf( result ) << "'";

    EXPECT_EQ( includer.LiveIncludeResults(), 1u )
         << "a failure result owns bytes exactly like a success and must be counted, or the leak it used "
            "to be is invisible again";

    includer.ReleaseInclude( result );
    EXPECT_EQ( includer.LiveIncludeResults(), 0u );
}

// ── THE COUNT IS THE OWNERSHIP CLAIM, so it is asserted over MANY results and not one ──────────────────
//
// Several results alive at once is the real shape of a compile: shaderc holds every include of a
// translation unit until the module is built. The count returning to zero is the only observable form of
// "every body was freed" available on a platform with no LeakSanitizer.
TEST( ShaderIncluderOwnership, EveryResultHandedOutIsHandedBack )
{
    const TempTree tree;
    tree.Write( "a.glslh", "int a;" );                 // short
    tree.Write( "b.glslh", std::string( 2048, 'b' ) ); // long
    tree.Write( "c.glslh", "" );                       // EMPTY, which is the shortest a real header can be

    Desert::Core::ShaderIncluder includer( tree.Requester() );

    shaderc_include_result* results[4] = {};
    const char* const       names[4]   = { "a.glslh", "b.glslh", "c.glslh", "missing.glslh" };
    for ( int i = 0; i < 4; ++i )
    {
        results[i] =
             includer.GetInclude( names[i], shaderc_include_type_relative, tree.Requester().string().c_str(), 0 );
        ASSERT_NE( results[i], nullptr ) << names[i];
        EXPECT_EQ( includer.LiveIncludeResults(), static_cast<std::size_t>( i + 1 ) );
    }

    // An EMPTY header is a legal header and must not read as a failure: its source name is the file's,
    // and only a MISSING one carries the empty name shaderc treats as an error.
    EXPECT_FALSE( NameOf( results[2] ).empty() )
         << "an empty header came back looking like a failed include, so a legitimately empty `.glslh` "
            "would abort the compile";
    EXPECT_TRUE( ContentOf( results[2] ).empty() );

    for ( int i = 0; i < 4; ++i )
        includer.ReleaseInclude( results[i] );

    EXPECT_EQ( includer.LiveIncludeResults(), 0u );
    std::printf( "[ShaderIncluderOwnership] 4 results handed out and back, live count %zu\n",
                 includer.LiveIncludeResults() );
}

// ── RELEASING NOTHING IS NOT A CRASH ───────────────────────────────────────────────────────────────────
TEST( ShaderIncluderOwnership, ReleasingANullResultIsSafe )
{
    const TempTree               tree;
    Desert::Core::ShaderIncluder includer( tree.Requester() );

    includer.ReleaseInclude( nullptr );
    EXPECT_EQ( includer.LiveIncludeResults(), 0u );
}
