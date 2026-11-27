// BuildPool: the three keys of an action (BuildPool/Invocation.hpp) - where its cost is remembered (CostKey),
// which compiler it is (CompilerId), and which other session is compiling the same thing (InflightKey over a
// PreprocessedDigest). Equal keys are a hint for speed; correctness always stays with the cache.
#include "BuildPool/Invocation.hpp"

#include <algorithm>
#include <system_error>

namespace buildpool
{
    namespace
    {
#if defined( _WIN32 )
        constexpr bool kPathsFoldCase = true;
#else
        constexpr bool kPathsFoldCase = false;
#endif

        char Fold( char c )
        {
            if ( kPathsFoldCase && c >= 'A' && c <= 'Z' )
                return static_cast<char>( c - 'A' + 'a' );
            return c;
        }

        // The length of `root` written at text[at] (either slash, a doubled backslash, case on Windows); 0 = none.
        size_t MatchRoot( std::string_view text, size_t at, std::string_view root )
        {
            size_t t = at;
            for ( const char r : root )
            {
                if ( t >= text.size() )
                    return 0;
                const char c = text[t];
                if ( r == '/' || r == '\\' )
                {
                    if ( c == '/' )
                        ++t;
                    else if ( c == '\\' )
                    {
                        ++t;
                        if ( t < text.size() && text[t] == '\\' )
                            ++t;
                    }
                    else
                        return 0;
                    continue;
                }
                if ( Fold( c ) != Fold( r ) )
                    return 0;
                ++t;
            }
            return t - at;
        }

        std::string ReplaceRoot( std::string_view text, std::string_view root, std::string_view token )
        {
            if ( root.empty() )
                return std::string( text );
            std::string out;
            out.reserve( text.size() );
            const char first        = Fold( root[0] );
            const bool firstIsSlash = root[0] == '/' || root[0] == '\\';
            size_t     i            = 0;
            while ( i < text.size() )
            {
                const char c         = text[i];
                const bool candidate = firstIsSlash ? ( c == '/' || c == '\\' ) : Fold( c ) == first;
                if ( candidate )
                {
                    if ( const size_t length = MatchRoot( text, i, root ); length > 0 )
                    {
                        out.append( token );
                        i += length;
                        continue;
                    }
                }
                out.push_back( c );
                ++i;
            }
            return out;
        }

        std::string_view ToolText( ToolKind tool )
        {
            switch ( tool )
            {
                case ToolKind::Msvc:
                    return "cl";
                case ToolKind::ClangCl:
                    return "clang-cl";
                case ToolKind::GccLike:
                    return "cc";
                case ToolKind::MsvcLink:
                    return "link";
                case ToolKind::Archiver:
                    return "ar";
                case ToolKind::Unknown:
                    break;
            }
            return "unknown";
        }

        std::string_view ActionText( ActionKind action )
        {
            switch ( action )
            {
                case ActionKind::Preprocess:
                    return "preprocess";
                case ActionKind::Compile:
                    return "compile";
                case ActionKind::PchCreate:
                    return "pch";
                case ActionKind::Link:
                    return "link";
                case ActionKind::Other:
                    break;
            }
            return "other";
        }

        // A field of a hashed record: its length first, so ("ab","c") and ("a","bc") never hash alike.
        void HashField( Sha256& hash, std::string_view field )
        {
            hash.Update( std::to_string( field.size() ) );
            hash.Update( ":" );
            hash.Update( field );
        }

        bool IsLineMarker( std::string_view line )
        {
            size_t i = 0;
            while ( i < line.size() && ( line[i] == ' ' || line[i] == '\t' ) )
                ++i;
            const std::string_view rest = line.substr( i );
            if ( rest.starts_with( "#line " ) )
                return true;
            return rest.size() >= 3 && rest[0] == '#' && rest[1] == ' ' && rest[2] >= '0' && rest[2] <= '9';
        }
    } // namespace

    std::string NormalizeRoots( std::string_view text, const TreeRoots& roots )
    {
        // The build root first: it usually lies inside the source root.
        const std::string withoutBuild = ReplaceRoot( text, roots.build, "@BUILD@" );
        return ReplaceRoot( withoutBuild, roots.source, "@SOURCE@" );
    }

    std::string CostKey( const Invocation& invocation, const TreeRoots& roots )
    {
        std::string output = NormalizeRoots( invocation.primaryOutput, roots );
        std::replace( output.begin(), output.end(), '\\', '/' );
        constexpr std::string_view kBuildPrefix = "@BUILD@/";
        if ( output.starts_with( kBuildPrefix ) )
            output.erase( 0, kBuildPrefix.size() );

        const size_t           slash         = roots.build.find_last_of( '/' );
        const std::string_view configuration = slash == std::string::npos
                                                    ? std::string_view( roots.build )
                                                    : std::string_view( roots.build ).substr( slash + 1 );

        std::string key;
        key.append( ToolText( invocation.tool ) ).append( "|" );
        key.append( ActionText( invocation.action ) ).append( "|" );
        key.append( configuration ).append( "|" );
        key.append( output );
        return key;
    }

