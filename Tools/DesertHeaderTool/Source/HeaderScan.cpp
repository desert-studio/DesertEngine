#include "HeaderScan.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <string_view>

namespace Desert::HeaderTool
{
    namespace
    {
        bool IsIdentStart( char c )
        {
            return std::isalpha( static_cast<unsigned char>( c ) ) != 0 || c == '_';
        }

        bool IsIdentChar( char c )
        {
            return std::isalnum( static_cast<unsigned char>( c ) ) != 0 || c == '_';
        }

        bool IsRawStringPrefix( const std::string& text, size_t quote )
        {
            if ( quote == 0 || text[quote - 1] != 'R' )
                return false;
            if ( quote == 1 )
                return true;
            const char before = text[quote - 2];
            return !IsIdentChar( before ) || before == '8' || before == 'u' || before == 'U' || before == 'L';
        }

        std::string BlankLiteralsAndDirectives( const std::string& source )
        {
            std::string out      = source;
            bool        lineHead = true;
            for ( size_t i = 0; i < out.size(); )
            {
                const char c = out[i];
                if ( c == '\n' )
                {
                    lineHead = true;
                    ++i;
                    continue;
                }
                if ( lineHead && c == '#' )
                {
                    while ( i < out.size() && out[i] != '\n' )
                    {
                        if ( out[i] == '\\' && i + 1 < out.size() && out[i + 1] == '\n' )
                        {
                            out[i] = ' ';
                            i += 2;
                            continue;
                        }
                        out[i] = ' ';
                        ++i;
                    }
                    continue;
                }
                if ( std::isspace( static_cast<unsigned char>( c ) ) == 0 )
                    lineHead = false;
                if ( c == '"' && IsRawStringPrefix( out, i ) )
                {
                    const size_t      open = out.find( '(', i );
                    const std::string delimiter =
                         open == std::string::npos ? std::string() : out.substr( i + 1, open - i - 1 );
                    const std::string closing = std::format( "){}\"", delimiter );
                    const size_t close = open == std::string::npos ? std::string::npos : out.find( closing, open );
                    const size_t stop  = close == std::string::npos ? out.size() : close + closing.size();
                    for ( size_t k = i; k < stop; ++k )
                    {
                        if ( out[k] != '\n' )
                            out[k] = ' ';
                    }
                    i = stop;
                    continue;
                }
                if ( c == '"' || c == '\'' )
                {
                    const bool digitSeparator = c == '\'' && i > 0 && IsIdentChar( out[i - 1] ) &&
                                                i + 1 < out.size() && IsIdentChar( out[i + 1] );
                    if ( digitSeparator )
                    {
                        ++i;
                        continue;
                    }
                    const char quote = c;
                    ++i;
                    while ( i < out.size() && out[i] != quote && out[i] != '\n' )
                    {
                        if ( out[i] == '\\' && i + 1 < out.size() )
                        {
                            out[i]     = ' ';
                            out[i + 1] = out[i + 1] == '\n' ? '\n' : ' ';
                            i += 2;
                            continue;
                        }
                        out[i] = ' ';
                        ++i;
                    }
                    ++i;
                    continue;
                }
                ++i;
            }
            return out;
        }

        void SkipSpace( std::string_view text, size_t& i )
        {
            while ( i < text.size() && std::isspace( static_cast<unsigned char>( text[i] ) ) != 0 )
                ++i;
        }

        std::string ReadIdent( std::string_view text, size_t& i )
        {
            const size_t start = i;
            while ( i < text.size() && IsIdentChar( text[i] ) )
                ++i;
            return std::string( text.substr( start, i - start ) );
        }

        std::string ReadQualifiedIdent( std::string_view text, size_t& i )
        {
            std::string name;
            while ( i < text.size() )
            {
                if ( text.substr( i, 2 ) == "::" )
                {
                    name += "::";
                    i += 2;
                    continue;
                }
                if ( !IsIdentChar( text[i] ) )
                    break;
                name += text[i++];
            }
            return name;
        }

