// "A FAILURE CANNOT BE READ AS A SUCCESS HOLDING NOTHING" — AS THREE RULES ABOUT THE SHAPE OF THE
// RESULT TYPES THEMSELVES, NOT AS A LIST OF CALL SITES.
//
// Ф1 made the file-read primitive stop aborting. Ф3 made the read CONTRACT guarded by the type rather
// than by somebody having read it — and the comment Ф3 left in FileSystem.hpp says, in as many words,
// what it could not buy: "`GetValue()` and `ExtractValue()` hand back a default-constructed T when the
// result is an error, so an unchecked unwrap still compiles and still yields the silent emptiness §1.4
// forbids — one method call away, with no diagnostic". Ф4 is that sentence being paid off.
//
// WHAT THE CENSUS OF `dev` (a67f876b) MEASURED BEFORE ANY CHANGE, run rather than estimated:
//
//   151 call sites of GetValue()/ExtractValue() outside the test tree.
//   126 of them test the result first. On inspection the ~19 that a naive proximity scan calls
//       unguarded are almost all FALSE POSITIVES of the scan, not of the code: the tree overwhelmingly
//       writes `if ( const auto text = Read( p ); text ) use( text.GetValue() );`, an init-statement
//       form the scan cannot see, or guards several lines up. THIS IS THE FINDING THAT SHAPED THE FIX:
//       the engine's authors do check. A gate over call sites would therefore have been almost pure
//       noise, and noise in a gate is how a gate stops being read.
//     9 sites unwrapped a TEMPORARY — `Foo().GetValue()`. That form is UNGUARDABLE BY CONSTRUCTION:
//       there is no variable in the caller's hands, so no reviewer, no census and no amount of care
//       could ever have added the missing check. Every one of the nine wrote a possibly-null handle or
//       shared_ptr into a member and carried on.
//
// So the fix is where the compiler can act and the gate is where drift can happen — and neither is a
// list of files. Deleting the rvalue overloads turned all nine into compile errors (they were fixed by
// the COMPILER, one build at a time, not by eye), and among them were two that had SURVIVED Г7's
// by-name pass over unchecked allocations for exactly the reason above: there was no name to look at.
//
// THE THREE RULES. Each is about the FORM of the two result types, so a third result type added
// tomorrow, or an overload quietly restored, goes red by itself:
//
//   RefQualifiedRule   every GetValue/ExtractValue declaration is lvalue-qualified (`&` / `const&`)
//                      AND its rvalue counterpart is `= delete`.
//   NotSilentRule      every failure path in those accessors calls ReportFailedUnwrap before handing
//                      back a default — the silence cannot come back one edit at a time.
//   ConstDefaultRule   a `static` default on a failure path is `const`. ResultWithCodes returned
//                      `static T default_value{}` from a NON-CONST overload: one process-wide object
//                      per T, shared by every failed unwrap in the binary, writable by any of them and
//                      read by all the others, with no synchronisation. That overload is gone; this
//                      rule is what stops it growing back.
//
// Comments and string literals are blanked first with the shared reader (Д33 — note its known bug: it
// used to go blind after a single quote in a char literal). This file names every symbol it is about,
// and a census that counted its own prose would be worthless.

#include "../SettingConsumers/setting_consumers_reader.hpp"

