#pragma once

// Does a source file REALLY read a field of a particular type, or does it merely contain the word?
//
// This header is the whole difference between the census this suite used to be and the one it is now.
// The old WIRED row asserted that the named consumer file mentions the field's NAME somewhere. That is
// vacuous whenever a field name is shared by several components, and it was measured to be vacuous: task
// У3 deleted the canvas's background draw from UICanvasRenderer2D.cpp and the census stayed GREEN,
// because the button, the panel and the image each have a `Sprite` of their own and kept the word alive
// in that file. A census that cannot see a setting die is a census that certifies nothing.
//
// So what is asserted now is an ANCHORED READ: a member access of the field whose receiver is tied,
// inside that same file, to THIS type. Two ways a receiver gets tied:
//
//   * it is an ANCHOR itself - the type's own name, the ECS wrapper whose `Data` member holds it, or a
//     named accessor that returns it (`scene->GetSettings().ShowGrid`);
//   * it is an IDENTIFIER the file binds to an anchor - a parameter (`const ECS::UICanvasData& d`), a
//     local (`const auto& canvasData = reg.get<ECS::UICanvasComponent>( e ).Data;`), an alias of one, or
//     a parameter of the lambda handed to an entt view's `each`.
//
// WHY THE RECEIVER IS DERIVED RATHER THAN SPELLED IN THE ROW. A row that spelled `canvasData.Sprite`
// would be exact, and it would also go red the day somebody renames a local variable - which is an
// honest edit that changes nothing about whether the setting is read. Deriving the receiver from the
// same file means the rename moves the binding and the read together and the census does not notice,
// while DELETING the read leaves no anchored access at all and the census does.
//
// A write is not a read. `d.Gravity = glm::vec3( ... )` is a preset author filling the component in, not
// a consumer of the value, so a member access immediately followed by a plain `=` does not count. That
// is what stops a Details widget or a preset table from certifying a field nobody downstream ever reads.
//
// The matching is textual on purpose: it lets the audit check a render pass, an ECS system and an editor
// panel without a GPU, a registry or a link against the engine. It is deliberately conservative in the
// direction that matters - it can fail to see an exotic read (and then the row's author must name a file
// where the read is plain), but the shapes it accepts all really are reads of that type's field.

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

namespace Desert::Tests::ConsumerText
{
    inline bool IsIdentChar( char c )
    {
        return std::isalnum( static_cast<unsigned char>( c ) ) != 0 || c == '_';
    }

    // ----------------------------------------------------------------------------------------------
    // Blanking the things that are not code
    // ----------------------------------------------------------------------------------------------
    //
    // Comments and literals are removed before anything else. A file that only NAMES the type in a
    // comment (and `Components.hpp` mentions half the engine in comments) must not thereby acquire
    // receivers, and a read that lives in a commented-out line is not a read.
    //
    // Д33 REBUILT THIS, because the old version knew about `"..."` and about comments and about nothing
    // else, and that hole took a whole gate's sight away without anyone being able to see that it had.
    // The failure had a very particular shape, and it is the reason the invariants below are worth their
    // cost:
    //
    //   * A CHARACTER LITERAL HOLDING A QUOTE ATE THE REST OF THE FILE. `c.Peek() == '"'` is ordinary
    //     C++ and it appears in DShaderParser.cpp. The old scanner saw the `"` inside it, opened a
    //     "string literal", and closed it at the next quote HUNDREDS OF LINES LATER — deleting
    //     everything in between. М9 found it because six rows of a new census went red for a defect in
    //     the census; the two censuses already shipped could not have told anybody, because a deletion
    //     that hides a read makes a gate answer "nobody reads this" in exactly the confident voice it
    //     uses when that is true (§1.4 of the contract, applied to the checker itself).
    //   * AND IT LIED IN BOTH DIRECTIONS. Every stray quote flips the scanner's parity, so past the
    //     first one the CONTENTS of real string literals were emitted as if they were code — a log line
    //     naming a field could then certify that the field is read.
    //   * RAW STRINGS were never handled at all. `R"(...)"` survived by accident as long as it contained
    //     no quote of its own, and our raw strings hold C++ and JSON, which do.
    //
    // Two invariants close the class rather than the instance, and `TheReaderSeesCodeAfterEveryLiteral
    // Form` asserts both:
    //
    //   1. NOTHING RUNS AWAY. An ordinary string and a character literal are bounded by their own line
    //      (a line splice aside), so an unterminated one costs that line and not the file. Only a block
    //      comment and a raw string may span lines, and both have an explicit terminator to look for.
    //   2. THE RESULT IS THE SAME LENGTH AS THE INPUT. Every removed byte becomes a space and every
    //      newline survives, so an offset into the output still names the same line of the original.
    //      That is what lets the whole-tree censuses (PureVirtualCensus, DeviceLostCensus) report a
    //      line number, and it is why they no longer carry a private copy of this function.