        std::string LastComponent( std::string_view qualified )
        {
            const size_t templateStart = qualified.find( '<' );
            if ( templateStart != std::string_view::npos )
                qualified = qualified.substr( 0, templateStart );
            while ( !qualified.empty() && std::isspace( static_cast<unsigned char>( qualified.back() ) ) != 0 )
                qualified.remove_suffix( 1 );
            const size_t scope = qualified.rfind( "::" );
            return std::string( scope == std::string_view::npos ? qualified : qualified.substr( scope + 2 ) );
        }

        bool IsSingleColon( std::string_view text, size_t i )
        {
            return text[i] == ':' && ( i + 1 >= text.size() || text[i + 1] != ':' ) &&
                   ( i == 0 || text[i - 1] != ':' );
        }

        struct Handler
        {
            std::string Name;
            std::string EventClass;
            std::string Access;
            int         Line = 0;
        };

        struct ClassInfo
        {
            std::string                Name;
            std::string                QualifiedName;
            std::vector<std::string>   Bases;
            std::vector<Handler>       Handlers;
            std::optional<std::string> SubsystemOwner;
            bool                       JoinsEvents = false;
            bool                       Checked     = false;
            std::filesystem::path      Path;
            std::string                IncludePath;
            int                        Line = 0;
        };

        struct Frame
        {
            enum class Kind
            {
                Namespace,
                Class,
                Other
            };
            Kind        FrameKind = Kind::Other;
            std::string Name;
            int         ClassIndex = -1;
            std::string Access;
        };

        class LineIndex
        {
        public:
            explicit LineIndex( std::string_view text )
            {
                m_Starts.push_back( 0 );
                for ( size_t i = 0; i < text.size(); ++i )
                {
                    if ( text[i] == '\n' )
                        m_Starts.push_back( i + 1 );
                }
            }
            [[nodiscard]] int LineOf( size_t offset ) const
            {
                return static_cast<int>( std::upper_bound( m_Starts.begin(), m_Starts.end(), offset ) -
                                         m_Starts.begin() );
            }

        private:
            std::vector<size_t> m_Starts;
        };

        struct ClassHead
        {
            std::string              Name;
            std::vector<std::string> Bases;
            size_t                   Brace = 0;
        };

        std::optional<ClassHead> ReadClassHead( std::string_view text, size_t i )
        {
            const size_t end = text.find_first_of( "{;", i );
            if ( end == std::string_view::npos || text[end] != '{' )
                return std::nullopt;
            size_t colon = std::string_view::npos;
            for ( size_t k = i; k < end; ++k )
            {
                if ( IsSingleColon( text, k ) )
                {
                    colon = k;
                    break;
                }
            }
            const std::string_view namePart =
                 text.substr( i, ( colon == std::string_view::npos ? end : colon ) - i );
            std::string name;
            for ( size_t k = 0; k < namePart.size(); )
            {
                const char c = namePart[k];
                if ( std::isspace( static_cast<unsigned char>( c ) ) != 0 || c == ':' )
                {
                    ++k;
                    continue;
                }
                if ( namePart.substr( k, 2 ) == "[[" )
                {
                    const size_t close = namePart.find( "]]", k );
                    if ( close == std::string_view::npos )
                        return std::nullopt;
                    k = close + 2;
                    continue;
                }
                if ( !IsIdentStart( c ) )
                    return std::nullopt;
                std::string word = ReadIdent( namePart, k );
                if ( word != "final" )
                    name = std::move( word );
            }
            if ( name.empty() )
                return std::nullopt;
            ClassHead head{ name, {}, end };
            if ( colon == std::string_view::npos )
                return head;
            const std::string_view basePart = text.substr( colon + 1, end - colon - 1 );
            int                    angle    = 0;
            std::string            current;
            const auto             flush = [&]
            {
                size_t      k = 0;
                std::string last;
                while ( k < current.size() )
                {
                    SkipSpace( current, k );
                    if ( k >= current.size() )
                        break;
                    std::string word = ReadQualifiedIdent( current, k );
                    if ( word.empty() )
                        break;
                    if ( word != "public" && word != "private" && word != "protected" && word != "virtual" )
                    {
                        last = std::move( word );
                        break;
                    }
                }
                if ( !last.empty() )
                    head.Bases.push_back( LastComponent( last ) );
                current.clear();
            };
            for ( const char c : basePart )
            {
                if ( c == '<' )
                    ++angle;
                else if ( c == '>' )
                    --angle;
                if ( c == ',' && angle == 0 )
                {
                    flush();
                    continue;
                }
                if ( angle == 0 && c != '>' )
                    current += c;
            }
            flush();
            return head;
        }

