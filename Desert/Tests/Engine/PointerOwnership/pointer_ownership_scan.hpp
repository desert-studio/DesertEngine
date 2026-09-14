#pragma once

// THE SUBJECT OF THE POINTER-OWNERSHIP CENSUS: every pointer-typed NON-STATIC DATA MEMBER in the
// trees it covers, found in the source text.
//
// WHY A DATA MEMBER AND NOT "EVERY USE OF A POINTER". The owner's request was an audit of where
// shared_ptr, unique_ptr and raw pointers are used and which one each place should be. Taken
// literally that is every local, every parameter and every return type in 914 files, and it
// degenerates into taste. A LOCAL OR A PARAMETER OWNS NOTHING BY CONSTRUCTION: its lifetime is the
// call, the caller holds the object for at least that long, and the only question it can raise is a
// missing `const&`. A DATA MEMBER is the only place in C++ where the two questions this census is
// built on can actually be answered differently by different code:
//
//   Q1. Who is OBLIGED to destroy this object?
//   Q2. Can the observed object die before the thing observing it, and what stops that?
//
// So the enumerable unit is the member, the number is exact, and a new member cannot appear without
// this census noticing.
//
// HOW A MEMBER IS TOLD FROM A LOCAL. Brace depth, tracked against the brace that opened a `class` or
// `struct` body. A declaration sitting directly in a class body at that depth is a member; anything
// one brace deeper is inside a function body and is a local. A declaration containing parentheses is
// a function declaration and is skipped -- which also drops the handful of members whose type spells
// parentheses (`std::function<...>` has none; a function-pointer member would), and that is stated
// here rather than hidden, because the census's number is only honest if what it cannot see is named.
//
// Comments and literals are blanked first with the shared reader (Д33). A private copy of that logic
// went blind at the first quote inside a character literal and silently under-counted, so there is
// exactly one implementation of "what is code" in this repository and this file uses it.

