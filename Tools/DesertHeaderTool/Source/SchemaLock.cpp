#include "SchemaLock.hpp"

#include "AnnotationText.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace Desert::HeaderTool
{
    namespace
    {
        bool IsIdent( char c )
        {
            return std::isalnum( static_cast<unsigned char>( c ) ) || c == '_';
        }

        // "Shape.Radius" -> "Shape." ; "Radius" -> "".
        std::string ParentPrefix( const std::string& path )
        {
            const auto dot = path.rfind( '.' );
            return dot == std::string::npos ? std::string{} : path.substr( 0, dot + 1 );
        }

        std::string Leaf( const std::string& path )
        {
            const auto dot = path.rfind( '.' );
            return dot == std::string::npos ? path : path.substr( dot + 1 );
        }

        std::vector<std::string> Segments( const std::string& path )
        {
            std::vector<std::string> out;
            std::size_t              from = 0;
            while ( true )
            {
                const auto dot = path.find( '.', from );
                out.push_back( path.substr( from, dot - from ) );
                if ( dot == std::string::npos )
                    return out;
                from = dot + 1;
            }
        }

        std::string EnumText( const SchemaField& f )
        {
            std::string text = f.type + " {";
            for ( std::size_t i = 0; i < f.enumerators.size(); ++i )
                text += std::format( "{}{}={}", i == 0 ? "" : ", ", f.enumerators[i].first, f.enumerators[i].second );
            return text + "}";
        }

        // The current field the locked path is stored under now: each segment by its name, or by a sibling's
        // WasNamed. Null when a segment has neither; `missing` is then the locked path down to that segment.
        const SchemaField* Resolve( const std::string& lockedPath, const std::map<std::string, const SchemaField*>& byPath,
                                    const std::vector<SchemaField>& fields, std::string& missing, bool& renamed )
        {
            std::string        prefix; // the current spelling of the parent, "" or "Shape." or "Points[]."
            std::string        lockedPrefix;
            const SchemaField* at = nullptr;
            for ( const std::string& segment : Segments( lockedPath ) )
            {
                const bool        element = segment.ends_with( "[]" );
                const std::string name    = element ? segment.substr( 0, segment.size() - 2 ) : segment;
                at                        = nullptr;
                if ( const auto found = byPath.find( prefix + name ); found != byPath.end() )
                    at = found->second;
                else
                    for ( const SchemaField& f : fields )
                        if ( ParentPrefix( f.path ) == prefix &&
                             std::find( f.wasNamed.begin(), f.wasNamed.end(), name ) != f.wasNamed.end() )
                        {
                            at      = &f;
                            renamed = true;
                            break;
                        }
                lockedPrefix += name;
                if ( at == nullptr )
                {
                    missing = lockedPrefix;
                    return nullptr;
                }
                prefix = at->path + ( element ? "[]." : "." );
                lockedPrefix += element ? "[]." : ".";
            }
            return at;
        }

        // The locked enumerators all still there with the same values (new ones may follow).
        bool EnumKept( const SchemaField& locked, const SchemaField& current )
        {
            for ( const auto& e : locked.enumerators )
                if ( std::find( current.enumerators.begin(), current.enumerators.end(), e ) == current.enumerators.end() )
                    return false;
            return true;
        }
    } // namespace

    std::string CanonicalSchemaType( const std::string& cppSpelling )
    {
        std::string out;
        for ( std::size_t i = 0; i < cppSpelling.size(); )
        {
            const char c = cppSpelling[i];
            if ( std::isspace( static_cast<unsigned char>( c ) ) )
            {
                // Kept only between two identifiers ("unsigned int").
                std::size_t next = i;
                while ( next < cppSpelling.size() && std::isspace( static_cast<unsigned char>( cppSpelling[next] ) ) )
                    ++next;
                if ( !out.empty() && IsIdent( out.back() ) && next < cppSpelling.size() && IsIdent( cppSpelling[next] ) )
                    out += ' ';
                i = next;
                continue;
            }
            if ( c == ':' && i + 1 < cppSpelling.size() && cppSpelling[i + 1] == ':' )
            {
                // Drop the qualifier just written: back to the start of its identifier.
                while ( !out.empty() && IsIdent( out.back() ) )
                    out.pop_back();
                i += 2;
                continue;
            }
            out += c;
            ++i;
        }
        return out;
    }

    std::string WriteSchemaLock( const std::vector<SchemaBlock>& blocks )
    {
        std::vector<const SchemaBlock*> ordered;
        for ( const SchemaBlock& b : blocks )
            ordered.push_back( &b );
        std::sort( ordered.begin(), ordered.end(),
                   []( const SchemaBlock* a, const SchemaBlock* b ) { return a->key < b->key; } );

        std::string text =
             "# GENERATED by DesertHeaderTool on every build (Tools/DesertHeaderTool/Source/SchemaLock.hpp) - commit it\n"
             "# with the change that moved it. The stored shape of every COMPONENT block: \"[Key] Version N\", then one\n"
             "# \"path: type\" line per reflected field. A new field only extends the lock; a field removed, renamed or\n"
             "# retyped fails the build until it carries PROPERTY( WasNamed( \"Old\" ) ) or the block COMPONENT( Version( N+1 ) ).\n";
        for ( const SchemaBlock* b : ordered )
        {
            text += std::format( "\n[{}] Version {}\n", b->key, b->version );
            for ( const SchemaField& f : b->fields )
                text += std::format( "{}: {}\n", f.path, f.enumerators.empty() ? f.type : EnumText( f ) );
        }
        return text;
    }

    std::vector<SchemaBlock> ReadSchemaLock( const std::string& text, std::vector<std::string>& errors )
    {
        std::vector<SchemaBlock> blocks;
        std::istringstream       in( text );
        std::string              raw;
        int                      line = 0;
        while ( std::getline( in, raw ) )
        {
            ++line;
            const std::string l = Trimmed( raw );
            if ( l.empty() || l.front() == '#' )
                continue;
            if ( l.front() == '[' )
            {
                const auto  close = l.find( ']' );
                const auto  ver   = l.find( "Version", close == std::string::npos ? 0 : close );
                SchemaBlock b;
                unsigned    version = 0;
                if ( close != std::string::npos && ver != std::string::npos )
                {
                    const std::string digits = Trimmed( l.substr( ver + 7 ) );
                    std::from_chars( digits.data(), digits.data() + digits.size(), version );
                }
                if ( close == std::string::npos || version == 0 )
                {
                    errors.push_back( std::format( "ComponentSchema.lock:{}: expected \"[Key] Version N\", got '{}'", line, l ) );
                    continue;
                }
                b.key     = l.substr( 1, close - 1 );
                b.version = version;
                blocks.push_back( std::move( b ) );
                continue;
            }
            const auto colon = l.find( ": " );
            if ( blocks.empty() || colon == std::string::npos )
            {
                errors.push_back( std::format( "ComponentSchema.lock:{}: expected \"path: type\" inside a block, got '{}'", line, l ) );
                continue;
            }
            SchemaField f;
            f.path            = l.substr( 0, colon );
            std::string type  = l.substr( colon + 2 );
            const auto  brace = type.find( " {" );
            if ( type.starts_with( "enum " ) && brace != std::string::npos && type.ends_with( "}" ) )
            {
                for ( const std::string& entry : SplitTopLevel( type.substr( brace + 2, type.size() - brace - 3 ) ) )
                {
                    const std::string e  = Trimmed( entry );
                    const auto        eq = e.find( '=' );
                    long long         v  = 0;
                    if ( eq == std::string::npos ||
                         std::from_chars( e.data() + eq + 1, e.data() + e.size(), v ).ec != std::errc{} )
                    {
                        errors.push_back( std::format( "ComponentSchema.lock:{}: enumerator '{}' is not Name=Value", line, e ) );
                        continue;
                    }
                    f.enumerators.emplace_back( e.substr( 0, eq ), v );
                }
                type.resize( brace );
            }
            f.type = std::move( type );
            blocks.back().fields.push_back( std::move( f ) );
        }
        return blocks;
    }

    SchemaLockVerdict CheckSchemaLock( const std::vector<SchemaBlock>& locked, const std::vector<SchemaBlock>& current )
    {
        SchemaLockVerdict                         verdict;
        std::map<std::string, const SchemaBlock*> currentByKey;
        for ( const SchemaBlock& b : current )
            currentByKey[b.key] = &b;
        std::set<std::string> lockedKeys;

        for ( const SchemaBlock& l : locked )
        {
            lockedKeys.insert( l.key );
            const auto found = currentByKey.find( l.key );
            if ( found == currentByKey.end() )
            {
                verdict.errors.push_back( std::format(
                     "[{0}] is in ComponentSchema.lock but no COMPONENT( Key( \"{0}\" ) ) states it: the scenes that store "
                     "it need its upgrade step; then delete the [{0}] section of Desert/Schema/ComponentSchema.lock in "
                     "the same change",
                     l.key ) );
                continue;
            }
            const SchemaBlock& c = *found->second;
            if ( c.version < l.version )
            {
                verdict.errors.push_back( std::format( "{}: COMPONENT {}: Version( {} ) is below the locked Version( {} ) - "
                                                       "a block's version only rises",
                                                       c.where, c.key, c.version, l.version ) );
                continue;
            }
            if ( c.version > l.version )
            {
                verdict.changes.push_back(
                     std::format( "[{}] Version {} -> {}: the new shape is locked as is", c.key, l.version, c.version ) );
                continue;
            }

            std::map<std::string, const SchemaField*> byPath;
            for ( const SchemaField& f : c.fields )
                byPath[f.path] = &f;
            std::set<std::string> explained; // current paths a locked one maps to
            std::set<std::string> gone;      // locked paths already reported (their children follow from them)
            for ( const SchemaField& lf : l.fields )
            {
                if ( std::any_of( gone.begin(), gone.end(), [&]( const std::string& g )
                                  { return lf.path.starts_with( g + "." ) || lf.path.starts_with( g + "[]." ); } ) )
                    continue;
                std::string        missing;
                bool               renamed = false;
                const SchemaField* cf      = Resolve( lf.path, byPath, c.fields, missing, renamed );
                if ( cf == nullptr )
                {
                    gone.insert( missing );
                    if ( missing != lf.path )
                        continue; // reported with its parent
                    const std::string leaf = Leaf( missing );
                    verdict.errors.push_back( std::format(
                         "{0}.{1} removed: add WasNamed or Version({2})+upgrade\n"
                         "    renamed: PROPERTY( ..., WasNamed( \"{3}\" ) ) on the field that replaces it (old scenes are read "
                         "by the old key)\n"
                         "    removed or reshaped: COMPONENT( Key( \"{0}\" ), ..., Version( {2} ) ) at {4} and the block's "
                         "upgrade step from {5}",
                         l.key, missing, l.version + 1, leaf, c.where, l.version ) );
                    continue;
                }
                explained.insert( cf->path );
                const bool sameType =
                     cf->type == lf.type && ( lf.enumerators.empty() ? cf->enumerators.empty() : EnumKept( lf, *cf ) );
                if ( !sameType )
                {
                    verdict.errors.push_back( std::format(
                         "{0}.{1} type {2} -> {3}: old scenes store the old type - COMPONENT( Key( \"{0}\" ), ..., "
                         "Version( {4} ) ) at {5} and the block's upgrade step from {6}",
                         l.key, lf.path, lf.enumerators.empty() ? lf.type : EnumText( lf ),
                         cf->enumerators.empty() ? cf->type : EnumText( *cf ), l.version + 1, c.where, l.version ) );
                    continue;
                }
                if ( renamed )
                    verdict.changes.push_back( std::format( "{}.{} -> {}.{} (WasNamed)", l.key, lf.path, c.key, cf->path ) );
                else if ( cf->enumerators.size() > lf.enumerators.size() )
                    verdict.changes.push_back( std::format( "{}.{}: enumerators added", c.key, cf->path ) );
            }
            for ( const SchemaField& f : c.fields )
            {
                if ( !explained.contains( f.path ) )
                    verdict.changes.push_back( std::format( "+ {}.{}: {}", c.key, f.path, f.type ) );
                // A WasNamed that is also a live sibling would read another field's value.
                for ( const std::string& old : f.wasNamed )
                    if ( byPath.contains( ParentPrefix( f.path ) + old ) )
                        verdict.errors.push_back( std::format(
                             "{}: {}.{}: WasNamed( \"{}\" ) names a field that still exists beside it", c.where, c.key,
                             f.path, old ) );
            }
        }
        for ( const SchemaBlock& c : current )
            if ( !lockedKeys.contains( c.key ) )
                verdict.changes.push_back( std::format( "+ [{}] Version {} ({} fields)", c.key, c.version, c.fields.size() ) );
        return verdict;
    }
} // namespace Desert::HeaderTool