        std::optional<Handler> ReadHandler( std::string_view text, size_t i )
        {
            SkipSpace( text, i );
            if ( text.substr( i, 2 ) != "On" )
                return std::nullopt;
            Handler handler;
            handler.Name = ReadIdent( text, i );
            SkipSpace( text, i );
            if ( i >= text.size() || text[i] != '(' )
                return std::nullopt;
            ++i;
            SkipSpace( text, i );
            const std::string type = ReadQualifiedIdent( text, i );
            if ( type.empty() || type == "const" )
                return std::nullopt;
            SkipSpace( text, i );
            if ( i >= text.size() || text[i] != '&' )
                return std::nullopt;
            ++i;
            SkipSpace( text, i );
            ReadIdent( text, i );
            SkipSpace( text, i );
            if ( i >= text.size() || text[i] != ')' )
                return std::nullopt;
            handler.EventClass = LastComponent( type );
            return handler;
        }

        std::string JoinScope( const std::vector<Frame>& stack, const std::string& name )
        {
            std::string qualified;
            for ( const Frame& frame : stack )
            {
                if ( frame.FrameKind == Frame::Kind::Other || frame.Name.empty() )
                    continue;
                std::format_to( std::back_inserter( qualified ), "{}::", frame.Name );
            }
            return qualified + name;
        }

