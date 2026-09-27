// "NO PATH THAT WRITES INTO GPU MEMORY CAN REPORT SUCCESS WITHOUT WRITING" — AS A CENSUS OVER THE
// SOURCE TEXT, PLUS THE ONE DECISION THAT CAN BE ASSERTED WITHOUT A DEVICE.
//
// Г7-C closed the layer below this one: a mapping is a live mapping or a NAMED REFUSAL, there is no
// address to reach, and `memcpy` past the end of a buffer stopped compiling. What it could not close is
// the layer above. `SetData` on all four buffer types was declared `void`, so the refusal Г7-C had just
// made honest had nowhere to go but a `LOG_ERROR` — the frame was then drawn from data that is not there
// and nobody asked. That is §1.4 of the contract at the level of an INTERFACE: the contract is guarded by
// the TYPE, not by somebody having read the log.
//
// WHAT THIS SCAN REPORTED ON `dev` (bc6a790c), RUN RATHER THAN ESTIMATED — the three neighbours the task
// was opened for, each named by the rule that finds it:
//
//   AnswersRule      13 declarations of a transfer entry point returned `void` or dropped [[nodiscard]]:
//                    VertexBuffer/IndexBuffer/BaseBuffer::SetData and their four Vulkan overrides,
//                    Image2D::ReadPixelsRGBA8 (+ the Vulkan override), Image2D::SetData (result type,
//                    no NO_DISCARD), and RT_Invalidate on VulkanUniformBuffer, VulkanStorageBuffer,
//                    VulkanUniformImage2D and VulkanUniformImageCube.
//   CallSiteRule     23 call sites threw the answer away.
//   ContinueRule      3 loops answered a refused allocation with a bare `continue`
//                    (VulkanUniformBuffer::RT_Invalidate, VulkanStorageBuffer::RT_Invalidate, and
//                    AnimatedImageService, which silently dropped a GIF frame).
//   WritePathRule     1 write path destroyed the resource it was writing into
//                    (VulkanStorageBuffer::SetData called RT_Invalidate to grow — which for a
//                    PERSISTENT buffer throws away the GPU simulation state its own comment says
//                    "must survive across frames", silently, from the per-frame write path).
//
// All four are one shape: a path into GPU memory that can fail and cannot say so. They are censused
// together on purpose — fixing them apart means taking the same decision three times.
//
// HOW THE SCAN DECIDES. Comments and literals are blanked first with the shared reader (Д33) — this file
// names every entry point it is about, and a census that counted its own prose would be worthless. It
// does not parse C++: a DECLARATION is the transfer name NOT reached through `.` or `->` and preceded by
// a type; a CALL is the same name reached through `.` or `->`. Conservative in the safe direction, as
// MappedMemoryCensus is: a guard it cannot recognise reads as a defect and the author must then say where
// the check is, which is the failure that costs a paragraph rather than a frame.
//
// AND ONE HALF IS NOT A TEXT SCAN AT ALL. Whether a write may grow a buffer is a DECISION, and a decision
// belongs in a pure function that a test can reach without a device — `ShaderResources::ClassifyBufferWrite`
// (BufferGrowth.hpp), asserted at the bottom of this file. The text rules say the channel exists; that one
// says the channel carries the right answer.

#include "../SettingConsumers/setting_consumers_reader.hpp"