    // The characters that may precede a literal's opening quote, as ONE token: the encoding prefix. `R`
    // is what makes a string raw. Anything else in front of a quote is not a prefix, and the run is left
    // alone so that it stays visible as code.
    inline std::size_t LiteralPrefixStart( const std::string& s, std::size_t quote, bool& raw )
    {
        raw               = false;
        std::size_t start = quote;
        while ( start > 0 && IsIdentChar( s[start - 1] ) )
            --start;
        if ( start == quote )
            return quote;

        const std::string run = s.substr( start, quote - start );
        for ( const char* ok : { "L", "u", "U", "u8", "R", "LR", "uR", "UR", "u8R" } )
        {
            if ( run != ok )
                continue;
            raw = run.back() == 'R';
            return start;
        }
        return quote;
    }

    // `1'000'000` and `0x1F'FF'00`: C++14's digit separator, not the start of a character literal. Told
    // apart by what the run behind the quote BEGINS with — an encoding prefix (`L'x'`, `u8'x'`) begins
    // with a letter, a numeric literal with a digit. The run is scanned back over EARLIER SEPARATORS as
    // well, or the second quote of `0x1F'FF'00` sees a run of `FF` and calls itself a character literal.
    inline bool IsDigitSeparator( const std::string& s, std::size_t quote )
    {
        std::size_t start = quote;
        while ( start > 0 && ( IsIdentChar( s[start - 1] ) || s[start - 1] == '\'' ) )
            --start;
        return start < quote && std::isdigit( static_cast<unsigned char>( s[start] ) ) != 0;
    }