#include <Common/Core/ResultStr.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    // The two accessors that can hand back a value the caller never proved exists.
    const std::vector<std::string>& UnwrapNames()
    {
        static const std::vector<std::string> names = { "GetValue", "ExtractValue" };
        return names;
    }

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Common/Source/Common/Core/ResultWithCodes.hpp" );
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

    // The two files that DEFINE a result type. Deliberately named rather than discovered: a third
    // result type is a decision somebody must take, and taking it should mean adding a line here and
    // reading these three rules, not inheriting them silently.
    std::vector<fs::path> ResultTypeHeaders( const std::string& root )
    {
        return { fs::path( root ) / "Desert/Common/Source/Common/Core/ResultWithCodes.hpp",
                 fs::path( root ) / "ThirdParty/desert-shared/Include/DesertShared/ResultStr.hpp" };
    }

    int LineOf( const std::string& src, std::size_t at )
    {
        return 1 + static_cast<int>( std::count( src.begin(), src.begin() + at, '\n' ) );
    }

    struct Finding
    {
        std::string File;
        int         Line = 0;
        std::string What;
    };

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

    // A declaration of an unwrap accessor: the name, not reached through `.` or `->`, followed by an
    // empty parameter list. Returns the offsets of each such declaration in @p src.
    std::vector<std::size_t> FindDeclarations( const std::string& src, const std::string& name )
    {
        std::vector<std::size_t> out;
        for ( std::size_t at = src.find( name ); at != std::string::npos; at = src.find( name, at + 1 ) )
        {
            if ( at > 0 && Desert::Tests::ConsumerText::IsIdentChar( src[at - 1] ) )
                continue;
            const std::size_t after = at + name.size();
            if ( after < src.size() && Desert::Tests::ConsumerText::IsIdentChar( src[after] ) )
                continue;

            // Reached through an object => a call, not a declaration.
            std::size_t back = at;
            while ( back > 0 && std::isspace( static_cast<unsigned char>( src[back - 1] ) ) != 0 )
                --back;
            if ( back > 0 && src[back - 1] == '.' )
                continue;
            if ( back > 1 && src[back - 2] == '-' && src[back - 1] == '>' )
                continue;

            const std::size_t open = Desert::Tests::ConsumerText::SkipSpace( src, after );
            if ( open >= src.size() || src[open] != '(' )
                continue;
            const std::size_t close = MatchParen( src, open );
            if ( close == std::string::npos )
                continue;
            // These accessors take no arguments; anything else with the same name is not one of them.
            if ( src.find_first_not_of( " \t\r\n", open + 1 ) != close )
                continue;
            out.push_back( at );
        }
        return out;
    }

    // Everything between the `)` of the parameter list and the `{` or `;` that ends the declaration —
    // the ref-qualifier, cv-qualifiers, and `= delete` live here.
    std::string QualifierTail( const std::string& src, std::size_t close )
    {
        const std::size_t stop = src.find_first_of( "{;", close );
        if ( stop == std::string::npos )
            return {};
        return src.substr( close + 1, stop - close - 1 );
    }

    bool Contains( const std::string& haystack, const std::string& needle )
    {
        return haystack.find( needle ) != std::string::npos;
    }

    // The body of a declaration that has one, or an empty string for `= delete;`.
    std::string BodyOf( const std::string& src, std::size_t close )
    {
        const std::size_t brace = src.find_first_of( "{;", close );
        if ( brace == std::string::npos || src[brace] != '{' )
            return {};
        int depth = 0;
        for ( std::size_t i = brace; i < src.size(); ++i )
        {
            if ( src[i] == '{' )
                ++depth;
            else if ( src[i] == '}' && --depth == 0 )
                return src.substr( brace, i - brace + 1 );
        }
        return {};
    }
} // namespace

// -------------------------------------------------------------------------------------------------
// Rule 1: an unwrap accessor is lvalue-qualified, and its rvalue counterpart is deleted.
// -------------------------------------------------------------------------------------------------
TEST( ResultUnwrapCensus, RefQualifiedRule )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

    std::vector<Finding> findings;
    int                  declarations = 0;
    int                  deleted      = 0;

    for ( const fs::path& header : ResultTypeHeaders( root ) )
    {
        const std::string raw = ReadAll( header );
        ASSERT_FALSE( raw.empty() ) << "result type header is missing or empty: " << header.string();
        const std::string src = Desert::Tests::ConsumerText::StripCommentsAndLiterals( raw );

        for ( const std::string& name : UnwrapNames() )
        {
            for ( std::size_t at : FindDeclarations( src, name ) )
            {
                const std::size_t open  = Desert::Tests::ConsumerText::SkipSpace( src, at + name.size() );
                const std::size_t close = MatchParen( src, open );
                const std::string tail  = QualifierTail( src, close );

                ++declarations;
                if ( Contains( tail, "delete" ) )
                {
                    // The deleted overload must be the RVALUE one, or the deletion protects nothing.
                    if ( !Contains( tail, "&&" ) )
                    {
                        findings.push_back( { header.filename().string(), LineOf( raw, at ),
                                              name + " is deleted, but not on the rvalue overload" } );
                    }
                    ++deleted;
                    continue;
                }
                if ( !Contains( tail, "&" ) )
                {
                    findings.push_back( { header.filename().string(), LineOf( raw, at ),
                                          name + " is not ref-qualified, so it can unwrap a TEMPORARY: `Foo()." +
                                               name + "()` has no variable anyone could have checked" } );
                }
                else if ( Contains( tail, "&&" ) )
                {
                    findings.push_back( { header.filename().string(), LineOf( raw, at ),
                                          name + " is rvalue-qualified but not deleted" } );
                }
            }
        }
    }

    // Both types must actually have been scanned — a header that moved would otherwise pass this
    // test by containing nothing, which is the empty-successful-answer shape §1.4 forbids.
    EXPECT_GE( declarations, 4 ) << "expected at least GetValue+ExtractValue and their deleted rvalue "
                                    "overloads; found "
                                 << declarations;
    EXPECT_GE( deleted, 2 ) << "no rvalue overload is deleted, so unwrapping a temporary compiles again";

    std::ostringstream report;
    for ( const Finding& f : findings )
        report << "\n  " << f.File << ":" << f.Line << "  " << f.What;
    EXPECT_TRUE( findings.empty() ) << findings.size()
                                    << " unwrap accessor(s) can take a temporary:" << report.str();
}

