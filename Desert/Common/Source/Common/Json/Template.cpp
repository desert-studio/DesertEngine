#include "Template.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace Common::Text
{
    namespace
    {
        // An include chain deeper than this is a cycle in practice; reported instead of overflowing the stack.
        constexpr int kMaxIncludeDepth = 64;

        // Three-way order as -1/0/1: the comparison filters report it and both numeric kinds share it.
        template <typename T>
        int ThreeWay( const T& a, const T& b )
        {
            if ( a < b )
                return -1;
            if ( b < a )
                return 1;
            return 0;
        }

        struct Position
        {
            int Line   = 1;
            int Column = 1;
        };

        // ------------------------------------------------------------------ expressions

        enum class ExprKind
        {
            Literal,
            Path,
            Not,
            And,
            Or,
            Compare,
            Filter
        };

        enum class CompareOp
        {
            Equal,
            NotEqual,
            Less,
            Greater,
            LessEqual,
            GreaterEqual
        };

        enum class FilterKind
        {
            Upper,
            Lower,
            Join,
            Length,
            Default
        };

        struct PathSegment
        {
            bool        IsIndex = false;
            std::string Key;
            std::size_t Index = 0;
        };

        struct Expr
        {
            ExprKind                           Kind = ExprKind::Literal;
            Position                           Where;
            Json::Value                        Literal;
            std::vector<PathSegment>           Segments; // Path: Segments[0] is the root name
            std::string                        PathText; // Path: as written, for error messages
            CompareOp                          Op     = CompareOp::Equal;
            FilterKind                         Filter = FilterKind::Upper;
            std::vector<std::unique_ptr<Expr>> Operands; // Not: 1, And/Or/Compare: 2, Filter: input + argument
        };
        using ExprPtr = std::unique_ptr<Expr>;

        // ------------------------------------------------------------------ statements

        enum class BlockKind
        {
            Text,
            Output,
            For,
            If,
            Include
        };

        struct Block
        {
            BlockKind          Kind = BlockKind::Text;
            Position           Where;
            std::string        Text;    // Text: the literal; Include: the template name
            ExprPtr            Value;   // Output: the expression; For: the iterable
            std::string        KeyVar;  // For: `k` of `k, v`, empty for a single variable
            std::string        ItemVar; // For: `x` / `v`
            std::vector<Block> Body;    // For
            std::vector<std::pair<ExprPtr, std::vector<Block>>> Branches; // If: condition + body
            std::vector<Block>                                  Else;     // If
        };

        std::string Located( std::string_view name, Position where, const std::string& message )
        {
            return std::string( name ) + ":" + std::to_string( where.Line ) + ":" +
                   std::to_string( where.Column ) + ": " + message;
        }
    } // namespace

    namespace Detail
    {
        struct TemplateBody
        {
            std::string        Name;
            std::vector<Block> Blocks;
        };
    } // namespace Detail

    namespace
    {
        // ------------------------------------------------------------------ lexer

        enum class TagKind
        {
            Text,
            Output,
            Statement
        };

        struct Tag
        {
            TagKind     Kind = TagKind::Text;
            std::string Content; // text, or the inside of the tag without delimiters and dashes
            Position    Where;   // tag start; for inner content the column of the first content character
            Position    ContentWhere;
        };

        bool IsSpace( char c )
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
        }

        class Lexer
        {
        public:
            Lexer( std::string_view text, std::string_view name ) : m_Text( text ), m_Name( name )
            {
            }

            ResultStr<std::vector<Tag>> Run()
            {
                std::vector<Tag> tags;
                bool             stripNext = false;
                while ( m_Pos < m_Text.size() )
                {
                    const std::size_t open = FindOpen( m_Pos );
                    std::string       text( m_Text.substr( m_Pos, open - m_Pos ) );
                    const Position    textWhere = m_Where;
                    Advance( open - m_Pos );
                    if ( stripNext )
                    {
                        const auto first = std::find_if_not( text.begin(), text.end(), IsSpace );
                        text.erase( text.begin(), first );
                    }
                    stripNext = false;
                    if ( open == m_Text.size() )
                    {
                        PushText( tags, std::move( text ), textWhere );
                        break;
                    }

                    const char  kind     = m_Text[open + 1];
                    const bool  stripPrv = open + 2 < m_Text.size() && m_Text[open + 2] == '-';
                    const char* close    = "#}";
                    if ( kind == '{' )
                        close = "}}";
                    else if ( kind == '%' )
                        close = "%}";
                    if ( stripPrv )
                    {
                        const auto last = std::find_if_not( text.rbegin(), text.rend(), IsSpace );
                        text.erase( last.base(), text.end() );
                    }
                    PushText( tags, std::move( text ), textWhere );

                    const Position    tagWhere     = m_Where;
                    const std::size_t contentStart = open + ( stripPrv ? 3 : 2 );
                    const std::size_t end          = m_Text.find( close, contentStart );
                    if ( end == std::string_view::npos )
                        return MakeError<std::vector<Tag>>( Located(
                             m_Name, tagWhere, std::string( "unterminated tag, expected '" ) + close + "'" ) );
                    std::size_t contentEnd = end;
                    if ( contentEnd > contentStart && m_Text[contentEnd - 1] == '-' )
                    {
                        stripNext = true;
                        --contentEnd;
                    }
                    Advance( contentStart - open );
                    const Position contentWhere = m_Where;
                    Advance( end + 2 - contentStart );

                    if ( kind == '#' )
                        continue;
                    tags.push_back( { kind == '{' ? TagKind::Output : TagKind::Statement,
                                      std::string( m_Text.substr( contentStart, contentEnd - contentStart ) ),
                                      tagWhere, contentWhere } );
                }
                return MakeSuccess( std::move( tags ) );
            }

        private:
            [[nodiscard]] std::size_t FindOpen( std::size_t from ) const
            {
                for ( std::size_t i = from; i + 1 < m_Text.size(); ++i )
                    if ( m_Text[i] == '{' &&
                         ( m_Text[i + 1] == '{' || m_Text[i + 1] == '%' || m_Text[i + 1] == '#' ) )
                        return i;
                return m_Text.size();
            }

            void Advance( std::size_t count )
            {
                for ( std::size_t i = 0; i < count; ++i, ++m_Pos )
                {
                    if ( m_Text[m_Pos] == '\n' )
                    {
                        ++m_Where.Line;
                        m_Where.Column = 1;
                    }
                    else
                        ++m_Where.Column;
                }
            }

            static void PushText( std::vector<Tag>& tags, std::string text, Position where )
            {
                if ( !text.empty() )
                    tags.push_back( { TagKind::Text, std::move( text ), where, where } );
            }

            std::string_view m_Text;
            std::string_view m_Name;
            std::size_t      m_Pos = 0;
            Position         m_Where;
        };

        // ------------------------------------------------------------------ expression parser

        enum class TokenKind
        {
            End,
            Identifier,
            String,
            Integer,
            Real,
            Symbol
        };

        struct Token
        {
            TokenKind   Kind = TokenKind::End;
            std::string Text;
            Position    Where;
        };

        // The template grammar nests (parenthesised expressions, blocks inside for/if, includes), so the
        // parser and the renderer are recursive descent by design. Include depth is capped at
        // kMaxIncludeDepth; expression and block nesting is bounded by the authored template's own text.
        // NOLINTBEGIN(misc-no-recursion)
        class ExprParser
        {
        public:
            ExprParser( std::string_view source, Position where, std::string_view name )
                 : m_Source( source ), m_Where( where ), m_Name( name )
            {
            }

            // Splits the whole tag content into tokens; statement keywords are ordinary identifiers.
            BoolResultStr Tokenize()
            {
                std::size_t i    = 0;
                Position    at   = m_Where;
                auto        step = [&]( std::size_t n )
                {
                    for ( std::size_t k = 0; k < n; ++k, ++i )
                    {
                        if ( m_Source[i] == '\n' )
                        {
                            ++at.Line;
                            at.Column = 1;
                        }
                        else
                            ++at.Column;
                    }
                };
                while ( i < m_Source.size() )
                {
                    const char c = m_Source[i];
                    if ( IsSpace( c ) )
                    {
                        step( 1 );
                        continue;
                    }
                    const Position start = at;
                    if ( ( std::isalpha( static_cast<unsigned char>( c ) ) != 0 ) || c == '_' )
                    {
                        std::size_t j = i;
                        while ( j < m_Source.size() &&
                                ( ( std::isalnum( static_cast<unsigned char>( m_Source[j] ) ) != 0 ) ||
                                  m_Source[j] == '_' ) )
                            ++j;
                        m_Tokens.push_back(
                             { TokenKind::Identifier, std::string( m_Source.substr( i, j - i ) ), start } );
                        step( j - i );
                    }
                    else if ( ( std::isdigit( static_cast<unsigned char>( c ) ) != 0 ) ||
                              ( c == '-' && i + 1 < m_Source.size() &&
                                ( std::isdigit( static_cast<unsigned char>( m_Source[i + 1] ) ) != 0 ) ) )
                    {
                        std::size_t j    = i + 1;
                        bool        real = false;
                        while ( j < m_Source.size() &&
                                ( ( std::isdigit( static_cast<unsigned char>( m_Source[j] ) ) != 0 ) ||
                                  ( m_Source[j] == '.' && !real ) ) )
                        {
                            real = real || m_Source[j] == '.';
                            ++j;
                        }
                        m_Tokens.push_back( { real ? TokenKind::Real : TokenKind::Integer,
                                              std::string( m_Source.substr( i, j - i ) ), start } );
                        step( j - i );
                    }
                    else if ( c == '"' || c == '\'' )
                    {
                        std::string value;
                        std::size_t j = i + 1;
                        while ( j < m_Source.size() && m_Source[j] != c )
                        {
                            if ( m_Source[j] == '\\' && j + 1 < m_Source.size() )
                            {
                                const char e       = m_Source[++j];
                                char       decoded = e;
                                if ( e == 'n' )
                                    decoded = '\n';
                                else if ( e == 't' )
                                    decoded = '\t';
                                value += decoded;
                            }
                            else
                                value += m_Source[j];
                            ++j;
                        }
                        if ( j >= m_Source.size() )
                            return MakeError( Located( m_Name, start, "unterminated string literal" ) );
                        m_Tokens.push_back( { TokenKind::String, std::move( value ), start } );
                        step( j + 1 - i );
                    }
                    else
                    {
                        static constexpr std::string_view kTwo[] = { "==", "!=", "<=", ">=" };
                        const std::string_view            two    = m_Source.substr( i, 2 );
                        const bool                        isTwo =
                             std::find( std::begin( kTwo ), std::end( kTwo ), two ) != std::end( kTwo );
                        if ( !isTwo && std::string_view( ".[]()|,<>" ).find( c ) == std::string_view::npos )
                            return MakeError(
                                 Located( m_Name, start, std::string( "unexpected character '" ) + c + "'" ) );
                        m_Tokens.push_back(
                             { TokenKind::Symbol, std::string( isTwo ? two : m_Source.substr( i, 1 ) ), start } );
                        step( isTwo ? 2 : 1 );
                    }
                }
                m_Tokens.push_back( { TokenKind::End, "", at } );
                return MakeSuccess( true );
            }

            [[nodiscard]] const Token& Peek() const
            {
                return m_Tokens[m_Index];
            }
            const Token& Next()
            {
                return m_Tokens[m_Index == m_Tokens.size() - 1 ? m_Index : m_Index++];
            }
            [[nodiscard]] bool IsIdentifier( std::string_view text ) const
            {
                return Peek().Kind == TokenKind::Identifier && Peek().Text == text;
            }
            [[nodiscard]] bool IsSymbol( std::string_view text ) const
            {
                return Peek().Kind == TokenKind::Symbol && Peek().Text == text;
            }
            [[nodiscard]] bool AtEnd() const
            {
                return Peek().Kind == TokenKind::End;
            }

            [[nodiscard]] std::string ErrorAt( const Token& token, const std::string& message ) const
            {
                const std::string found = token.Kind == TokenKind::End ? "end of tag" : "'" + token.Text + "'";
                return Located( m_Name, token.Where, message + ", found " + found );
            }

            ResultStr<std::string> ExpectIdentifier( std::string_view what )
            {
                if ( Peek().Kind != TokenKind::Identifier )
                    return MakeError<std::string>( ErrorAt( Peek(), "expected " + std::string( what ) ) );
                return MakeSuccess( std::string( Next().Text ) );
            }

            BoolResultStr ExpectEnd() const
            {
                if ( !AtEnd() )
                    return MakeError( ErrorAt( Peek(), "unexpected token" ) );
                return MakeSuccess( true );
            }

            ResultStr<ExprPtr> ParseExpression()
            {
                return ParseOr();
            }

        private:
            static ExprPtr MakeBinary( ExprKind kind, Position where, ExprPtr lhs, ExprPtr rhs )
            {
                auto e   = std::make_unique<Expr>();
                e->Kind  = kind;
                e->Where = where;
                e->Operands.push_back( std::move( lhs ) );
                e->Operands.push_back( std::move( rhs ) );
                return e;
            }

            ResultStr<ExprPtr> ParseOr()
            {
                auto lhs = ParseAnd();
                if ( !lhs.IsSuccess() )
                    return lhs;
                ExprPtr result = lhs.ExtractValue();
                while ( IsIdentifier( "or" ) )
                {
                    const Position where = Next().Where;
                    auto           rhs   = ParseAnd();
                    if ( !rhs.IsSuccess() )
                        return rhs;
                    result = MakeBinary( ExprKind::Or, where, std::move( result ), rhs.ExtractValue() );
                }
                return MakeSuccess( std::move( result ) );
            }

            ResultStr<ExprPtr> ParseAnd()
            {
                auto lhs = ParseNot();
                if ( !lhs.IsSuccess() )
                    return lhs;
                ExprPtr result = lhs.ExtractValue();
                while ( IsIdentifier( "and" ) )
                {
                    const Position where = Next().Where;
                    auto           rhs   = ParseNot();
                    if ( !rhs.IsSuccess() )
                        return rhs;
                    result = MakeBinary( ExprKind::And, where, std::move( result ), rhs.ExtractValue() );
                }
                return MakeSuccess( std::move( result ) );
            }

            ResultStr<ExprPtr> ParseNot()
            {
                if ( !IsIdentifier( "not" ) )
                    return ParseCompare();
                const Position where   = Next().Where;
                auto           operand = ParseNot();
                if ( !operand.IsSuccess() )
                    return operand;
                auto e   = std::make_unique<Expr>();
                e->Kind  = ExprKind::Not;
                e->Where = where;
                e->Operands.push_back( operand.ExtractValue() );
                return MakeSuccess( std::move( e ) );
            }

            ResultStr<ExprPtr> ParseCompare()
            {
                auto lhs = ParseFiltered();
                if ( !lhs.IsSuccess() )
                    return lhs;
                static const std::pair<std::string_view, CompareOp> kOps[] = {
                     { "==", CompareOp::Equal },     { "!=", CompareOp::NotEqual },
                     { "<", CompareOp::Less },       { ">", CompareOp::Greater },
                     { "<=", CompareOp::LessEqual }, { ">=", CompareOp::GreaterEqual } };
                for ( const auto& [text, op] : kOps )
                {
                    if ( !IsSymbol( text ) )
                        continue;
                    const Position where = Next().Where;
                    auto           rhs   = ParseFiltered();
                    if ( !rhs.IsSuccess() )
                        return rhs;
                    auto e = MakeBinary( ExprKind::Compare, where, lhs.ExtractValue(), rhs.ExtractValue() );
                    e->Op  = op;
                    return MakeSuccess( std::move( e ) );
                }
                return lhs;
            }

            ResultStr<ExprPtr> ParseFiltered()
            {
                auto input = ParsePrimary();
                if ( !input.IsSuccess() )
                    return input;
                ExprPtr result = input.ExtractValue();
                while ( IsSymbol( "|" ) )
                {
                    Next();
                    const Token nameToken = Peek();
                    auto        name      = ExpectIdentifier( "a filter name" );
                    if ( !name.IsSuccess() )
                        return MakeError<ExprPtr>( name.GetError() );
                    static const std::pair<std::string_view, std::pair<FilterKind, bool>> kFilters[] = {
                         { "upper", { FilterKind::Upper, false } },
                         { "lower", { FilterKind::Lower, false } },
                         { "join", { FilterKind::Join, true } },
                         { "length", { FilterKind::Length, false } },
                         { "default", { FilterKind::Default, true } } };
                    const auto* found =
                         std::find_if( std::begin( kFilters ), std::end( kFilters ),
                                       [&]( const auto& f ) { return f.first == name.GetValue(); } );
                    if ( found == std::end( kFilters ) )
                        return MakeError<ExprPtr>(
                             Located( m_Name, nameToken.Where,
                                      "unknown filter '" + name.GetValue() +
                                           "' (known: upper, lower, join, length, default)" ) );
                    auto e    = std::make_unique<Expr>();
                    e->Kind   = ExprKind::Filter;
                    e->Where  = nameToken.Where;
                    e->Filter = found->second.first;
                    e->Operands.push_back( std::move( result ) );
                    if ( found->second.second )
                    {
                        if ( !IsSymbol( "(" ) )
                            return MakeError<ExprPtr>( ErrorAt(
                                 Peek(), "filter '" + name.GetValue() + "' takes one argument, expected '('" ) );
                        Next();
                        auto argument = ParseOr();
                        if ( !argument.IsSuccess() )
                            return argument;
                        e->Operands.push_back( argument.ExtractValue() );
                        if ( !IsSymbol( ")" ) )
                            return MakeError<ExprPtr>(
                                 ErrorAt( Peek(), "expected ')' after the filter argument" ) );
                        Next();
                    }
                    else if ( IsSymbol( "(" ) )
                        return MakeError<ExprPtr>(
                             ErrorAt( Peek(), "filter '" + name.GetValue() + "' takes no argument" ) );
                    result = std::move( e );
                }
                return MakeSuccess( std::move( result ) );
            }

            ResultStr<ExprPtr> ParsePrimary()
            {
                const Token token = Peek();
                auto        e     = std::make_unique<Expr>();
                e->Where          = token.Where;
                switch ( token.Kind )
                {
                    case TokenKind::String:
                        Next();
                        e->Literal = Json::Value( token.Text );
                        return MakeSuccess( std::move( e ) );
                    case TokenKind::Integer:
                    {
                        Next();
                        std::int64_t value = 0;
                        const auto [ptr, ec] =
                             std::from_chars( token.Text.data(), token.Text.data() + token.Text.size(), value );
                        if ( ec != std::errc() || ptr != token.Text.data() + token.Text.size() )
                            return MakeError<ExprPtr>( ErrorAt( token, "integer literal out of range" ) );
                        e->Literal = Json::Value( value );
                        return MakeSuccess( std::move( e ) );
                    }
                    case TokenKind::Real:
                        Next();
                        e->Literal = Json::Value( std::stod( token.Text ) );
                        return MakeSuccess( std::move( e ) );
                    case TokenKind::Identifier:
                        if ( token.Text == "true" || token.Text == "false" )
                        {
                            Next();
                            e->Literal = Json::Value( token.Text == "true" );
                            return MakeSuccess( std::move( e ) );
                        }
                        if ( token.Text == "and" || token.Text == "or" || token.Text == "not" ||
                             token.Text == "in" )
                            return MakeError<ExprPtr>( ErrorAt( token, "expected a value" ) );
                        return ParsePath();
                    case TokenKind::Symbol:
                        if ( token.Text == "(" )
                        {
                            Next();
                            auto inner = ParseOr();
                            if ( !inner.IsSuccess() )
                                return inner;
                            if ( !IsSymbol( ")" ) )
                                return MakeError<ExprPtr>( ErrorAt( Peek(), "expected ')'" ) );
                            Next();
                            return inner;
                        }
                        [[fallthrough]];
                    default:
                        return MakeError<ExprPtr>( ErrorAt( token, "expected a value" ) );
                }
            }

            ResultStr<ExprPtr> ParsePath()
            {
                auto e   = std::make_unique<Expr>();
                e->Kind  = ExprKind::Path;
                e->Where = Peek().Where;
                e->Segments.push_back( { false, Next().Text, 0 } );
                e->PathText = e->Segments.front().Key;
                for ( ;; )
                {
                    if ( IsSymbol( "." ) )
                    {
                        Next();
                        auto key = ExpectIdentifier( "a member name after '.'" );
                        if ( !key.IsSuccess() )
                            return MakeError<ExprPtr>( key.GetError() );
                        e->PathText += "." + key.GetValue();
                        e->Segments.push_back( { false, key.ExtractValue(), 0 } );
                    }
                    else if ( IsSymbol( "[" ) )
                    {
                        Next();
                        const Token inside = Next();
                        if ( inside.Kind == TokenKind::Integer && inside.Text.front() != '-' )
                        {
                            std::size_t index = 0;
                            std::from_chars( inside.Text.data(), inside.Text.data() + inside.Text.size(), index );
                            e->Segments.push_back( { true, "", index } );
                            e->PathText += "[" + inside.Text + "]";
                        }
                        else if ( inside.Kind == TokenKind::String )
                        {
                            e->Segments.push_back( { false, inside.Text, 0 } );
                            e->PathText += "[\"" + inside.Text + "\"]";
                        }
                        else
                            return MakeError<ExprPtr>(
                                 ErrorAt( inside, "expected a non-negative integer or a string inside '[ ]'" ) );
                        if ( !IsSymbol( "]" ) )
                            return MakeError<ExprPtr>( ErrorAt( Peek(), "expected ']'" ) );
                        Next();
                    }
                    else
                        return MakeSuccess( std::move( e ) );
                }
            }

            std::string_view   m_Source;
            Position           m_Where;
            std::string_view   m_Name;
            std::vector<Token> m_Tokens;
            std::size_t        m_Index = 0;
        };

        // ------------------------------------------------------------------ block parser

        class BlockParser
        {
        public:
            BlockParser( const std::vector<Tag>& tags, std::string_view name ) : m_Tags( tags ), m_Name( name )
            {
            }

            // Parses blocks until one of `terminators` (a statement keyword) or the end of the template.
            // On return m_Stop holds the terminating keyword ("" at the end) and m_StopParser its remaining
            // tokens.
            BoolResultStr ParseBlocks( std::vector<Block>&                     out,
                                       std::initializer_list<std::string_view> terminators )
            {
                while ( m_Index < m_Tags.size() )
                {
                    const Tag& tag = m_Tags[m_Index++];
                    if ( tag.Kind == TagKind::Text )
                    {
                        Block b;
                        b.Kind  = BlockKind::Text;
                        b.Where = tag.Where;
                        b.Text  = tag.Content;
                        out.push_back( std::move( b ) );
                        continue;
                    }
                    auto parser = std::make_unique<ExprParser>( tag.Content, tag.ContentWhere, m_Name );
                    if ( auto tokens = parser->Tokenize(); !tokens.IsSuccess() )
                        return tokens;
                    if ( tag.Kind == TagKind::Output )
                    {
                        if ( parser->AtEnd() )
                            return MakeError( Located( m_Name, tag.Where, "empty output tag" ) );
                        auto expr = parser->ParseExpression();
                        if ( !expr.IsSuccess() )
                            return MakeError( expr.GetError() );
                        if ( auto end = parser->ExpectEnd(); !end.IsSuccess() )
                            return end;
                        Block b;
                        b.Kind  = BlockKind::Output;
                        b.Where = tag.Where;
                        b.Value = expr.ExtractValue();
                        out.push_back( std::move( b ) );
                        continue;
                    }

                    auto keyword = parser->ExpectIdentifier( "a statement keyword" );
                    if ( !keyword.IsSuccess() )
                        return MakeError( keyword.GetError() );
                    const std::string& word = keyword.GetValue();
                    if ( std::find( terminators.begin(), terminators.end(), word ) != terminators.end() )
                    {
                        m_Stop       = word;
                        m_StopWhere  = tag.Where;
                        m_StopParser = std::move( parser );
                        return MakeSuccess( true );
                    }
                    BoolResultStr parsed;
                    if ( word == "for" )
                        parsed = ParseFor( *parser, tag.Where, out );
                    else if ( word == "if" )
                        parsed = ParseIf( *parser, tag.Where, out );
                    else if ( word == "include" )
                        parsed = ParseInclude( *parser, tag.Where, out );
                    else if ( word == "endfor" || word == "endif" || word == "elif" || word == "else" )
                        return MakeError(
                             Located( m_Name, tag.Where, "'" + word + "' without a matching opening tag" ) );
                    else
                        return MakeError( Located(
                             m_Name, tag.Where, "unknown statement '" + word + "' (known: for, if, include)" ) );
                    if ( !parsed.IsSuccess() )
                        return parsed;
                }
                m_Stop.clear();
                return MakeSuccess( true );
            }

            [[nodiscard]] const std::string& Stop() const
            {
                return m_Stop;
            }

        private:
            BoolResultStr ExpectClosing( std::string_view expected, std::string_view opener, Position openWhere )
            {
                if ( m_Stop.empty() )
                    return MakeError( Located( m_Name, openWhere,
                                               "'" + std::string( opener ) + "' is never closed, expected '" +
                                                    std::string( expected ) + "'" ) );
                return MakeSuccess( true );
            }

            BoolResultStr ParseFor( ExprParser& parser, Position where, std::vector<Block>& out )
            {
                Block b;
                b.Kind     = BlockKind::For;
                b.Where    = where;
                auto first = parser.ExpectIdentifier( "a loop variable" );
                if ( !first.IsSuccess() )
                    return MakeError( first.GetError() );
                b.ItemVar = first.ExtractValue();
                if ( parser.IsSymbol( "," ) )
                {
                    parser.Next();
                    auto second = parser.ExpectIdentifier( "a second loop variable after ','" );
                    if ( !second.IsSuccess() )
                        return MakeError( second.GetError() );
                    b.KeyVar  = b.ItemVar;
                    b.ItemVar = second.ExtractValue();
                }
                if ( b.ItemVar == "loop" || b.KeyVar == "loop" )
                    return MakeError(
                         Located( m_Name, where, "'loop' is reserved and cannot be a loop variable" ) );
                if ( !parser.IsIdentifier( "in" ) )
                    return MakeError( parser.ErrorAt( parser.Peek(), "expected 'in'" ) );
                parser.Next();
                auto iterable = parser.ParseExpression();
                if ( !iterable.IsSuccess() )
                    return MakeError( iterable.GetError() );
                if ( auto end = parser.ExpectEnd(); !end.IsSuccess() )
                    return end;
                b.Value = iterable.ExtractValue();

                if ( auto body = ParseBlocks( b.Body, { "endfor" } ); !body.IsSuccess() )
                    return body;
                if ( auto closed = ExpectClosing( "endfor", "for", where ); !closed.IsSuccess() )
                    return closed;
                if ( auto end = m_StopParser->ExpectEnd(); !end.IsSuccess() )
                    return end;
                out.push_back( std::move( b ) );
                return MakeSuccess( true );
            }

            BoolResultStr ParseIf( ExprParser& parser, Position where, std::vector<Block>& out )
            {
                Block b;
                b.Kind         = BlockKind::If;
                b.Where        = where;
                auto condition = parser.ParseExpression();
                if ( !condition.IsSuccess() )
                    return MakeError( condition.GetError() );
                if ( auto end = parser.ExpectEnd(); !end.IsSuccess() )
                    return end;
                ExprPtr current = condition.ExtractValue();
                for ( ;; )
                {
                    std::vector<Block> body;
                    if ( auto parsed = ParseBlocks( body, { "elif", "else", "endif" } ); !parsed.IsSuccess() )
                        return parsed;
                    if ( auto closed = ExpectClosing( "endif", "if", where ); !closed.IsSuccess() )
                        return closed;
                    b.Branches.emplace_back( std::move( current ), std::move( body ) );
                    if ( m_Stop == "elif" )
                    {
                        if ( m_StopParser->AtEnd() )
                            return MakeError( Located( m_Name, m_StopWhere, "'elif' needs a condition" ) );
                        auto next = m_StopParser->ParseExpression();
                        if ( !next.IsSuccess() )
                            return MakeError( next.GetError() );
                        if ( auto end = m_StopParser->ExpectEnd(); !end.IsSuccess() )
                            return end;
                        current = next.ExtractValue();
                        continue;
                    }
                    if ( auto end = m_StopParser->ExpectEnd(); !end.IsSuccess() )
                        return end;
                    if ( m_Stop == "else" )
                    {
                        // 'elif' and 'else' stop the else-body too, so they are named as misplaced here instead
                        // of surfacing as tags without an opening one.
                        if ( auto parsed = ParseBlocks( b.Else, { "elif", "else", "endif" } );
                             !parsed.IsSuccess() )
                            return parsed;
                        if ( m_Stop.empty() )
                            return MakeError( Located( m_Name, where, "'if' is never closed, expected 'endif'" ) );
                        if ( m_Stop != "endif" )
                            return MakeError( Located( m_Name, m_StopWhere, "'" + m_Stop + "' after 'else'" ) );
                        if ( auto end = m_StopParser->ExpectEnd(); !end.IsSuccess() )
                            return end;
                    }
                    break;
                }
                out.push_back( std::move( b ) );
                return MakeSuccess( true );
            }

            static BoolResultStr ParseInclude( ExprParser& parser, Position where, std::vector<Block>& out )
            {
                if ( parser.Peek().Kind != TokenKind::String )
                    return MakeError( parser.ErrorAt( parser.Peek(), "include expects a quoted template name" ) );
                Block b;
                b.Kind  = BlockKind::Include;
                b.Where = where;
                b.Text  = parser.Next().Text;
                if ( auto end = parser.ExpectEnd(); !end.IsSuccess() )
                    return end;
                out.push_back( std::move( b ) );
                return MakeSuccess( true );
            }

            const std::vector<Tag>&     m_Tags;
            std::string_view            m_Name;
            std::size_t                 m_Index = 0;
            std::string                 m_Stop;
            Position                    m_StopWhere;
            std::unique_ptr<ExprParser> m_StopParser;
        };

        // ------------------------------------------------------------------ renderer

        // A value during rendering: a reference into the caller's data, or one the template produced.
        struct Value
        {
            const Json::Value* Ref = nullptr;
            Json::Value        Owned;
            bool               Missing = false;

            [[nodiscard]] const Json::Value& Get() const
            {
                return Ref != nullptr ? *Ref : Owned;
            }
            [[nodiscard]] Json::Kind Kind() const
            {
                return Json::Root( Get() ).GetKind();
            }
            static Value Of( Json::Value v )
            {
                Value out;
                out.Owned = std::move( v );
                return out;
            }
        };

        struct Binding
        {
            std::string_view   Name;
            const Json::Value* Ref = nullptr;
            Json::Value        Owned;
        };

        struct LoopState
        {
            std::size_t Index  = 0;
            std::size_t Length = 0;
        };

        std::string KindName( Json::Kind kind )
        {
            switch ( kind )
            {
                case Json::Kind::Null:
                    return "null";
                case Json::Kind::Bool:
                    return "bool";
                case Json::Kind::Integer:
                    return "integer";
                case Json::Kind::Real:
                    return "number";
                case Json::Kind::String:
                    return "string";
                case Json::Kind::Array:
                    return "array";
                case Json::Kind::Object:
                    return "object";
            }
            return "unknown";
        }

        bool IsNumber( Json::Kind kind )
        {
            return kind == Json::Kind::Integer || kind == Json::Kind::Real;
        }

        // Callers check the kind first, so these reads cannot fail.
        std::int64_t IntegerOf( const Json::Value& value )
        {
            const auto read = Json::Root( value ).AsInteger();
            return read.GetValue();
        }
        double NumberOf( const Json::Value& value )
        {
            const auto read = Json::Root( value ).AsNumber();
            return read.GetValue();
        }

        std::string FormatReal( double value )
        {
            std::array<char, 64> buffer{};
            const auto [ptr, ec] = std::to_chars( buffer.data(), buffer.data() + buffer.size(), value );
            return ec == std::errc() ? std::string( buffer.data(), ptr ) : std::to_string( value );
        }

        class Renderer
        {
        public:
            Renderer( const Json::Node& data, const TemplateLoader& loader ) : m_Data( data ), m_Loader( loader )
            {
            }

            BoolResultStr Run( const Detail::TemplateBody& body, std::string& out )
            {
                const std::string_view previous = m_Name;
                m_Name                          = body.Name;
                auto result                     = RenderBlocks( body.Blocks, out );
                m_Name                          = previous;
                return result;
            }

        private:
            [[nodiscard]] std::string Error( Position where, const std::string& message ) const
            {
                return Located( m_Name, where, message );
            }

            BoolResultStr RenderBlocks( const std::vector<Block>& blocks, std::string& out )
            {
                for ( const Block& b : blocks )
                {
                    BoolResultStr result = MakeSuccess( true );
                    switch ( b.Kind )
                    {
                        case BlockKind::Text:
                            out += b.Text;
                            break;
                        case BlockKind::Output:
                            result = RenderOutput( b, out );
                            break;
                        case BlockKind::For:
                            result = RenderFor( b, out );
                            break;
                        case BlockKind::If:
                            result = RenderIf( b, out );
                            break;
                        case BlockKind::Include:
                            result = RenderInclude( b, out );
                            break;
                    }
                    if ( !result.IsSuccess() )
                        return result;
                }
                return MakeSuccess( true );
            }

            BoolResultStr RenderOutput( const Block& b, std::string& out )
            {
                auto value = Evaluate( *b.Value );
                if ( !value.IsSuccess() )
                    return MakeError( value.GetError() );
                return AppendText( *b.Value, value.GetValue(), out );
            }

            BoolResultStr AppendText( const Expr& e, const Value& v, std::string& out )
            {
                if ( v.Missing )
                    return MakeError( Error( e.Where, "unknown variable '" + e.PathText + "'" ) );
                const Json::Value& raw = v.Get();
                switch ( const Json::Kind kind = v.Kind() )
                {
                    case Json::Kind::String:
                        out += std::get<std::string>( raw.variant() );
                        break;
                    case Json::Kind::Integer:
                        out += std::to_string( IntegerOf( raw ) );
                        break;
                    case Json::Kind::Real:
                        out += FormatReal( NumberOf( raw ) );
                        break;
                    case Json::Kind::Bool:
                        out += std::get<bool>( raw.variant() ) ? "true" : "false";
                        break;
                    default:
                        return MakeError(
                             Error( e.Where, "cannot print " + KindName( kind ) +
                                                  ( e.PathText.empty() ? "" : " '" + e.PathText + "'" ) ) );
                }
                return MakeSuccess( true );
            }

            BoolResultStr RenderFor( const Block& b, std::string& out )
            {
                auto iterable = Evaluate( *b.Value );
                if ( !iterable.IsSuccess() )
                    return MakeError( iterable.GetError() );
                const Value& source = iterable.GetValue();
                if ( source.Missing )
                    return MakeError( Error( b.Value->Where, "unknown variable '" + b.Value->PathText + "'" ) );
                const Json::Value& raw       = source.Get();
                const std::size_t  scopeMark = m_Scope.size();
                BoolResultStr      result    = MakeSuccess( true );
                if ( b.KeyVar.empty() )
                {
                    const auto* array = std::get_if<Json::Value::Array>( &raw.variant() );
                    if ( array == nullptr )
                        return MakeError( Error(
                             b.Value->Where,
                             "'for " + b.ItemVar + " in' needs an array, found " + KindName( source.Kind() ) +
                                  ( source.Kind() == Json::Kind::Object ? " (iterate an object with 'for k, v in')"
                                                                        : "" ) ) );
                    m_Loops.push_back( { 0, array->size() } );
                    m_Scope.push_back( { b.ItemVar, nullptr, {} } );
                    for ( std::size_t i = 0; i < array->size() && result.IsSuccess(); ++i )
                    {
                        m_Loops.back().Index = i;
                        m_Scope.back().Ref   = &( *array )[i];
                        result               = RenderBlocks( b.Body, out );
                    }
                }
                else
                {
                    const auto* object = std::get_if<Json::Object>( &raw.variant() );
                    if ( object == nullptr )
                        return MakeError( Error( b.Value->Where, "'for " + b.KeyVar + ", " + b.ItemVar +
                                                                      " in' needs an object, found " +
                                                                      KindName( source.Kind() ) ) );
                    m_Loops.push_back( { 0, object->size() } );
                    m_Scope.push_back( { b.KeyVar, nullptr, {} } );
                    m_Scope.push_back( { b.ItemVar, nullptr, {} } );
                    std::size_t i = 0;
                    for ( auto it = object->begin(); it != object->end() && result.IsSuccess(); ++it, ++i )
                    {
                        m_Loops.back().Index              = i;
                        m_Scope[m_Scope.size() - 2].Owned = Json::Value( it->first );
                        m_Scope.back().Ref                = &it->second;
                        result                            = RenderBlocks( b.Body, out );
                    }
                }
                m_Scope.resize( scopeMark );
                m_Loops.pop_back();
                return result;
            }

            BoolResultStr RenderIf( const Block& b, std::string& out )
            {
                for ( const auto& [condition, body] : b.Branches )
                {
                    auto truth = Truth( *condition );
                    if ( !truth.IsSuccess() )
                        return MakeError( truth.GetError() );
                    if ( truth.GetValue() )
                        return RenderBlocks( body, out );
                }
                return RenderBlocks( b.Else, out );
            }

            BoolResultStr RenderInclude( const Block& b, std::string& out )
            {
                if ( !m_Loader )
                    return MakeError(
                         Error( b.Where, "include '" + b.Text + "' but no template loader was given" ) );
                if ( m_Depth >= kMaxIncludeDepth )
                    return MakeError( Error( b.Where, "include '" + b.Text + "' nests deeper than " +
                                                           std::to_string( kMaxIncludeDepth ) + " (a cycle?)" ) );
                auto loaded = m_Loader( b.Text );
                if ( !loaded.IsSuccess() )
                    return MakeError( Error( b.Where, "include '" + b.Text + "': " + loaded.GetError() ) );
                const Template* included = loaded.GetValue();
                if ( included == nullptr || !included->IsCompiled() )
                    return MakeError(
                         Error( b.Where, "include '" + b.Text + "': the loader returned no template" ) );
                ++m_Depth;
                auto result = Render( *included, out );
                --m_Depth;
                return result;
            }

            BoolResultStr Render( const Template& tpl, std::string& out );

            // A bare missing path is false (the existence test); every other operand must evaluate.
            ResultStr<bool> Truth( const Expr& e )
            {
                switch ( e.Kind )
                {
                    case ExprKind::Not:
                    {
                        auto inner = Truth( *e.Operands[0] );
                        return inner.IsSuccess() ? MakeSuccess( !inner.GetValue() ) : inner;
                    }
                    case ExprKind::And:
                    case ExprKind::Or:
                    {
                        auto lhs = Truth( *e.Operands[0] );
                        if ( !lhs.IsSuccess() )
                            return lhs;
                        if ( lhs.GetValue() == ( e.Kind == ExprKind::Or ) )
                            return lhs;
                        return Truth( *e.Operands[1] );
                    }
                    default:
                        break;
                }
                auto value = Evaluate( e );
                if ( !value.IsSuccess() )
                    return MakeError<bool>( value.GetError() );
                const Value& v = value.GetValue();
                if ( v.Missing )
                    return MakeSuccess( false );
                const Json::Value& raw = v.Get();
                switch ( v.Kind() )
                {
                    case Json::Kind::Null:
                        return MakeSuccess( false );
                    case Json::Kind::Bool:
                        return MakeSuccess( std::get<bool>( raw.variant() ) );
                    case Json::Kind::Integer:
                    case Json::Kind::Real:
                        return MakeSuccess( NumberOf( raw ) != 0.0 );
                    case Json::Kind::String:
                        return MakeSuccess( !std::get<std::string>( raw.variant() ).empty() );
                    case Json::Kind::Array:
                        return MakeSuccess( !std::get<Json::Value::Array>( raw.variant() ).empty() );
                    case Json::Kind::Object:
                        return MakeSuccess( std::get<Json::Object>( raw.variant() ).size() != 0 );
                }
                return MakeSuccess( false );
            }

            ResultStr<Value> Evaluate( const Expr& e )
            {
                switch ( e.Kind )
                {
                    case ExprKind::Literal:
                    {
                        Value v;
                        v.Ref = &e.Literal;
                        return MakeSuccess( std::move( v ) );
                    }
                    case ExprKind::Path:
                        return Lookup( e );
                    case ExprKind::Not:
                    case ExprKind::And:
                    case ExprKind::Or:
                    {
                        auto truth = Truth( e );
                        if ( !truth.IsSuccess() )
                            return MakeError<Value>( truth.GetError() );
                        return MakeSuccess( Value::Of( Json::Value( truth.GetValue() ) ) );
                    }
                    case ExprKind::Compare:
                        return Compare( e );
                    case ExprKind::Filter:
                        return ApplyFilter( e );
                }
                return MakeError<Value>( Error( e.Where, "unknown expression" ) );
            }

            ResultStr<Value> Lookup( const Expr& e )
            {
                const std::string& root  = e.Segments.front().Key;
                std::size_t        first = 1;
                Value              current;
                if ( root == "loop" && !m_Loops.empty() )
                {
                    if ( e.Segments.size() != 2 || e.Segments[1].IsIndex )
                        return MakeError<Value>( Error(
                             e.Where, "'" + e.PathText + "': loop has index, index0, first, last, length" ) );
                    const LoopState&   loop = m_Loops.back();
                    const std::string& what = e.Segments[1].Key;
                    if ( what == "index" )
                        current = Value::Of( Json::Value( static_cast<std::int64_t>( loop.Index + 1 ) ) );
                    else if ( what == "index0" )
                        current = Value::Of( Json::Value( static_cast<std::int64_t>( loop.Index ) ) );
                    else if ( what == "first" )
                        current = Value::Of( Json::Value( loop.Index == 0 ) );
                    else if ( what == "last" )
                        current = Value::Of( Json::Value( loop.Index + 1 == loop.Length ) );
                    else if ( what == "length" )
                        current = Value::Of( Json::Value( static_cast<std::int64_t>( loop.Length ) ) );
                    else
                        return MakeError<Value>( Error(
                             e.Where, "'" + e.PathText + "': loop has index, index0, first, last, length" ) );
                    return MakeSuccess( std::move( current ) );
                }

                const auto bound = std::find_if( m_Scope.rbegin(), m_Scope.rend(),
                                                 [&]( const Binding& b ) { return b.Name == root; } );
                if ( bound != m_Scope.rend() )
                    current.Ref = bound->Ref != nullptr ? bound->Ref : &bound->Owned;
                else
                {
                    first       = 0;
                    current.Ref = &m_Data.Raw();
                }
                for ( std::size_t i = first; i < e.Segments.size(); ++i )
                {
                    const PathSegment& s   = e.Segments[i];
                    const Json::Value& raw = *current.Ref;
                    if ( s.IsIndex )
                    {
                        const auto* array = std::get_if<Json::Value::Array>( &raw.variant() );
                        if ( array == nullptr )
                            return MakeError<Value>(
                                 Error( e.Where, "'" + e.PathText + "': [" + std::to_string( s.Index ) + "] on " +
                                                      KindName( Json::Root( raw ).GetKind() ) ) );
                        if ( s.Index >= array->size() )
                        {
                            current.Missing = true;
                            return MakeSuccess( std::move( current ) );
                        }
                        current.Ref = &( *array )[s.Index];
                        continue;
                    }
                    const auto* object = std::get_if<Json::Object>( &raw.variant() );
                    if ( object == nullptr )
                        return MakeError<Value>( Error( e.Where, "'" + e.PathText + "': member '" + s.Key +
                                                                      "' of " +
                                                                      KindName( Json::Root( raw ).GetKind() ) ) );
                    const auto it = std::find_if( object->begin(), object->end(),
                                                  [&]( const auto& member ) { return member.first == s.Key; } );
                    if ( it == object->end() )
                    {
                        current.Missing = true;
                        return MakeSuccess( std::move( current ) );
                    }
                    current.Ref = &it->second;
                }
                return MakeSuccess( std::move( current ) );
            }

            ResultStr<Value> Require( const Expr& e )
            {
                auto value = Evaluate( e );
                if ( value.IsSuccess() && value.GetValue().Missing )
                    return MakeError<Value>( Error( e.Where, "unknown variable '" + e.PathText + "'" ) );
                return value;
            }

            ResultStr<Value> Compare( const Expr& e )
            {
                auto lhsResult = Require( *e.Operands[0] );
                if ( !lhsResult.IsSuccess() )
                    return lhsResult;
                auto rhsResult = Require( *e.Operands[1] );
                if ( !rhsResult.IsSuccess() )
                    return rhsResult;
                const Json::Value& lhs   = lhsResult.GetValue().Get();
                const Json::Value& rhs   = rhsResult.GetValue().Get();
                const Json::Kind   lk    = Json::Root( lhs ).GetKind();
                const Json::Kind   rk    = Json::Root( rhs ).GetKind();
                int                order = 0;
                bool               equal = false;
                if ( IsNumber( lk ) && IsNumber( rk ) )
                {
                    if ( lk == Json::Kind::Integer && rk == Json::Kind::Integer )
                    {
                        const std::int64_t a = IntegerOf( lhs );
                        const std::int64_t b = IntegerOf( rhs );
                        order                = ThreeWay( a, b );
                    }
                    else
                    {
                        const double a = NumberOf( lhs );
                        const double b = NumberOf( rhs );
                        order          = ThreeWay( a, b );
                    }
                    equal = order == 0;
                }
                else if ( lk == Json::Kind::String && rk == Json::Kind::String )
                {
                    const int c =
                         std::get<std::string>( lhs.variant() ).compare( std::get<std::string>( rhs.variant() ) );
                    order = ThreeWay( c, 0 );
                    equal = c == 0;
                }
                else
                {
                    // Different kinds are unequal (1 != "1", as in Jinja); only == and != may ask that.
                    if ( e.Op != CompareOp::Equal && e.Op != CompareOp::NotEqual )
                        return MakeError<Value>(
                             Error( e.Where, "cannot order " + KindName( lk ) + " against " + KindName( rk ) ) );
                    equal = lk == rk && Json::Same( lhs, rhs );
                }
                bool result = false;
                switch ( e.Op )
                {
                    case CompareOp::Equal:
                        result = equal;
                        break;
                    case CompareOp::NotEqual:
                        result = !equal;
                        break;
                    case CompareOp::Less:
                        result = order < 0;
                        break;
                    case CompareOp::Greater:
                        result = order > 0;
                        break;
                    case CompareOp::LessEqual:
                        result = order <= 0;
                        break;
                    case CompareOp::GreaterEqual:
                        result = order >= 0;
                        break;
                }
                return MakeSuccess( Value::Of( Json::Value( result ) ) );
            }

            ResultStr<Value> ApplyFilter( const Expr& e )
            {
                const Expr& inputExpr = *e.Operands[0];
                if ( e.Filter == FilterKind::Default )
                {
                    auto input = Evaluate( inputExpr );
                    if ( !input.IsSuccess() || !input.GetValue().Missing )
                        return input;
                    return Require( *e.Operands[1] );
                }
                auto input = Require( inputExpr );
                if ( !input.IsSuccess() )
                    return input;
                const Json::Value& raw   = input.GetValue().Get();
                const Json::Kind   kind  = Json::Root( raw ).GetKind();
                auto               wrong = [&]( const char* needs )
                {
                    static constexpr std::string_view kNames[] = { "upper", "lower", "join", "length", "default" };
                    return MakeError<Value>(
                         Error( e.Where, "filter '" + std::string( kNames[static_cast<std::size_t>( e.Filter )] ) +
                                              "' needs " + needs + ", found " + KindName( kind ) ) );
                };
                switch ( e.Filter )
                {
                    case FilterKind::Upper:
                    case FilterKind::Lower:
                    {
                        if ( kind != Json::Kind::String )
                            return wrong( "a string" );
                        std::string text = std::get<std::string>( raw.variant() );
                        for ( char& c : text )
                            c = static_cast<char>( e.Filter == FilterKind::Upper
                                                        ? std::toupper( static_cast<unsigned char>( c ) )
                                                        : std::tolower( static_cast<unsigned char>( c ) ) );
                        return MakeSuccess( Value::Of( Json::Value( std::move( text ) ) ) );
                    }
                    case FilterKind::Length:
                    {
                        std::size_t length = 0;
                        if ( kind == Json::Kind::String )
                            length = std::get<std::string>( raw.variant() ).size();
                        else if ( kind == Json::Kind::Array )
                            length = std::get<Json::Value::Array>( raw.variant() ).size();
                        else if ( kind == Json::Kind::Object )
                            length = std::get<Json::Object>( raw.variant() ).size();
                        else
                            return wrong( "a string, array or object" );
                        return MakeSuccess( Value::Of( Json::Value( static_cast<std::int64_t>( length ) ) ) );
                    }
                    case FilterKind::Join:
                    {
                        if ( kind != Json::Kind::Array )
                            return wrong( "an array" );
                        auto separator = Require( *e.Operands[1] );
                        if ( !separator.IsSuccess() )
                            return separator;
                        if ( separator.GetValue().Kind() != Json::Kind::String )
                            return MakeError<Value>(
                                 Error( e.Operands[1]->Where, "join's separator must be a string" ) );
                        const std::string& sep = std::get<std::string>( separator.GetValue().Get().variant() );
                        std::string        text;
                        const auto&        array = std::get<Json::Value::Array>( raw.variant() );
                        for ( std::size_t i = 0; i < array.size(); ++i )
                        {
                            if ( i > 0 )
                                text += sep;
                            Value element;
                            element.Ref = &array[i];
                            if ( auto appended = AppendText( e, element, text ); !appended.IsSuccess() )
                                return MakeError<Value>( appended.GetError() + " (element " + std::to_string( i ) +
                                                         " of join)" );
                        }
                        return MakeSuccess( Value::Of( Json::Value( std::move( text ) ) ) );
                    }
                    case FilterKind::Default:
                        break;
                }
                return MakeError<Value>( Error( e.Where, "unknown filter" ) );
            }

            const Json::Node&      m_Data;
            const TemplateLoader&  m_Loader;
            std::string_view       m_Name;
            std::vector<Binding>   m_Scope;
            std::vector<LoopState> m_Loops;
            int                    m_Depth = 0;
        };
    } // namespace

    BoolResultStr Renderer::Render( const Template& tpl, std::string& out )
    {
        return Run( *tpl.Body(), out );
    }
    // NOLINTEND(misc-no-recursion)

    const std::string& Template::Name() const
    {
        static const std::string kEmpty;
        return m_Body != nullptr ? m_Body->Name : kEmpty;
    }

    ResultStr<Template> Compile( std::string_view text, std::string_view name )
    {
        auto tags = Lexer( text, name ).Run();
        if ( !tags.IsSuccess() )
            return MakeError<Template>( tags.GetError() );
        auto body  = std::make_shared<Detail::TemplateBody>();
        body->Name = std::string( name );
        BlockParser parser( tags.GetValue(), body->Name );
        if ( auto parsed = parser.ParseBlocks( body->Blocks, {} ); !parsed.IsSuccess() )
            return MakeError<Template>( parsed.GetError() );
        Template tpl;
        tpl.m_Body = std::move( body );
        return MakeSuccess( std::move( tpl ) );
    }

    ResultStr<std::string> Render( const Template& tpl, const Json::Node& data, const TemplateLoader& loader )
    {
        if ( !tpl.IsCompiled() )
            return MakeError<std::string>( "Render: the template was never compiled" );
        std::string out;
        Renderer    renderer( data, loader );
        if ( auto result = renderer.Run( *tpl.Body(), out ); !result.IsSuccess() )
            return MakeError<std::string>( result.GetError() );
        return MakeSuccess( std::move( out ) );
    }

    BoolResultStr TemplateSet::Add( std::string_view name, std::string_view text )
    {
        if ( m_Templates.find( name ) != m_Templates.end() )
            return MakeError( "TemplateSet: '" + std::string( name ) + "' is added twice" );
        auto compiled = Compile( text, name );
        if ( !compiled.IsSuccess() )
            return MakeError( compiled.GetError() );
        m_Templates.emplace( std::string( name ), compiled.ExtractValue() );
        return MakeSuccess( true );
    }

    const Template* TemplateSet::Find( std::string_view name ) const
    {
        const auto it = m_Templates.find( name );
        return it != m_Templates.end() ? &it->second : nullptr;
    }

    TemplateLoader TemplateSet::Loader() const
    {
        return [this]( std::string_view name ) -> ResultStr<const Template*>
        {
            const Template* found = Find( name );
            if ( found == nullptr )
                return MakeError<const Template*>( "no template named '" + std::string( name ) + "' in the set" );
            return MakeSuccess( found );
        };
    }
} // namespace Common::Text