    Result<std::string> CompilerId( const std::filesystem::path& compiler ) noexcept
    {
        struct Entry
        {
            std::string name;
            uintmax_t   size     = 0;
            long long   modified = 0;
        };

        const std::filesystem::path         directory = compiler.has_parent_path() ? compiler.parent_path() : ".";
        std::vector<Entry>                  entries;
        std::error_code                     error;
        std::filesystem::directory_iterator it( directory, error );
        for ( ; !error && it != std::filesystem::directory_iterator(); it.increment( error ) )
        {
            std::error_code fileError;
            if ( !it->is_regular_file( fileError ) || fileError )
                continue;
            Entry               entry;
            const std::u8string name = it->path().filename().u8string();
            entry.name.assign( name.begin(), name.end() );
            entry.size = it->file_size( fileError );
            if ( fileError )
                return Fail( "cannot read the size of " + entry.name +
                             " beside the compiler: " + fileError.message() );
            entry.modified = it->last_write_time( fileError ).time_since_epoch().count();
            if ( fileError )
                return Fail( "cannot read the time of " + entry.name +
                             " beside the compiler: " + fileError.message() );
            entries.push_back( std::move( entry ) );
        }
        if ( error )
        {
            const std::u8string text = directory.u8string();
            return Fail( "cannot list the compiler directory " + std::string( text.begin(), text.end() ) + ": " +
                         error.message() );
        }
        if ( entries.empty() )
            return Fail( "the compiler directory holds no file" );

        std::sort( entries.begin(), entries.end(),
                   []( const Entry& a, const Entry& b ) { return a.name < b.name; } );
        Sha256 hash;
        for ( const Entry& entry : entries )
        {
            HashField( hash, entry.name );
            HashField( hash, std::to_string( entry.size ) );
            HashField( hash, std::to_string( entry.modified ) );
        }
        return ToHex( hash.Finish() );
    }

    std::string ExpandCompilerId( std::string_view value, std::string_view compilerId )
    {
        constexpr std::string_view kToken = "{compiler_id}";
        std::string                out;
        out.reserve( value.size() + compilerId.size() );
        size_t from = 0;
        for ( size_t at = value.find( kToken ); at != std::string_view::npos; at = value.find( kToken, from ) )
        {
            out.append( value.substr( from, at - from ) );
            out.append( compilerId );
            from = at + kToken.size();
        }
        out.append( value.substr( from ) );
        return out;
    }

    PreprocessedDigest::PreprocessedDigest( TreeRoots roots ) : m_roots( std::move( roots ) )
    {
    }

    void PreprocessedDigest::Feed( std::span<const std::byte> chunk )
    {
        const std::string_view data( reinterpret_cast<const char*>( chunk.data() ), chunk.size() );
        const auto             hashLine = [this]( std::string_view line )
        {
            if ( IsLineMarker( line ) )
                m_hash.Update( NormalizeRoots( line, m_roots ) );
            else
                m_hash.Update( line );
        };

        size_t pos = 0;
        if ( !m_line.empty() )
        {
            const size_t newline = data.find( '\n' );
            if ( newline == std::string_view::npos )
            {
                m_line.append( data );
                return;
            }
            m_line.append( data.substr( 0, newline + 1 ) );
            hashLine( m_line );
            m_line.clear();
            pos = newline + 1;
        }

        // Consecutive ordinary lines go to the hash as one run: the digest of a stream does not depend on how it
        // is cut into updates, so only the line markers need to be looked at one by one.
        size_t run = pos;
        while ( pos < data.size() )
        {
            const size_t newline = data.find( '\n', pos );
            if ( newline == std::string_view::npos )
                break;
            const std::string_view line = data.substr( pos, newline + 1 - pos );
            if ( IsLineMarker( line ) )
            {
                if ( pos > run )
                    m_hash.Update( data.substr( run, pos - run ) );
                m_hash.Update( NormalizeRoots( line, m_roots ) );
                run = newline + 1;
            }
            pos = newline + 1;
        }
        if ( pos > run )
            m_hash.Update( data.substr( run, pos - run ) );
        m_line.assign( data.substr( pos ) );
    }

    std::string PreprocessedDigest::Finish()
    {
        if ( !m_line.empty() )
        {
            if ( IsLineMarker( m_line ) )
                m_hash.Update( NormalizeRoots( m_line, m_roots ) );
            else
                m_hash.Update( m_line );
            m_line.clear();
        }
        return ToHex( m_hash.Finish() );
    }

    std::string InflightKey( std::span<const std::string> argv, const TreeRoots& roots,
                             std::string_view compilerId, std::string_view preprocessedDigest )
    {
        Sha256 hash;
        HashField( hash, compilerId );
        // argv[0] is the compiler's path; the compiler's identity is compilerId, wherever it is installed.
        for ( size_t i = 1; i < argv.size(); ++i )
            HashField( hash, NormalizeRoots( argv[i], roots ) );
        HashField( hash, preprocessedDigest );
        return ToHex( hash.Finish() );
    }
} // namespace buildpool