#include "../SettingConsumers/setting_consumers_reader.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace Desert::Tests::PointerCensus
{
    namespace fs = std::filesystem;

    // The four forms a pointer member can take. The FORM is what answers Q1 and, for two of them, Q2
    // as well -- which is the whole reason the census classifies by it before it argues about
    // anything.
    enum class Form
    {
        Unique, // sole owner, said in the type. Q1 answered by the type; Q2 cannot arise.
        Weak,   // observer that cannot dangle. Q2 answered by the type.
        Shared, // shared ownership CLAIMED. Q1 says "several owners, unordered deaths".
        Raw     // an observer with no guarantee of any kind. Q2 is live and must be answered by hand.
    };

    inline const char* FormName( Form f )
    {
        switch ( f )
        {
            case Form::Unique:
                return "unique_ptr";
            case Form::Weak:
                return "weak_ptr";
            case Form::Shared:
                return "shared_ptr";
            case Form::Raw:
                return "raw";
        }
        return "?";
    }

    struct Member
    {
        std::string File; // repository-relative
        int         Line = 0;
        std::string Class;
        std::string Name;
        std::string Decl; // the declaration text, whitespace-collapsed
        Form        Kind = Form::Raw;
    };

    inline std::string ReadAll( const fs::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    inline std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Graphic/Materials/Material.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    // The trees this census covers. STAGE 1 IS THE GPU-RESOURCE AND ASSET FAMILIES, deliberately:
    // a lifetime mistake there is a use-after-free of a device object or of a loaded asset, which is
    // the most expensive kind this engine has. Widening the list is how the next stage lands, and it
    // is one line plus the rows it brings.
    inline std::vector<const char*> ScannedTrees()
    {
        return { "Desert/Desert/Source", "Desert/Common/Source", "Editor/Source", "Runtime/Source" };
    }

    inline std::vector<fs::path> ScannedSources( const std::string& root )
    {
        std::vector<fs::path> out;
        for ( const char* tree : ScannedTrees() )
        {
            std::error_code ec;
            const fs::path  base = fs::path( root ) / tree;
            for ( auto it = fs::recursive_directory_iterator( base, ec );
                  !ec && it != fs::recursive_directory_iterator(); ++it )
            {
                const fs::path& p = it->path();
                // A vendored third-party tree that happens to live under our Vulkan folder.
                if ( p.string().find( "lightweightvk" ) != std::string::npos )
                    continue;
                if ( p.extension() == ".cpp" || p.extension() == ".hpp" )
                    out.push_back( p );
            }
        }
        std::sort( out.begin(), out.end() );
        return out;
    }

    // THE PROJECT'S OWN SMART-POINTER ALIASES, DERIVED AND NOT TYPED.
    //
    // `MaterialSlotBindingPtr MaterialSlots;` spells neither `shared_ptr` nor `*`, so a scanner that only
    // knows the standard names sees NOTHING there — the member disappears from the census rather than
    // being counted under the wrong form, which is the worse of the two failures and the one this census
    // exists to prevent. It was found the honest way: A8-3 converted eight raw members to co-owned handles
    // and the total fell by five instead of holding, because three of them had become invisible.
    //
    // Typed by hand the list would drift the moment somebody adds the ninth alias, so it is read off the
    // tree exactly as the sweep's tool list is read off `ls Tools`: every `using X = std::shared_ptr<...>`
    // in the scanned trees plus the two the engine declares outside them (`Common::Unique`,
    // `Assets::Asset`) is an alias, and a member naming one is that form.
    struct Aliases
    {
        std::vector<std::string> Shared;
        std::vector<std::string> Unique;
        std::vector<std::string> Weak;
    };

    inline Aliases DeclaredAliases( const std::string& root )
    {
        Aliases out;
        // The two aliases declared outside the scanned trees. Named here because they are reachable from
        // inside them, and a census that could not see `Common::Unique<T> m_X` would under-count in the
        // same direction as the defect above.
        out.Unique.push_back( "Unique" );
        out.Shared.push_back( "Asset" );

        for ( const char* tree :
              { "Desert/Desert/Source", "Desert/Common/Source", "Editor/Source", "Runtime/Source" } )
        {
            std::error_code ec;
            const fs::path  base = fs::path( root ) / tree;
            for ( auto it = fs::recursive_directory_iterator( base, ec );
                  !ec && it != fs::recursive_directory_iterator(); ++it )
            {
                const fs::path& p = it->path();
                if ( p.string().find( "lightweightvk" ) != std::string::npos )
                    continue;
                if ( p.extension() != ".hpp" )
                    continue;

                const std::string src = ConsumerText::StripCommentsAndLiterals( ReadAll( p ) );
                for ( std::size_t at : ConsumerText::WordPositions( src, "using" ) )
                {
                    std::size_t       i    = ConsumerText::SkipSpace( src, at + 5 );
                    const std::string name = ConsumerText::IdentAt( src, i );
                    if ( name.empty() )
                        continue;
                    i = ConsumerText::SkipSpace( src, i + name.size() );
                    if ( i >= src.size() || src[i] != '=' )
                        continue;
                    const std::size_t semi = src.find( ';', i );
                    if ( semi == std::string::npos )
                        continue;
                    const std::string rhs = src.substr( i + 1, semi - i - 1 );

                    std::vector<std::string>* bucket = nullptr;
                    if ( rhs.find( "std::shared_ptr" ) != std::string::npos )
                        bucket = &out.Shared;
                    else if ( rhs.find( "std::unique_ptr" ) != std::string::npos )
                        bucket = &out.Unique;
                    else if ( rhs.find( "std::weak_ptr" ) != std::string::npos )
                        bucket = &out.Weak;
                    if ( !bucket )
                        continue;
                    if ( std::find( bucket->begin(), bucket->end(), name ) == bucket->end() )
                        bucket->push_back( name );
                }
            }
        }
        return out;
    }

    inline int LineOf( const std::string& src, std::size_t at )
    {
        return 1 + static_cast<int>( std::count( src.begin(), src.begin() + at, '\n' ) );
    }

    inline std::string Collapse( const std::string& s )
    {
        std::string out;
        bool        space = false;
        for ( char c : s )
        {
            if ( std::isspace( static_cast<unsigned char>( c ) ) != 0 )
            {
                space = !out.empty();
                continue;
            }
            if ( space )
                out += ' ';
            space = false;
            out += c;
        }
        return out;
    }

    // The name being declared, with any array extent removed: `Image2D* Slots[4]` declares `Slots`.
    inline std::string DeclaredMemberName( const std::string& decl )
    {
        std::size_t end = decl.size();
        while ( end > 0 && std::isspace( static_cast<unsigned char>( decl[end - 1] ) ) != 0 )
            --end;
        if ( end > 0 && decl[end - 1] == ']' )
        {
            int depth = 0;
            while ( end > 0 )
            {
                --end;
                if ( decl[end] == ']' )
                    ++depth;
                else if ( decl[end] == '[' && --depth == 0 )
                    break;
            }
            while ( end > 0 && std::isspace( static_cast<unsigned char>( decl[end - 1] ) ) != 0 )
                --end;
        }
        const std::size_t last = end;
        while ( end > 0 && ConsumerText::IsIdentChar( decl[end - 1] ) )
            --end;
        return decl.substr( end, last - end );
    }

    // The class or struct whose body this brace opens, or empty when the brace opens anything else
    // (a function, a namespace, an initializer). `head` is the text since the previous statement
    // boundary.
    inline std::string ClassNameOpenedBy( const std::string& head )
    {
        std::size_t found = std::string::npos;
        std::size_t after = 0;
        for ( const char* kw : { "class", "struct" } )
        {
            for ( std::size_t at : ConsumerText::WordPositions( head, kw ) )
            {
                found = at;
                after = at + std::string( kw ).size();
            }
        }
        if ( found == std::string::npos )
            return {};
        // `class Foo;` forward declarations never reach here (no brace), but `void f( class Foo* p )`
        // does -- the parenthesis is what tells them apart.
        if ( head.find( '(', found ) != std::string::npos )
            return {};

        std::size_t i    = ConsumerText::SkipSpace( head, after );
        std::string name = ConsumerText::IdentAt( head, i );

        // `class DESERT_API Foo` — an all-caps run is an export macro, not the name. Anything else
        // that follows (`final`, `:`) is not the name either, which is why this takes the FIRST
        // identifier and not the last: taking the last named half the render materials `final`.
        const auto shouty = []( const std::string& id )
        {
            for ( char c : id )
                if ( std::islower( static_cast<unsigned char>( c ) ) != 0 )
                    return false;
            return !id.empty();
        };
        while ( shouty( name ) )
        {
            i                      = ConsumerText::SkipSpace( head, i + name.size() );
            const std::string next = ConsumerText::IdentAt( head, i );
            if ( next.empty() )
                break;
            name = next;
        }
        return name;
    }

    inline std::vector<Member> ScanMembers( const std::string& root )
    {
        using namespace ConsumerText;

        const Aliases aliases = DeclaredAliases( root );

        // AN ALIAS NAMES A TYPE, SO IT IS LOOKED FOR IN THE TYPE AND NOT IN THE WHOLE DECLARATION.
        //
        // Found by Ю13: `std::string Asset;` — a font token's PATH inside a UI theme — was counted as a
        // `shared_ptr`, because `Assets::Asset` is one of the two aliases declared outside the scanned
        // trees and the match ran over the declaration's every word, the member's NAME included. Two
        // members of a plain data struct therefore arrived on the shared side of a census whose whole
        // subject is ownership, and the count that is supposed to make a silent shift visible moved for a
        // reason that had nothing to do with ownership at all.
        //
        // The member name is the LAST identifier of the declaration (arrays and initialisers are already
        // cut off above), so dropping it leaves exactly the type. A declaration with one identifier is not
        // a member declaration and yields an empty type, which matches nothing — the same direction of
        // failure the scanner already prefers everywhere else.
        const auto typeOf = []( const std::string& decl )
        {
            std::size_t end = decl.size();
            while ( end > 0 && !IsIdentChar( decl[end - 1] ) )
                --end;
            std::size_t start = end;
            while ( start > 0 && IsIdentChar( decl[start - 1] ) )
                --start;
            return decl.substr( 0, start );
        };

        const auto names = [&]( const std::string& decl, const std::vector<std::string>& list )
        {
            const std::string type = typeOf( decl );
            for ( const std::string& alias : list )
                if ( !WordPositions( type, alias ).empty() )
                    return true;
            return false;
        };

        std::vector<Member> out;
        for ( const fs::path& path : ScannedSources( root ) )
        {
            const std::string src = StripCommentsAndLiterals( ReadAll( path ) );

            // (depth, class name or empty) for every open brace, innermost last.
            std::vector<std::pair<int, std::string>> braces;
            int                                      depth     = 0;
            std::size_t                              stmtStart = 0;
            std::string                              relative  = fs::relative( path, root ).generic_string();

            for ( std::size_t i = 0; i < src.size(); ++i )
            {
                const char c = src[i];
                if ( c == '{' )
                {
                    braces.emplace_back( ++depth, ClassNameOpenedBy( src.substr( stmtStart, i - stmtStart ) ) );
                    stmtStart = i + 1;
                    continue;
                }
                if ( c == '}' )
                {
                    if ( !braces.empty() && braces.back().first == depth )
                        braces.pop_back();
                    --depth;
                    stmtStart = i + 1;
                    continue;
                }
                if ( c != ';' )
                    continue;

                const std::string stmt = src.substr( stmtStart, i - stmtStart );
                const std::size_t at   = stmtStart;
                (void)at;
                stmtStart = i + 1;

                if ( braces.empty() || braces.back().second.empty() )
                    continue; // not directly inside a class body

                const std::string collapsed = Collapse( stmt );
                if ( collapsed.empty() )
                    continue;
                // A function declaration, a using/typedef alias, a friend, an enumerator list.
                if ( collapsed.find( '(' ) != std::string::npos || collapsed.find( ')' ) != std::string::npos )
                    continue;
                if ( !WordPositions( collapsed, "using" ).empty() ||
                     !WordPositions( collapsed, "typedef" ).empty() ||
                     !WordPositions( collapsed, "friend" ).empty() ||
                     !WordPositions( collapsed, "return" ).empty() )
                    continue;

                // Everything left of the initializer is the declaration.
                const std::string decl = collapsed.substr( 0, collapsed.find( '=' ) );

                Member m;
                m.Kind = Form::Raw;
                if ( decl.find( "shared_ptr" ) != std::string::npos || names( decl, aliases.Shared ) )
                    m.Kind = Form::Shared;
                else if ( decl.find( "unique_ptr" ) != std::string::npos || names( decl, aliases.Unique ) )
                    m.Kind = Form::Unique;
                else if ( decl.find( "weak_ptr" ) != std::string::npos || names( decl, aliases.Weak ) )
                    m.Kind = Form::Weak;
                else
                {
                    // A raw pointer member: a `*` that is followed by the declared name. `a * b` as an
                    // expression cannot appear here -- this is a declaration statement in a class body.
                    const std::size_t star = decl.rfind( '*' );
                    if ( star == std::string::npos )
                        continue;
                    const std::string tail = decl.substr( star + 1 );
                    if ( DeclaredMemberName( tail ).empty() )
                        continue;
                }

                m.File = relative;
                // The line the declaration ENDS on, not the one the statement started on: the
                // statement begins right after the previous `;`, which is usually the end of the
                // line above.
                m.Line  = LineOf( src, i );
                m.Class = braces.back().second;
                m.Name  = DeclaredMemberName( decl );
                m.Decl  = decl;
                if ( m.Name.empty() )
                    continue;
                out.push_back( m );
            }
        }
        return out;
    }
} // namespace Desert::Tests::PointerCensus
