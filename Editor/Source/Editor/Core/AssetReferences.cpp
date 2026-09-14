#include "AssetReferences.hpp"

#include <algorithm>
#include <cctype>

namespace Desert::Editor
{
    namespace
    {
        bool IsAllDigits( const std::string& s )
        {
            return !s.empty() &&
                   std::all_of( s.begin(), s.end(), []( unsigned char c ) { return std::isdigit( c ); } );
        }

        // Does @p text contain @p token? For all-digit tokens (asset handles) the match must be
        // digit-bounded so a handle isn't found as a substring of a longer number.
        bool ContainsToken( const std::string& text, const std::string& token )
        {
            if ( token.empty() || text.size() < token.size() )
                return false;

            const bool numeric = IsAllDigits( token );
            for ( std::size_t pos = text.find( token ); pos != std::string::npos;
                  pos            = text.find( token, pos + 1 ) )
            {
                if ( !numeric )
                    return true;
                const bool leftOk =
                     pos == 0 || !std::isdigit( static_cast<unsigned char>( text[pos - 1] ) );
                const std::size_t after = pos + token.size();
                const bool        rightOk =
                     after >= text.size() || !std::isdigit( static_cast<unsigned char>( text[after] ) );
                if ( leftOk && rightOk )
                    return true;
            }
            return false;
        }
    } // namespace

    void AssetReferenceIndex::Clear()
    {
        m_Entries.clear();
    }

    void AssetReferenceIndex::Add( Entry entry )
    {
        m_Entries.push_back( std::move( entry ) );
    }

    const std::vector<AssetReferenceIndex::Entry>& AssetReferenceIndex::Entries() const
    {
        return m_Entries;
    }

    const AssetReferenceIndex::Entry* AssetReferenceIndex::Find( const std::string& path ) const
    {
        for ( const auto& e : m_Entries )
            if ( e.Path == path )
                return &e;
        return nullptr;
    }

    std::vector<std::string> AssetReferenceIndex::ReferencersOf( const std::string& path ) const
    {
        std::vector<std::string> out;
        const Entry*             target = Find( path );
        if ( !target )
            return out;

        for ( const auto& e : m_Entries )
        {
            if ( &e == target || e.Text.empty() )
                continue;
            const bool refs = std::any_of( target->Tokens.begin(), target->Tokens.end(),
                                           [&]( const std::string& t ) { return ContainsToken( e.Text, t ); } );
            if ( refs )
                out.push_back( e.Path );
        }
        std::sort( out.begin(), out.end() );
        out.erase( std::unique( out.begin(), out.end() ), out.end() );
        return out;
    }

    bool AssetReferenceIndex::IsReferenced( const std::string& path ) const
    {
        return !ReferencersOf( path ).empty();
    }

    std::vector<std::string> AssetReferenceIndex::ReferencedBy( const std::string& path ) const
    {
        std::vector<std::string> out;
        const Entry*             source = Find( path );
        if ( !source || source->Text.empty() )
            return out; // a binary has no text to scan, so it names nothing — not "it names nothing yet"

        for ( const auto& e : m_Entries )
        {
            if ( &e == source )
                continue;
            const bool refs = std::any_of( e.Tokens.begin(), e.Tokens.end(), [&]( const std::string& t )
                                           { return ContainsToken( source->Text, t ); } );
            if ( refs )
                out.push_back( e.Path );
        }
        std::sort( out.begin(), out.end() );
        out.erase( std::unique( out.begin(), out.end() ), out.end() );
        return out;
    }

    std::vector<std::string> AssetReferenceIndex::ClosureFrom( const std::string& path ) const
    {
        std::vector<std::string> reached;
        if ( !Find( path ) )
            return reached;

        // Breadth-first over the reference edges. `reached` doubles as the visited set and as the
        // frontier, which is why the index into it is the loop variable rather than a queue: a cycle
        // (two prefabs naming each other) then costs one extra membership test instead of hanging.
        reached.push_back( path );
        for ( std::size_t i = 0; i < reached.size(); ++i )
        {
            for ( const auto& next : ReferencedBy( reached[i] ) )
            {
                if ( std::find( reached.begin(), reached.end(), next ) == reached.end() )
                    reached.push_back( next );
            }
        }
        std::sort( reached.begin(), reached.end() );
        return reached;
    }

    std::vector<std::string> AssetReferenceIndex::Orphans( const std::vector<std::string>& leafExts ) const
    {
        std::vector<std::string> out;
        for ( const auto& e : m_Entries )
        {
            const bool isLeaf = std::find( leafExts.begin(), leafExts.end(), e.Ext ) != leafExts.end();
            if ( isLeaf && !IsReferenced( e.Path ) )
                out.push_back( e.Path );
        }
        std::sort( out.begin(), out.end() );
        return out;
    }

    std::vector<WithheldRemoval> WithholdReferencedRemovals( Common::Utils::ContentUpdatePlan& plan,
                                                             const AssetReferenceIndex&        index,
                                                             const std::string&                indexPathPrefix )
    {
        // Decided over a snapshot of the keys, then applied — rather than mutating the plan while
        // walking it, which happens to be safe here only because no step is added or removed.
        std::vector<WithheldRemoval> withheld;
        std::vector<std::string>     planned;
        for ( const auto& step : plan.Steps )
            if ( step.Action == Common::Utils::ContentAction::Remove )
                planned.push_back( step.Key );

        for ( const auto& key : planned )
        {
            const std::string indexPath   = indexPathPrefix.empty() ? key : indexPathPrefix + "/" + key;
            const auto        referencers = index.ReferencersOf( indexPath );
            if ( referencers.empty() )
                continue; // nothing points at it: the source dropped it and so may we

            // WithholdRemoval cannot fail here — the key came out of the plan's own Remove steps — but
            // it is asked rather than assumed, because "cannot fail" is a property of this loop and the
            // plan does not know that. A silent no-op would mean reporting a file as saved and deleting
            // it in the same breath.
            if ( plan.WithholdRemoval( key ) )
                withheld.push_back( { key, referencers.front(), referencers.size() } );
        }
        return withheld;
    }
} // namespace Desert::Editor