    // ONE PASS, TWO ENTRY POINTS. `keepLiterals` exists because a census whose SUBJECT is a string
    // literal cannot use a reader that blanks string literals — the resolver census (I13) asks whether
    // `ComponentRegistry.cpp` contains `type == "FontAsset"`, and the answer is in the literal. Reading
    // RAW source instead would make a literal inside a COMMENT satisfy the census, which is a false
    // PASS: a census that certifies a type as handled when it is not is worse than no census. So the
    // comments still go and the literals stay, through this same reader rather than a second opinion
    // about where a literal begins and ends.
    inline std::string StripCommentsMaybeLiterals( const std::string& src, bool keepLiterals )
    {
        std::string out( src.size(), ' ' );

        const auto blank = [&out, &src]( std::size_t from, std::size_t to )
        {
            for ( std::size_t p = from; p < to && p < src.size(); ++p )
                out[p] = src[p] == '\n' ? '\n' : ' ';
        };

        const auto keep = [&out, &src]( std::size_t from, std::size_t to )
        {
            for ( std::size_t p = from; p < to && p < src.size(); ++p )
                out[p] = src[p];
        };

        // The end of an ordinary string or character literal opened at `open`, or npos when the line
        // ends first. A backslash escapes the next byte, including a newline (a line splice, which is
        // the one way a non-raw literal legitimately continues).
        const auto closingQuote = [&src]( std::size_t open, char quote ) -> std::size_t
        {
            for ( std::size_t j = open + 1; j < src.size(); )
            {
                if ( src[j] == '\\' && j + 1 < src.size() )
                {
                    j += 2;
                    continue;
                }
                if ( src[j] == '\n' )
                    return std::string::npos;
                if ( src[j] == quote )
                    return j + 1;
                ++j;
            }
            return std::string::npos;
        };

        std::size_t i = 0;
        while ( i < src.size() )
        {
            if ( src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/' )
            {
                // A `//` comment ENDING IN A BACKSLASH swallows the next line too: the compiler splices
                // the lines before it looks for comments at all. This is the mirror of the defect that
                // caused the rebuild, and the reason to close it here rather than to note it — instead
                // of hiding a read that exists, getting it wrong SHOWS a read the compiler never
                // compiles, and the census then certifies a setting that is dead. There is no such
                // comment in the tree today (measured: zero, over the 926 project sources), so this
                // costs nothing now and cannot be introduced later without the reader following it.
                std::size_t j = i;
                while ( j < src.size() )
                {
                    if ( src[j] != '\n' )
                    {
                        ++j;
                        continue;
                    }
                    std::size_t back = j;
                    while ( back > i && src[back - 1] == '\r' )
                        --back;
                    if ( back == i || src[back - 1] != '\\' )
                        break;
                    ++j;
                }
                blank( i, j );
                i = j;
            }
            else if ( src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*' )
            {
                const std::size_t end = src.find( "*/", i + 2 );
                const std::size_t j   = end == std::string::npos ? src.size() : end + 2;
                blank( i, j );
                i = j;
            }
            else if ( src[i] == '"' )
            {
                bool              raw   = false;
                const std::size_t start = LiteralPrefixStart( src, i, raw );

                std::size_t j = std::string::npos;
                if ( raw )
                {
                    // `R"delim(` ... `)delim"`. The delimiter is the text up to the first `(`; a raw
                    // string with no terminator is not valid C++ at all, and blanking to the end of the
                    // file is what a compiler effectively does with it.
                    const std::size_t paren = src.find( '(', i + 1 );
                    if ( paren != std::string::npos && paren - i - 1 <= 16 )
                    {
                        const std::string close = ")" + src.substr( i + 1, paren - i - 1 ) + "\"";
                        const std::size_t end   = src.find( close, paren );
                        j                       = end == std::string::npos ? src.size() : end + close.size();
                    }
                }
                else
                {
                    j = closingQuote( i, '"' );
                }

                if ( j == std::string::npos )
                {
                    // Not a literal after all — a lone quote. Copied through so that the damage of a
                    // malformed file is one character, never the rest of the file.
                    out[i] = src[i];
                    ++i;
                }
                else
                {
                    if ( keepLiterals )
                        keep( start, j );
                    else
                        blank( start, j );
                    i = j;
                }
            }
            else if ( src[i] == '\'' && !IsDigitSeparator( src, i ) )
            {
                bool              prefixIsRaw = false;
                const std::size_t start       = LiteralPrefixStart( src, i, prefixIsRaw );
                const std::size_t j           = closingQuote( i, '\'' );
                if ( j == std::string::npos )
                {
                    out[i] = src[i];
                    ++i;
                }
                else
                {
                    if ( keepLiterals )
                        keep( start, j );
                    else
                        blank( start, j );
                    i = j;
                }
            }
            else
            {
                out[i] = src[i];
                ++i;
            }
        }
        return out;
    }

    // The spelling every existing caller uses: comments and literals both gone.
    inline std::string StripCommentsAndLiterals( const std::string& src )
    {
        return StripCommentsMaybeLiterals( src, /*keepLiterals=*/false );
    }

    // Comments gone, literals INTACT — for a census whose subject is a literal. See the note above.
    inline std::string StripComments( const std::string& src )
    {
        return StripCommentsMaybeLiterals( src, /*keepLiterals=*/true );
    }

    inline bool WordAt( const std::string& s, std::size_t at, const std::string& word )
    {
        if ( at + word.size() > s.size() || s.compare( at, word.size(), word ) != 0 )
            return false;
        const bool leftOk  = at == 0 || !IsIdentChar( s[at - 1] );
        const bool rightOk = at + word.size() >= s.size() || !IsIdentChar( s[at + word.size()] );
        return leftOk && rightOk;
    }

    inline std::vector<std::size_t> WordPositions( const std::string& s, const std::string& word )
    {
        std::vector<std::size_t> out;
        if ( word.empty() )
            return out;
        for ( std::size_t at = s.find( word ); at != std::string::npos; at = s.find( word, at + 1 ) )
            if ( WordAt( s, at, word ) )
                out.push_back( at );
        return out;
    }

    inline std::size_t SkipSpace( const std::string& s, std::size_t i )
    {
        while ( i < s.size() && std::isspace( static_cast<unsigned char>( s[i] ) ) != 0 )
            ++i;
        return i;
    }

    inline std::string IdentAt( const std::string& s, std::size_t i )
    {
        const std::size_t start = i;
        if ( i < s.size() && ( std::isalpha( static_cast<unsigned char>( s[i] ) ) != 0 || s[i] == '_' ) )
        {
            while ( i < s.size() && IsIdentChar( s[i] ) )
                ++i;
            return s.substr( start, i - start );
        }
        return {};
    }

    // The statement `at` sits in, bounded by the nearest `;`, `{` or `}` behind it. Good enough to keep a
    // binding and its initializer together without parsing C++.
    inline std::string StatementBefore( const std::string& s, std::size_t at )
    {
        std::size_t start = 0;
        for ( char stop : { ';', '{', '}' } )
        {
            const std::size_t p = s.rfind( stop, at == 0 ? 0 : at - 1 );
            if ( p != std::string::npos && p + 1 > start )
                start = p + 1;
        }
        return s.substr( start, at - start );
    }

    // The identifier being declared by `IDENT ... = <initializer>`, given the text left of the `=`.
    inline std::string DeclaredNameBeforeAssignment( const std::string& stmt )
    {
        std::size_t eq = std::string::npos;
        for ( std::size_t i = stmt.size(); i-- > 0; )
        {
            if ( stmt[i] != '=' )
                continue;
            const char prev = i > 0 ? stmt[i - 1] : ' ';
            const char next = i + 1 < stmt.size() ? stmt[i + 1] : ' ';
            if ( prev == '=' || prev == '!' || prev == '<' || prev == '>' || next == '=' )
                continue; // a comparison, not a binding
            eq = i;
            break;
        }
        if ( eq == std::string::npos )
            return {};

        std::string last;
        for ( std::size_t i = 0; i < eq; )
        {
            const std::string id = IdentAt( stmt, i );
            if ( id.empty() )
            {
                ++i;
                continue;
            }
            i += id.size();
            if ( id != "const" && id != "auto" && id != "static" && id != "inline" && id != "return" )
                last = id;
        }
        return last;
    }

    // `.Field` / `->Field`, optionally through `.Data`, at `i` - and not a plain assignment to it.
    inline bool MemberReadAt( const std::string& s, std::size_t i, const std::string& field )
    {
        i = SkipSpace( s, i );
        if ( i < s.size() && s[i] == '.' )
            ++i;
        else if ( i + 1 < s.size() && s[i] == '-' && s[i + 1] == '>' )
            i += 2;
        else
            return false;

        i = SkipSpace( s, i );
        if ( WordAt( s, i, "Data" ) )
        {
            i = SkipSpace( s, i + 4 );
            if ( i < s.size() && s[i] == '.' )
                ++i;
            else if ( i + 1 < s.size() && s[i] == '-' && s[i + 1] == '>' )
                i += 2;
            else
                return false;
            i = SkipSpace( s, i );
        }

        if ( !WordAt( s, i, field ) )
            return false;

        const std::size_t after = SkipSpace( s, i + field.size() );
        if ( after < s.size() && s[after] == '=' && ( after + 1 >= s.size() || s[after + 1] != '=' ) )
            return false; // the field is being written here, not read

        return true;
    }

    inline bool ReceiverReadsField( const std::string& s, const std::string& receiver, const std::string& field )
    {
        for ( std::size_t at : WordPositions( s, receiver ) )
            if ( MemberReadAt( s, at + receiver.size(), field ) )
                return true;
        return false;
    }

    // `call( ..., receiver.field, ... )` for one of `receivers` - the shape of an assertion that a particular
    // EXPRESSION is present, without pinning the local variable's spelling.
    inline bool CallOnFieldRead( const std::string& s, const std::string& call,
                                 const std::vector<std::string>& receivers, const std::string& field )
    {
        for ( std::size_t at : WordPositions( s, call ) )
        {
            std::size_t i = SkipSpace( s, at + call.size() );
            if ( i >= s.size() || s[i] != '(' )
                continue;

            // Every top-level argument of the call, so a resource or context passed ahead of the field
            // (`ResolveSpriteImage( res, canvas.Sprite )`) does not hide the read.
            int depth = 0;
            for ( std::size_t a = i; a < s.size(); ++a )
            {
                const char c = s[a];
                if ( c == '(' || c == '[' || c == '{' )
                    ++depth;
                else if ( c == ')' || c == ']' || c == '}' )
                {
                    if ( --depth == 0 )
                        break;
                    continue;
                }
                if ( depth != 1 || ( c != '(' && c != ',' ) )
                    continue;

                const std::size_t argAt = SkipSpace( s, a + 1 );
                const std::string recv  = IdentAt( s, argAt );
                bool              known = false;
                for ( const std::string& r : receivers )
                    known = known || ( !recv.empty() && r == recv );
                if ( !known || !MemberReadAt( s, argAt + recv.size(), field ) )
                    continue;

                const std::size_t after = SkipSpace( s, s.find( field, argAt ) + field.size() );
                if ( after < s.size() && ( s[after] == ')' || s[after] == ',' ) )
                    return true;
            }
        }
        return false;
    }

    // An anchor may be followed by a template close and a call before the member access, which is how
    // `reg.get<ECS::UILayoutComponent>( e ).Data.HitTest` and `scene->GetSettings().ShowGrid` read.
    inline bool AnchorReadsField( const std::string& s, const std::string& anchor, const std::string& field )
    {
        for ( std::size_t at : WordPositions( s, anchor ) )
        {
            std::size_t i = SkipSpace( s, at + anchor.size() );

            // A POINTER TO MEMBER, `&ECS::UIPointerEventsData::OnDownMessage`, names the field on the type
            // explicitly, so it is a STRICTER anchor than `x.Field` and not a looser one: the type and the
            // field have to stand next to each other for it to match at all. It is here because
            // UICanvasRenderer2D routes press and release through one function that takes the member as a
            // parameter, and without this shape two live settings read as dead.
            if ( i + 1 < s.size() && s[i] == ':' && s[i + 1] == ':' &&
                 IdentAt( s, SkipSpace( s, i + 2 ) ) == field )
                return true;

            if ( i < s.size() && s[i] == '>' )
                i = SkipSpace( s, i + 1 );
            if ( i < s.size() && s[i] == '(' )
            {
                int depth = 0;
                while ( i < s.size() && s[i] != ';' )
                {
                    if ( s[i] == '(' )
                        ++depth;
                    else if ( s[i] == ')' && --depth == 0 )
                    {
                        ++i;
                        break;
                    }
                    ++i;
                }
                i = SkipSpace( s, i );
            }
            if ( MemberReadAt( s, i, field ) )
                return true;
        }
        return false;
    }

    // Identifiers this file binds to one of `anchors`. Deliberately narrow: an over-generous rule here
    // (any identifier assigned from any expression mentioning a receiver) was measured to credit 150
    // names in UICanvasRenderer2D.cpp - including the button's `b` - which would have re-opened exactly
    // the hole this file exists to close.
    inline std::vector<std::string> DeriveReceivers( const std::string&              s,
                                                     const std::vector<std::string>& anchors )
    {
        std::vector<std::string> out;
        const auto               add = [&out]( const std::string& name )
        {
            if ( name.empty() )
                return;
            for ( const std::string& have : out )
                if ( have == name )
                    return;
            out.push_back( name );
        };

        for ( const std::string& anchor : anchors )
        {
            for ( std::size_t at : WordPositions( s, anchor ) )
            {
                // A declaration or parameter: `const ECS::UICanvasData& d`, `VolumetricCloudData m_Data`.
                std::size_t i    = SkipSpace( s, at + anchor.size() );
                const char  head = i < s.size() ? s[i] : '\0';
                if ( head != '>' && head != ':' && head != '(' && head != '.' && head != ',' && head != ';' &&
                     head != ')' )
                {
                    while ( i < s.size() && ( s[i] == '&' || s[i] == '*' || WordAt( s, i, "const" ) ||
                                              std::isspace( static_cast<unsigned char>( s[i] ) ) != 0 ) )
                        i += WordAt( s, i, "const" ) ? 5 : 1;
                    add( IdentAt( s, i ) );
                }

                // A local bound from an expression that names the anchor.
                add( DeclaredNameBeforeAssignment( StatementBefore( s, at ) ) );
            }
        }

        // Aliases: `IDENT = ... receiver.Data`, `IDENT = ... receiver( ... )`, and the parameters of the
        // lambda an entt view's `each` is handed - which is how nearly every ECS system in this engine
        // gets hold of a component.
        for ( int round = 0; round < 4; ++round )
        {
            const std::size_t before = out.size();
            for ( std::size_t r = 0; r < out.size(); ++r )
            {
                const std::string receiver = out[r];
                for ( std::size_t at : WordPositions( s, receiver ) )
                {
                    std::size_t i = SkipSpace( s, at + receiver.size() );

                    bool aliasSource = i < s.size() && s[i] == '(';
                    if ( !aliasSource )
                    {
                        std::size_t j = i;
                        if ( j < s.size() && s[j] == '.' )
                            ++j;
                        else if ( j + 1 < s.size() && s[j] == '-' && s[j + 1] == '>' )
                            j += 2;
                        else
                            j = std::string::npos;
                        if ( j != std::string::npos && WordAt( s, SkipSpace( s, j ), "Data" ) )
                            aliasSource = true;
                    }
                    if ( aliasSource )
                        add( DeclaredNameBeforeAssignment( StatementBefore( s, at ) ) );

                    // `view.each( [&]( auto entity, const auto& light, ... ) {`
                    std::size_t k = i;
                    if ( k < s.size() && s[k] == '.' )
                        ++k;
                    else if ( k + 1 < s.size() && s[k] == '-' && s[k + 1] == '>' )
                        k += 2;
                    else
                        continue;
                    k = SkipSpace( s, k );
                    if ( !WordAt( s, k, "each" ) )
                        continue;
                    k = SkipSpace( s, k + 4 );
                    if ( k >= s.size() || s[k] != '(' )
                        continue;

                    const std::size_t bracket = s.find( '[', k );
                    if ( bracket == std::string::npos || bracket > k + 400 )
                        continue;
                    const std::size_t close = s.find( ']', bracket );
                    if ( close == std::string::npos )
                        continue;
                    const std::size_t open = s.find( '(', close );
                    const std::size_t end  = open == std::string::npos ? std::string::npos : s.find( ')', open );
                    if ( open == std::string::npos || end == std::string::npos )
                        continue;

                    std::string chunk;
                    for ( std::size_t p = open + 1; p <= end; ++p )
                    {
                        if ( p == end || s[p] == ',' )
                        {
                            std::string last;
                            for ( std::size_t q = 0; q < chunk.size(); )
                            {
                                const std::string id = IdentAt( chunk, q );
                                if ( id.empty() )
                                {
                                    ++q;
                                    continue;
                                }
                                q += id.size();
                                last = id;
                            }
                            add( last );
                            chunk.clear();
                        }
                        else
                        {
                            chunk += s[p];
                        }
                    }
                }
            }
            if ( out.size() == before )
                break;
        }
        return out;
    }
} // namespace Desert::Tests::ConsumerText