        void ScanFile( const ScannedFile& file, std::vector<ClassInfo>& classes, std::set<std::string>& attached,
                       std::vector<RoutedEventName>& events )
        {
            const std::string    text = BlankLiteralsAndDirectives( StripComments( file.Text ) );
            const LineIndex      lines( text );
            std::vector<Frame>   stack;
            std::optional<Frame> pending;
            std::string          previous;

            for ( size_t i = 0; i < text.size(); )
            {
                const char c = text[i];
                if ( c == '{' )
                {
                    stack.push_back( pending.value_or( Frame{} ) );
                    pending.reset();
                    ++i;
                    continue;
                }
                if ( c == '}' )
                {
                    if ( !stack.empty() )
                        stack.pop_back();
                    ++i;
                    continue;
                }
                if ( !IsIdentStart( c ) || ( i > 0 && IsIdentChar( text[i - 1] ) ) )
                {
                    ++i;
                    continue;
                }
                const size_t      wordStart = i;
                const std::string word      = ReadIdent( text, i );
                Frame*            owner =
                     !stack.empty() && stack.back().FrameKind == Frame::Kind::Class ? &stack.back() : nullptr;

                if ( word == "namespace" )
                {
                    const size_t end = text.find_first_of( "{;=", i );
                    if ( end != std::string::npos && text[end] == '{' )
                    {
                        std::string name( text.substr( i, end - i ) );
                        std::erase_if( name, []( char ch )
                                       { return std::isspace( static_cast<unsigned char>( ch ) ) != 0; } );
                        if ( name.starts_with( "inline" ) )
                            name.clear();
                        pending = Frame{ Frame::Kind::Namespace, name, -1, {} };
                        i       = end;
                    }
                }
                else if ( ( word == "class" || word == "struct" ) && previous != "enum" )
                {
                    if ( const auto head = ReadClassHead( text, i ) )
                    {
                        ClassInfo info;
                        info.Name          = head->Name;
                        info.QualifiedName = JoinScope( stack, head->Name );
                        info.Bases         = head->Bases;
                        info.Checked       = file.Checked;
                        info.Path          = file.Path;
                        info.IncludePath   = file.IncludePath;
                        info.Line          = lines.LineOf( wordStart );
                        classes.push_back( std::move( info ) );
                        pending = Frame{ Frame::Kind::Class, head->Name, static_cast<int>( classes.size() - 1 ),
                                         word == "class" ? "private" : "public" };
                        i       = head->Brace;
                    }
                }
                else if ( owner != nullptr && ( word == "public" || word == "private" || word == "protected" ) )
                {
                    size_t k = i;
                    SkipSpace( text, k );
                    if ( k < text.size() && IsSingleColon( text, k ) )
                        owner->Access = word;
                }
                else if ( owner != nullptr && word == "bool" )
                {
                    if ( auto handler = ReadHandler( text, i ) )
                    {
                        handler->Access = owner->Access;
                        handler->Line   = lines.LineOf( wordStart );
                        classes[owner->ClassIndex].Handlers.push_back( std::move( *handler ) );
                    }
                }
                else if ( owner != nullptr && word == "JoinEvents" )
                {
                    size_t k = i;
                    SkipSpace( text, k );
                    if ( k < text.size() && text[k] == '(' )
                        classes[owner->ClassIndex].JoinsEvents = true;
                }
                else if ( owner != nullptr && word == "DESERT_SUBSYSTEM" )
                {
                    size_t k = i;
                    SkipSpace( text, k );
                    if ( k < text.size() && text[k] == '(' )
                    {
                        ++k;
                        SkipSpace( text, k );
                        classes[owner->ClassIndex].SubsystemOwner = ReadIdent( text, k );
                    }
                }
                else if ( word == "DESERT_ROUTED_EVENT" )
                {
                    size_t k = i;
                    SkipSpace( text, k );
                    if ( k < text.size() && text[k] == '(' )
                    {
                        ++k;
                        SkipSpace( text, k );
                        RoutedEventName name;
                        name.EventClass = ReadIdent( text, k );
                        SkipSpace( text, k );
                        if ( k < text.size() && text[k] == ',' )
                        {
                            ++k;
                            SkipSpace( text, k );
                            name.HandlerSuffix = ReadIdent( text, k );
                        }
                        if ( !name.EventClass.empty() && !name.HandlerSuffix.empty() )
                            events.push_back( std::move( name ) );
                    }
                }
                else if ( word == "Attach" || word == "Emplace" || word == "Adopt" )
                {
                    size_t k = i;
                    SkipSpace( text, k );
                    if ( k < text.size() && text[k] == '<' )
                    {
                        ++k;
                        SkipSpace( text, k );
                        const std::string type = ReadQualifiedIdent( text, k );
                        if ( !type.empty() )
                            attached.insert( LastComponent( type ) );
                    }
                }
                previous = word;
            }
        }

        bool JoinsTheTree( const std::vector<ClassInfo>& classes, const ClassInfo& start )
        {
            std::set<std::string>                                visited;
            std::vector<std::reference_wrapper<const ClassInfo>> pending{ std::cref( start ) };
            while ( !pending.empty() )
            {
                const ClassInfo& info = pending.back().get();
                pending.pop_back();
                if ( info.SubsystemOwner || info.JoinsEvents )
                    return true;
                for ( const std::string& base : info.Bases )
                {
                    if ( !visited.insert( base ).second )
                        continue;
                    for ( const ClassInfo& candidate : classes )
                    {
                        if ( candidate.Name == base )
                            pending.push_back( std::cref( candidate ) );
                    }
                }
            }
            return false;
        }

        std::string BubbleHandlerName( const RoutedEventName& event )
        {
            return std::format( "On{}", event.HandlerSuffix );
        }

        std::string PreviewHandlerName( const RoutedEventName& event )
        {
            return std::format( "OnPreview{}", event.HandlerSuffix );
        }