// -------------------------------------------------------------------------------------------------
// Rule 2: a failure path never hands back a default in silence.
// -------------------------------------------------------------------------------------------------
TEST( ResultUnwrapCensus, NotSilentRule )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::vector<Finding> findings;
    int                  bodiesChecked = 0;

    for ( const fs::path& header : ResultTypeHeaders( root ) )
    {
        const std::string raw = ReadAll( header );
        const std::string src = Desert::Tests::ConsumerText::StripCommentsAndLiterals( raw );

        for ( const std::string& name : UnwrapNames() )
        {
            for ( std::size_t at : FindDeclarations( src, name ) )
            {
                const std::size_t open  = Desert::Tests::ConsumerText::SkipSpace( src, at + name.size() );
                const std::size_t close = MatchParen( src, open );
                const std::string body  = BodyOf( src, close );
                if ( body.empty() )
                    continue; // `= delete;` — no failure path to be silent on.

                ++bodiesChecked;
                // The failure path is the one guarded by !m_IsSuccess. If the body has one, it must
                // report before returning anything.
                if ( Contains( body, "m_IsSuccess" ) && !Contains( body, "ReportFailedUnwrap" ) )
                {
                    findings.push_back(
                         { header.filename().string(), LineOf( raw, at ),
                           name + " returns a default on failure WITHOUT calling ReportFailedUnwrap: a "
                                  "failure would read as a success holding nothing, in silence" } );
                }
            }
        }
    }

    EXPECT_GE( bodiesChecked, 2 ) << "no accessor bodies were scanned — the headers moved or the scan "
                                     "stopped recognising them";

    std::ostringstream report;
    for ( const Finding& f : findings )
        report << "\n  " << f.File << ":" << f.Line << "  " << f.What;
    EXPECT_TRUE( findings.empty() ) << findings.size() << " silent failure path(s):" << report.str();
}

// -------------------------------------------------------------------------------------------------
// Rule 3: a static default on a failure path is const.
// -------------------------------------------------------------------------------------------------
TEST( ResultUnwrapCensus, ConstDefaultRule )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    std::vector<Finding> findings;

    for ( const fs::path& header : ResultTypeHeaders( root ) )
    {
        const std::string raw = ReadAll( header );
        const std::string src = Desert::Tests::ConsumerText::StripCommentsAndLiterals( raw );

        for ( std::size_t at = src.find( "static" ); at != std::string::npos; at = src.find( "static", at + 1 ) )
        {
            const std::size_t eol  = src.find( '\n', at );
            const std::string line = src.substr( at, eol == std::string::npos ? eol : eol - at );

            // Only the T-valued defaults handed back from an unwrap. `static inline std::string
            // s_NoError` is an error MESSAGE, not a value the caller mistakes for one, and the
            // reporter's own function-local static is the mechanism doing the reporting.
            const bool isTemplatedDefault = Contains( line, " T " ) || Contains( line, " T{" );
            if ( !isTemplatedDefault )
                continue;
            if ( Contains( line, "const" ) )
                continue;

            findings.push_back(
                 { header.filename().string(), LineOf( raw, at ),
                   "a MUTABLE static default is handed back on a failure path: one process-wide object "
                   "per T, shared by every failed unwrap in the binary and writable by any of them" } );
        }
    }

    std::ostringstream report;
    for ( const Finding& f : findings )
        report << "\n  " << f.File << ":" << f.Line << "  " << f.What;
    EXPECT_TRUE( findings.empty() ) << findings.size() << " mutable shared default(s):" << report.str();
}

// -------------------------------------------------------------------------------------------------
// And the behaviour itself, because a text scan proves the shape and not the answer.
// -------------------------------------------------------------------------------------------------
TEST( ResultUnwrapCensus, AFailedUnwrapReportsThroughTheInstalledReporter )
{
    static std::string lastMessage;
    lastMessage.clear();

    Common::SetResultUnwrapReporter( []( const char* message ) { lastMessage = message; } );

    const Common::ResultStr<int> failed = Common::MakeError<int>( "the disk said no" );
    const int                    value  = failed.GetValue();

    EXPECT_EQ( value, 0 ) << "the default is still handed back — the change is that it is not silent";
    EXPECT_NE( lastMessage.find( "the disk said no" ), std::string::npos )
         << "the reporter did not receive the failure's own message; it got: " << lastMessage;

    // A SUCCESSFUL unwrap must say nothing at all, or the report becomes noise nobody reads.
    lastMessage.clear();
    const Common::ResultStr<int> ok = Common::MakeSuccess( 7 );
    EXPECT_EQ( ok.GetValue(), 7 );
    EXPECT_TRUE( lastMessage.empty() ) << "a successful unwrap reported: " << lastMessage;

    Common::SetResultUnwrapReporter( nullptr );
}