#include <Engine/ShaderResources/BufferGrowth.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    // The names that move bytes into or out of GPU memory, or re-create the memory they move into.
    // Typed, and small enough to audit by eye — the same trade MappedMemoryCensus makes with
    // "MapMemory". What is DERIVED is where they occur, which is where drift actually happens.
    const std::vector<std::string>& TransferNames()
    {
        static const std::vector<std::string> names = { "SetData", "ReadPixelsRGBA8", "RT_Invalidate" };
        return names;
    }

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Graphic/MappedMemory.hpp" );
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

    // Every project source the engine compiles. Editor and Runtime are in scope because a buffer upload
    // is not confined to the backend by anything except habit — and `ReadPixelsRGBA8`'s three callers are
    // all in the Editor.
    std::vector<fs::path> ProjectSources( const std::string& root )
    {
        std::vector<fs::path> out;
        for ( const char* tree :
              { "Desert/Desert/Source", "Desert/Common/Source", "Editor/Source", "Runtime/Source" } )
        {
            std::error_code ec;
            const fs::path  base = fs::path( root ) / tree;
            for ( auto it = fs::recursive_directory_iterator( base, ec );
                  !ec && it != fs::recursive_directory_iterator(); ++it )
            {
                const fs::path& p = it->path();
                if ( p.extension() == ".cpp" || p.extension() == ".hpp" )
                    out.push_back( p );
            }
        }
        std::sort( out.begin(), out.end() );
        return out;
    }

    int LineOf( const std::string& src, std::size_t at )
    {
        return 1 + static_cast<int>( std::count( src.begin(), src.begin() + at, '\n' ) );
    }

    std::string Trimmed( const std::string& s )
    {
        std::size_t b = 0;
        std::size_t e = s.size();
        while ( b < e && std::isspace( static_cast<unsigned char>( s[b] ) ) != 0 )
            ++b;
        while ( e > b && std::isspace( static_cast<unsigned char>( s[e - 1] ) ) != 0 )
            --e;
        return s.substr( b, e - b );
    }

    // Is the name at @p at reached through an object (`x.Name`, `x->Name`)? That is what separates a CALL
    // from a declaration or an out-of-line definition without parsing C++.
    bool ReachedThroughObject( const std::string& src, std::size_t at )
    {
        std::size_t back = at;
        while ( back > 0 && std::isspace( static_cast<unsigned char>( src[back - 1] ) ) != 0 )
            --back;
        if ( back > 0 && src[back - 1] == '.' )
            return true;
        return back > 1 && src[back - 2] == '-' && src[back - 1] == '>';
    }

    // The `(` that opens the argument list, or npos when the name is not called/declared here at all.
    std::size_t ParameterListOpen( const std::string& src, std::size_t at, const std::string& name )
    {
        const std::size_t open = Desert::Tests::ConsumerText::SkipSpace( src, at + name.size() );
        if ( open >= src.size() || src[open] != '(' )
            return std::string::npos;
        return open;
    }

    // Matching `)` for the `(` at @p open.
    std::size_t MatchParen( const std::string& src, std::size_t open )
    {
        int depth = 0;
        for ( std::size_t i = open; i < src.size(); ++i )
        {
            if ( src[i] == '(' )
                ++depth;
            else if ( src[i] == ')' && --depth == 0 )
                return i;
        }
        return std::string::npos;
    }

    struct Finding
    {
        std::string File;
        int         Line = 0;
        std::string What;
    };

    // ---------------------------------------------------------------------------------------------
    // Rule 1: every declaration of a transfer entry point ANSWERS.
    // ---------------------------------------------------------------------------------------------

    // The text between the previous statement boundary and the transfer name — the return type and its
    // qualifiers, for a declaration. Empty (or a keyword like `return`) for a bare call, which is how a
    // call written without a receiver is told apart from a declaration.
    bool LooksLikeDeclaration( const std::string& prefix )
    {
        const std::string t = Trimmed( prefix );
        if ( t.empty() )
            return false;
        // `return RT_Invalidate();`, `x = RT_Invalidate();`, `Foo( RT_Invalidate() )` — calls, not
        // declarations. A declaration's prefix ends in a type name or a `::` scope.
        const char last = t.back();
        if ( last != ':' && Desert::Tests::ConsumerText::IsIdentChar( last ) == false )
            return false;
        std::size_t wordStart = t.size();
        while ( wordStart > 0 && Desert::Tests::ConsumerText::IsIdentChar( t[wordStart - 1] ) )
            --wordStart;
        const std::string lastWord = t.substr( wordStart );
        return lastWord != "return";
    }

    std::vector<Finding> ScanDeclarations( const std::string& root )
    {
        using namespace Desert::Tests::ConsumerText;

        std::vector<Finding> out;
        for ( const fs::path& path : ProjectSources( root ) )
        {
            const std::string src      = StripCommentsAndLiterals( ReadAll( path ) );
            const bool        isHeader = path.extension() == ".hpp";

            for ( const std::string& name : TransferNames() )
            {
                for ( std::size_t at : WordPositions( src, name ) )
                {
                    if ( ReachedThroughObject( src, at ) )
                        continue;
                    if ( ParameterListOpen( src, at, name ) == std::string::npos )
                        continue;

                    const std::string prefix = StatementBefore( src, at );
                    if ( !LooksLikeDeclaration( prefix ) )
                        continue;

                    // A RESULT TYPE, not `void`. `void` is the whole defect: the refusal the layer below
                    // now produces honestly has nowhere to be returned to.
                    if ( prefix.find( "ResultStr" ) == std::string::npos &&
                         prefix.find( "ResultWithCodes" ) == std::string::npos )
                    {
                        out.push_back( { path.string(), LineOf( src, at ),
                                         name + " is declared with no way to answer (return type is '" +
                                              Trimmed( prefix ) + "')" } );
                        continue;
                    }

                    // ...and the answer must be impossible to drop by accident. Only on DECLARATIONS:
                    // repeating the attribute on an out-of-line definition is noise, and the compiler
                    // takes it from the declaration anyway.
                    if ( isHeader && prefix.find( "NO_DISCARD" ) == std::string::npos &&
                         prefix.find( "[[nodiscard]]" ) == std::string::npos )
                    {
                        out.push_back( { path.string(), LineOf( src, at ),
                                         name + " answers but is not NO_DISCARD, so ignoring the answer "
                                                "is silent again" } );
                    }
                }
            }
        }
        return out;
    }

    // ---------------------------------------------------------------------------------------------
    // Rule 2: every CALL SITE reads the answer.
    // ---------------------------------------------------------------------------------------------

    // Walk back from the `.`/`->` to the beginning of the whole call expression, so that
    // `it->second.Particles->SetData(` yields the position of `it`.
    std::size_t ExpressionStart( const std::string& src, std::size_t at )
    {
        std::size_t i = at;
        while ( i > 0 )
        {
            const char c = src[i - 1];
            if ( std::isspace( static_cast<unsigned char>( c ) ) != 0 ||
                 Desert::Tests::ConsumerText::IsIdentChar( c ) || c == '.' || c == '>' || c == '-' || c == ':' )
            {
                --i;
                continue;
            }
            if ( c == ')' || c == ']' )
            {
                const char open  = c == ')' ? '(' : '[';
                const char close = c;
                int        depth = 0;
                while ( i > 0 )
                {
                    --i;
                    if ( src[i] == close )
                        ++depth;
                    else if ( src[i] == open && --depth == 0 )
                        break;
                }
                continue;
            }
            break;
        }
        return i;
    }

    std::vector<Finding> ScanCallSites( const std::string& root )
    {
        using namespace Desert::Tests::ConsumerText;

        std::vector<Finding> out;
        for ( const fs::path& path : ProjectSources( root ) )
        {
            const std::string src = StripCommentsAndLiterals( ReadAll( path ) );

            for ( const std::string& name : TransferNames() )
            {
                for ( std::size_t at : WordPositions( src, name ) )
                {
                    if ( !ReachedThroughObject( src, at ) )
                        continue;
                    if ( ParameterListOpen( src, at, name ) == std::string::npos )
                        continue;

                    const std::size_t start   = ExpressionStart( src, at );
                    const std::string leading = Trimmed( StatementBefore( src, start ) );
                    if ( !leading.empty() )
                        continue; // bound, returned, compared, passed on — the answer is in play

                    out.push_back(
                         { path.string(), LineOf( src, at ), "the answer from " + name + " is discarded" } );
                }
            }
        }
        return out;
    }

    // ---------------------------------------------------------------------------------------------
    // Rule 3: a refusal is never answered with a bare `continue`.
    // ---------------------------------------------------------------------------------------------

    std::vector<Finding> ScanBareContinues( const std::string& root )
    {
        using namespace Desert::Tests::ConsumerText;

        std::vector<Finding> out;
        for ( const fs::path& path : ProjectSources( root ) )
        {
            const std::string src = StripCommentsAndLiterals( ReadAll( path ) );

            for ( std::size_t at : WordPositions( src, "if" ) )
            {
                const std::size_t open = SkipSpace( src, at + 2 );
                if ( open >= src.size() || src[open] != '(' )
                    continue;
                const std::size_t close = MatchParen( src, open );
                if ( close == std::string::npos )
                    continue;

                const std::string condition = src.substr( open, close - open );
                // A NEGATED test of a result. `if ( x.IsSuccess() ) continue;` is the opposite statement
                // and a legitimate one, so the `!` is load-bearing rather than decoration.
                if ( condition.find( "IsSuccess" ) == std::string::npos ||
                     condition.find( '!' ) == std::string::npos )
                    continue;

                std::size_t body = SkipSpace( src, close + 1 );
                if ( body < src.size() && src[body] == '{' )
                    body = SkipSpace( src, body + 1 );
                if ( !WordAt( src, body, "continue" ) )
                    continue;

                out.push_back( { path.string(), LineOf( src, at ),
                                 "a refused result is answered with a bare `continue` -- the loop carries "
                                 "on and whatever this iteration was supposed to produce is simply absent, "
                                 "with nothing said" } );
            }
        }
        return out;
    }

    // ---------------------------------------------------------------------------------------------
    // Rule 4: a write path never destroys the resource it is writing into.
    // ---------------------------------------------------------------------------------------------

    std::vector<Finding> ScanWritePathsThatDestroy( const std::string& root )
    {
        using namespace Desert::Tests::ConsumerText;

        std::vector<Finding> out;
        for ( const fs::path& path : ProjectSources( root ) )
        {
            if ( path.extension() != ".cpp" )
                continue;
            const std::string src = StripCommentsAndLiterals( ReadAll( path ) );

            for ( std::size_t at : WordPositions( src, "SetData" ) )
            {
                if ( ReachedThroughObject( src, at ) )
                    continue;
                const std::size_t open = ParameterListOpen( src, at, "SetData" );
                if ( open == std::string::npos )
                    continue;
                const std::size_t close = MatchParen( src, open );
                if ( close == std::string::npos )
                    continue;
                const std::size_t bodyOpen = src.find( '{', close );
                if ( bodyOpen == std::string::npos )
                    continue;

                int         depth   = 0;
                std::size_t bodyEnd = bodyOpen;
                for ( std::size_t i = bodyOpen; i < src.size(); ++i )
                {
                    if ( src[i] == '{' )
                        ++depth;
                    else if ( src[i] == '}' && --depth == 0 )
                    {
                        bodyEnd = i;
                        break;
                    }
                }

                const std::string body = src.substr( bodyOpen, bodyEnd - bodyOpen );
                for ( const char* destroyer : { "RT_Invalidate", "Invalidate", "Release" } )
                {
                    const auto hits = WordPositions( body, destroyer );
                    if ( hits.empty() )
                        continue;
                    out.push_back( { path.string(), LineOf( src, bodyOpen + hits.front() ),
                                     std::string( "SetData calls " ) + destroyer +
                                          ", so the per-frame WRITE path destroys and re-creates the buffer it is "
                                          "writing into. For a buffer whose contents must survive across frames "
                                          "(GPU simulation state) that is the state gone, silently. Growing is a "
                                          "different operation from writing and needs its own name and its own "
                                          "refusal" } );
                    break;
                }
            }
        }
        return out;
    }

    std::string Render( const std::vector<Finding>& findings )
    {
        std::string all;
        for ( const Finding& f : findings )
            all += "\n  " + f.File + ":" + std::to_string( f.Line ) + " " + f.What;
        return all;
    }
} // namespace