        const RoutedEventName* FindEvent( const std::vector<RoutedEventName>& events,
                                          const std::string&                  eventClass )
        {
            const auto found = std::find_if( events.begin(), events.end(), [&]( const RoutedEventName& e )
                                             { return e.EventClass == eventClass; } );
            return found == events.end() ? nullptr : &*found;
        }
    } // namespace

    std::string StripComments( const std::string& source )
    {
        std::string out;
        out.reserve( source.size() );
        for ( size_t i = 0; i < source.size(); )
        {
            const char c = source[i];
            if ( c == '"' || c == '\'' )
            {
                const char quote = c;
                out += c;
                ++i;
                while ( i < source.size() )
                {
                    out += source[i];
                    if ( source[i] == '\\' && i + 1 < source.size() )
                    {
                        out += source[i + 1];
                        i += 2;
                        continue;
                    }
                    if ( source[i] == quote )
                    {
                        ++i;
                        break;
                    }
                    ++i;
                }
                continue;
            }
            if ( c == '/' && i + 1 < source.size() && source[i + 1] == '/' )
            {
                while ( i < source.size() && source[i] != '\n' )
                    ++i;
                continue;
            }
            if ( c == '/' && i + 1 < source.size() && source[i + 1] == '*' )
            {
                i += 2;
                while ( i + 1 < source.size() && ( source[i] != '*' || source[i + 1] != '/' ) )
                {
                    if ( source[i] == '\n' )
                        out += '\n';
                    ++i;
                }
                i += 2;
                continue;
            }
            out += c;
            ++i;
        }
        return out;
    }

    std::string FormatDiagnostic( const Diagnostic& diagnostic )
    {
        return std::format( "{}:{}: error: {}", diagnostic.Path.string(), diagnostic.Line, diagnostic.Message );
    }

    HeaderModel ScanHeaders( const std::vector<ScannedFile>& files )
    {
        HeaderModel            model;
        std::vector<ClassInfo> classes;
        std::set<std::string>  attached;
        for ( const ScannedFile& file : files )
            ScanFile( file, classes, attached, model.Events );

        for ( const ClassInfo& info : classes )
        {
            if ( !info.Checked )
                continue;
            if ( info.SubsystemOwner )
                model.Subsystems.push_back(
                     { info.QualifiedName, *info.SubsystemOwner, info.Path, info.IncludePath } );

            bool handlesEvents = false;
            for ( const Handler& handler : info.Handlers )
            {
                const RoutedEventName* event = FindEvent( model.Events, handler.EventClass );
                if ( event == nullptr )
                    continue;
                const std::string bubble  = BubbleHandlerName( *event );
                const std::string preview = PreviewHandlerName( *event );
                if ( handler.Name != bubble && handler.Name != preview )
                {
                    model.Errors.push_back(
                         { info.Path, handler.Line,
                           std::format( "'{}::{}' takes {}& but the handlers of {} are named '{}' and '{}'",
                                        info.QualifiedName, handler.Name, event->EventClass, event->EventClass,
                                        bubble, preview ) } );
                    continue;
                }
                if ( handler.Access != "public" )
                {
                    model.Errors.push_back(
                         { info.Path, handler.Line,
                           std::format( "'{}::{}' is {}: the event tree calls public handlers only",
                                        info.QualifiedName, handler.Name, handler.Access ) } );
                    continue;
                }
                handlesEvents = true;
            }

            if ( handlesEvents && !JoinsTheTree( classes, info ) && !attached.contains( info.Name ) )
            {
                model.Errors.push_back(
                     { info.Path, info.Line,
                       std::format(
                            "'{}' handles routed events but is not a node of an event tree: derive it from "
                            "a node (a layer, a panel), declare it DESERT_SUBSYSTEM( <Owner> ) or Attach it",
                            info.QualifiedName ) } );
            }
        }

        std::sort( model.Subsystems.begin(), model.Subsystems.end(),
                   []( const SubsystemDeclaration& a, const SubsystemDeclaration& b )
                   { return a.QualifiedName < b.QualifiedName; } );
        return model;
    }
} // namespace Desert::HeaderTool
