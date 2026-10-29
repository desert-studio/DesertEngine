#include "LogView.hpp"

#include <algorithm>
#include <cctype>

namespace Desert::Editor
{
    namespace
    {
        // "[abc]rest" -> field="abc", rest advanced past the "]". Returns false without touching either
        // when @p text does not open with a closing bracket group.
        bool TakeBracketed( std::string_view& text, std::string_view& field )
        {
            if ( text.empty() || text.front() != '[' )
                return false;
            const std::size_t close = text.find( ']' );
            if ( close == std::string_view::npos )
                return false;
            field = text.substr( 1, close - 1 );
            text.remove_prefix( close + 1 );
            return true;
        }
    } // namespace

    LogSeverity SeverityOfLevel( std::string_view level )
    {
        // spdlog's own level names, as its pattern's %l writes them. Anything else — trace, debug, info,
        // or a word from a future level — is Info: a log row is only ever promoted to a complaint by the
        // logger having called it one.
        if ( level == "warning" )
            return LogSeverity::Warning;
        if ( level == "error" || level == "critical" )
            return LogSeverity::Error;
        return LogSeverity::Info;
    }

    LogLineParts ParseLogLine( std::string_view line )
    {
        LogLineParts parts;
        parts.Message = line; // the honest fallback: an unrecognised line is shown whole, never dropped

        std::string_view rest = line;
        std::string_view time, level, logger;
        if ( !TakeBracketed( rest, time ) || !TakeBracketed( rest, level ) || !TakeBracketed( rest, logger ) )
            return parts;

        // The pattern ends "[Desert]: " — without the colon-space this is some other bracketed text that
        // merely looks like a header, and treating it as one would mislabel the row.
        if ( rest.size() < 2 || rest[0] != ':' || rest[1] != ' ' )
            return parts;
        rest.remove_prefix( 2 );

        parts.Time     = time;
        parts.Level    = level;
        parts.Severity = SeverityOfLevel( level );
        parts.Parsed   = true;

        // An OPTIONAL leading "[Tag]" the message itself opens with — "[Vulkan] GPU: ...". It is a
        // convention, not a guarantee: 59 of 278 lines in a real startup log carry one, and a line
        // without one gets no chip rather than an invented category.
        std::string_view category;
        std::string_view afterTag = rest;
        if ( TakeBracketed( afterTag, category ) && !category.empty() &&
             ( afterTag.empty() || afterTag.front() == ' ' ) )
        {
            // A tag is one word-ish token; a message that merely starts with a bracketed sentence is not
            // a category, and chipping it would put a paragraph in the chip.
            const bool tagLike = category.size() <= 24 && category.find( ' ' ) == std::string_view::npos &&
                                 category.find( ':' ) == std::string_view::npos;
            if ( tagLike )
            {
                parts.Category = category;
                if ( !afterTag.empty() )
                    afterTag.remove_prefix( 1 ); // the space between the tag and the message
                rest = afterTag;
            }
        }

        parts.Message = rest;
        return parts;
    }

    bool LogMatches( const std::string& line, const std::string& query )
    {
        if ( query.empty() )
            return true;
        if ( query.size() > line.size() )
            return false;

        auto lower = []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); };
        auto it    = std::search( line.begin(), line.end(), query.begin(), query.end(),
                                  [&]( char a, char b ) { return lower( a ) == lower( b ); } );
        return it != line.end();
    }

    void AppendCollapsed( std::vector<LogRepeatRun>& runs, std::string text, int level )
    {
        // The event's identity: a parsed line is (level, severity, category, message) - its time is not part of
        // it; an unparsed line has no parts and compares whole. The run's Text is its first line, which carries
        // the same identity as every line folded into it.
        const LogLineParts parts = ParseLogLine( text );
        if ( !runs.empty() && runs.back().Level == level )
        {
            const LogLineParts last = ParseLogLine( runs.back().Text );
            const bool same = last.Parsed == parts.Parsed &&
                              ( parts.Parsed ? last.Level == parts.Level && last.Category == parts.Category &&
                                                    last.Message == parts.Message
                                             : runs.back().Text == text );
            if ( same )
            {
                ++runs.back().Count;
                runs.back().LastTime = std::string( parts.Time );
                return;
            }
        }
        std::string time( parts.Time );
        runs.push_back( { std::move( text ), level, 1, time, time } );
    }

    std::vector<LogRepeatRun> CollapseRepeats( const std::vector<std::pair<std::string, int>>& lines )
    {
        std::vector<LogRepeatRun> runs;
        for ( const auto& [text, level] : lines )
            AppendCollapsed( runs, text, level );
        return runs;
    }
} // namespace Desert::Editor