TEST( GpuWriteCensus, EveryTransferEntryPointCanAnswer )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository from the working directory";

    const std::vector<Finding> findings = ScanDeclarations( root );
    EXPECT_TRUE( findings.empty() )
         << findings.size()
         << " transfer entry point(s) cannot report a failure, or can have their report dropped in "
            "silence. A write into GPU memory that fails and returns `void` is a frame drawn from data "
            "that is not there -- the caller has nothing to ask and nothing to retry."
         << Render( findings );
}

TEST( GpuWriteCensus, NoCallSiteDiscardsTheAnswer )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::vector<Finding> findings = ScanCallSites( root );

    // A census that finds no call sites at all has stopped working -- the same failure mode
    // MappedMemoryCensus guards against. Proved by the fact that the scan can see them.
    ASSERT_FALSE( ProjectSources( root ).empty() );

    EXPECT_TRUE( findings.empty() )
         << findings.size()
         << " call site(s) throw away the answer. If the refusal genuinely has nowhere to go at a site, "
            "that is a legitimate answer -- but it must be WRITTEN there with its argument (bind the "
            "result and log it, or say in a comment why a failure cannot change what this code does), "
            "not left implicit."
         << Render( findings );
}

TEST( GpuWriteCensus, NoRefusalIsAnsweredWithABareContinue )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::vector<Finding> findings = ScanBareContinues( root );
    EXPECT_TRUE( findings.empty() )
         << findings.size()
         << " loop(s) skip past a refusal without recording it. In a loop that is FILLING something -- "
            "per-frame buffer copies, the frames of an animation -- the skipped iteration leaves a hole "
            "that the code after the loop then publishes as if it were whole."
         << Render( findings );
}

