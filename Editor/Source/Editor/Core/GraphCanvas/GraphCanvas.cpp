#include "GraphCanvas.hpp"

#include <cstring>

namespace Desert::Editor::Graph
{
    namespace
    {
        // FNV-1a, 64 bit — the same function `ShaderGraphDeterminism` hashes emitted DSL with, so the two
        // numbers are comparable by eye in a log and neither needs a second implementation to explain.
        constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
        constexpr uint64_t kFnvPrime  = 1099511628211ULL;

        void MixBytes( uint64_t& hash, const void* data, size_t size )
        {
            const auto* bytes = static_cast<const unsigned char*>( data );
            for ( size_t i = 0; i < size; ++i )
            {
                hash ^= static_cast<uint64_t>( bytes[i] );
                hash *= kFnvPrime;
            }
        }

        void MixString( uint64_t& hash, const std::string& text )
        {
            // The LENGTH is mixed too, so "ab" + "c" and "a" + "bc" are different graphs. Two adjacent
            // keys concatenating to the same bytes is exactly how a fingerprint stops noticing a rename.
            const auto length = static_cast<uint64_t>( text.size() );
            MixBytes( hash, &length, sizeof( length ) );
            MixBytes( hash, text.data(), text.size() );
        }

        void MixU64( uint64_t& hash, uint64_t value )
        {
            MixBytes( hash, &value, sizeof( value ) );
        }

        void MixFloat( uint64_t& hash, float value )
        {
            // THE BIT PATTERN, not the value. A position that moved by one ulp moved, and a fingerprint
            // that rounds first cannot say so.
            uint32_t bits = 0;
            std::memcpy( &bits, &value, sizeof( bits ) );
            MixBytes( hash, &bits, sizeof( bits ) );
        }
    } // namespace

    ElementKind KindOf( ElementId id )
    {
        const uint64_t raw = Raw( id );
        if ( raw >= kLinkBase )
            return ElementKind::Link;
        if ( raw >= kPinBase )
            return ElementKind::Pin;
        return ElementKind::Node;
    }

    uint64_t BaseOf( ElementKind kind )
    {
        switch ( kind )
        {
            case ElementKind::Pin:
                return kPinBase;
            case ElementKind::Link:
                return kLinkBase;
            case ElementKind::Node:
            default:
                return kNodeBase;
        }
    }

    // ── ElementLedger ─────────────────────────────────────────────────────────────────────────────────

    void ElementLedger::BeginFrame()
    {
        m_SeenThisFrame.clear();
    }

    bool ElementLedger::See( ElementId id )
    {
        const uint64_t raw = Raw( id );
        m_SeenThisFrame.insert( raw );
        return m_Known.insert( raw ).second; // true == inserted == never seen before
    }

    void ElementLedger::EndFrame()
    {
        // AN ID THAT WAS NOT DRAWN THIS FRAME IS GONE, and its next appearance must be fresh. Keeping it
        // would mean a state deleted and re-created under the same name inherits the canvas position of
        // the one that was deleted — a ghost, and precisely the class of surprise this layer exists to
        // remove.
        for ( auto it = m_Known.begin(); it != m_Known.end(); )
        {
            if ( m_SeenThisFrame.find( *it ) == m_SeenThisFrame.end() )
                it = m_Known.erase( it );
            else
                ++it;
        }
    }

    size_t ElementLedger::LiveCount() const
    {
        return m_Known.size();
    }

    bool ElementLedger::IsKnown( ElementId id ) const
    {
        return m_Known.find( Raw( id ) ) != m_Known.end();
    }

    // ── ElementIdMap ──────────────────────────────────────────────────────────────────────────────────

    std::string ElementIdMap::TableKey( ElementKind kind, std::string_view key )
    {
        std::string composed;
        composed.reserve( key.size() + 2 );
        composed.push_back( static_cast<char>( '0' + static_cast<int>( kind ) ) );
        composed.push_back( '\x1f' ); // ASCII unit separator: not legal in a state name typed by a human
        composed.append( key );
        return composed;
    }

    void ElementIdMap::BeginFrame()
    {
        m_Ledger.BeginFrame();
    }

    Resolved ElementIdMap::Resolve( ElementKind kind, std::string_view key )
    {
        const std::string table = TableKey( kind, key );

        uint64_t   id  = 0;
        const auto hit = m_ByKey.find( table );
        if ( hit != m_ByKey.end() )
        {
            id = hit->second;
        }
        else
        {
            id = BaseOf( kind ) + m_Next[static_cast<size_t>( kind )]++;
            m_ByKey.emplace( table, id );
            m_ById.emplace( id, std::string( key ) );
        }

        Resolved out;
        out.Id    = static_cast<ElementId>( id );
        out.Fresh = m_Ledger.See( out.Id );
        return out;
    }

    void ElementIdMap::EndFrame()
    {
        m_Ledger.EndFrame();

        // The key table follows the ledger rather than keeping its own bookkeeping: an id the ledger has
        // forgotten names an element that was not drawn, so its key must lose its id too. Two copies of
        // "is this element still here" is the shape that drifts.
        for ( auto it = m_ByKey.begin(); it != m_ByKey.end(); )
        {
            if ( m_Ledger.IsKnown( static_cast<ElementId>( it->second ) ) )
            {
                ++it;
                continue;
            }
            m_ById.erase( it->second );
            it = m_ByKey.erase( it );
        }
    }

    const std::string* ElementIdMap::KeyOf( ElementId id ) const
    {
        const auto hit = m_ById.find( Raw( id ) );
        return hit == m_ById.end() ? nullptr : &hit->second;
    }

    ElementId ElementIdMap::Lookup( ElementKind kind, std::string_view key ) const
    {
        const auto hit = m_ByKey.find( TableKey( kind, key ) );
        return hit == m_ByKey.end() ? ElementId::Invalid : static_cast<ElementId>( hit->second );
    }

    size_t ElementIdMap::LiveCount() const
    {
        return m_ByKey.size();
    }

    // ── DeferredFrameAll ──────────────────────────────────────────────────────────────────────────────

    void DeferredFrameAll::Request()
    {
        m_FramesLeft = kFramesUntilCanvasExists;
    }

    bool DeferredFrameAll::Tick()
    {
        if ( m_FramesLeft <= 0 )
            return false;
        --m_FramesLeft;
        return m_FramesLeft == 0;
    }

    // ── The plan's fingerprint ────────────────────────────────────────────────────────────────────────

    uint64_t Fingerprint( const CanvasPlan& plan )
    {
        uint64_t hash = kFnvOffset;

        MixU64( hash, static_cast<uint64_t>( plan.Nodes.size() ) );
        for ( const auto& node : plan.Nodes )
        {
            MixU64( hash, Raw( node.Id ) );
            MixString( hash, node.Key );
            MixFloat( hash, node.X );
            MixFloat( hash, node.Y );
            MixU64( hash, node.PushPosition ? 1ULL : 0ULL );
        }

        MixU64( hash, static_cast<uint64_t>( plan.Links.size() ) );
        for ( const auto& link : plan.Links )
        {
            MixU64( hash, Raw( link.Id ) );
            MixString( hash, link.Key );
            MixU64( hash, link.FromPin );
            MixU64( hash, link.ToPin );
        }

        return hash;
    }
} // namespace Desert::Editor::Graph