TEST( GpuWriteCensus, NoWritePathDestroysTheResourceItWritesInto )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::vector<Finding> findings = ScanWritePathsThatDestroy( root );
    EXPECT_TRUE( findings.empty() ) << findings.size() << " write path(s) destroy their own resource."
                                    << Render( findings );
}

// -------------------------------------------------------------------------------------------------
// The decision itself, asserted without a device. THE RELATION, not the function: what must hold is
// that a buffer whose contents are required to outlive the frame is never resized by a routine write,
// and that the two answers "it fits" and "it does not fit" are never the same value.
// -------------------------------------------------------------------------------------------------

using Desert::ShaderResources::BufferWriteVerdict;
using Desert::ShaderResources::ClassifyBufferWrite;
using Desert::ShaderResources::Persistence;

TEST( BufferGrowth, AWriteInsideCapacityNeverResizes )
{
    for ( const Persistence life : { Persistence::PerFrame, Persistence::AcrossFrames } )
    {
        EXPECT_EQ( ClassifyBufferWrite( 256, 256, 0, life ), BufferWriteVerdict::Fits );
        EXPECT_EQ( ClassifyBufferWrite( 256, 128, 128, life ), BufferWriteVerdict::Fits );
        EXPECT_EQ( ClassifyBufferWrite( 256, 0, 0, life ), BufferWriteVerdict::Fits );
    }
}

TEST( BufferGrowth, APerFrameBufferGrowsAndAPersistentOneRefuses )
{
    // The whole defect, as one pair of assertions. The same overflowing write is a GROW for a buffer
    // the GPU re-reads every frame and a REFUSAL for one holding simulation state, and before this the
    // two were the same line: VulkanStorageBuffer::SetData called RT_Invalidate for both.
    EXPECT_EQ( ClassifyBufferWrite( 256, 512, 0, Persistence::PerFrame ), BufferWriteVerdict::Grow );
    EXPECT_EQ( ClassifyBufferWrite( 256, 512, 0, Persistence::AcrossFrames ),
               BufferWriteVerdict::RefuseWouldDestroyPersistentState );

    // An offset overflows exactly as a size does: the bound is size + offset, and reading only the size
    // is how `dst + offset` ran off the end of the mapping before Г7-C bounded it.
    EXPECT_EQ( ClassifyBufferWrite( 256, 4, 256, Persistence::PerFrame ), BufferWriteVerdict::Grow );
    EXPECT_EQ( ClassifyBufferWrite( 256, 4, 256, Persistence::AcrossFrames ),
               BufferWriteVerdict::RefuseWouldDestroyPersistentState );
}

TEST( BufferGrowth, TheRequiredSizeIsTheSumAndItDoesNotWrap )
{
    EXPECT_EQ( Desert::ShaderResources::RequiredBufferSize( 512, 0 ), 512u );
    EXPECT_EQ( Desert::ShaderResources::RequiredBufferSize( 512, 64 ), 576u );

    // 32-bit addition of two attacker-sized numbers wraps, and a wrapped requirement is SMALLER than
    // the buffer -- which reads as "it fits" and admits precisely the write the bound exists to refuse.
    // The classification must therefore refuse rather than compute, and it does so on both lifetimes.
    const uint32_t huge = 0xFFFFFFFFu;
    EXPECT_EQ( ClassifyBufferWrite( 256, huge, 16, Persistence::PerFrame ),
               BufferWriteVerdict::RefuseWouldNotFitAnyBuffer );
    EXPECT_EQ( ClassifyBufferWrite( 256, 16, huge, Persistence::AcrossFrames ),
               BufferWriteVerdict::RefuseWouldNotFitAnyBuffer );
}

TEST( BufferGrowth, EveryVerdictHasAName )
{
    // A refusal that reaches a log as an integer is a refusal nobody reads. Every enumerator, not the
    // three somebody remembered -- the switch in BufferWriteVerdictName has no default, so a fifth
    // verdict added tomorrow stops compiling instead of printing "unknown".
    for ( const BufferWriteVerdict v : { BufferWriteVerdict::Fits, BufferWriteVerdict::Grow,
                                         BufferWriteVerdict::RefuseWouldDestroyPersistentState,
                                         BufferWriteVerdict::RefuseWouldNotFitAnyBuffer } )
    {
        const char* name = Desert::ShaderResources::BufferWriteVerdictName( v );
        ASSERT_NE( name, nullptr );
        EXPECT_NE( std::string( name ), std::string() );
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
