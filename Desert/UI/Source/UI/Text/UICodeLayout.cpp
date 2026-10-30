#include <UI/Text/UICodeLayout.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string_view>

namespace Desert::UI
{
    void UICodeLayout::Update( const std::string& text, const IUICodeTokenizer* tokenizer )
    {
        if ( m_Valid && tokenizer == m_Tokenizer && text == m_Text )
            return;
        m_Text      = text;
        m_Tokenizer = tokenizer;
        m_Valid     = true;

        m_Starts.assign( 1, 0 );
        for ( std::size_t i = 0; i < m_Text.size(); ++i )
            if ( m_Text[i] == '\n' )
                m_Starts.push_back( i + 1 );

        // Every line's entry state, top to bottom: what a line inherits is decided by every line above it, so
        // this is the one pass that reads the whole text — on a change only (UE re-marshals the whole text on
        // every change as well).
        m_Entry.assign( m_Starts.size(), 0 );
        if ( m_Tokenizer == nullptr )
            return;
        std::vector<UICodeToken> scratch;
        UICodeLineState          state = 0;
        for ( std::size_t line = 0; line < m_Starts.size(); ++line )
        {
            m_Entry[line] = state;
            scratch.clear();
            const std::string_view view( m_Text );
            state = m_Tokenizer->Tokenize( view.substr( LineStart( line ), LineEnd( line ) - LineStart( line ) ),
                                           state, scratch );
        }
    }

    std::size_t UICodeLayout::LineStart( std::size_t line ) const
    {
        return m_Starts[std::min( line, m_Starts.size() - 1 )];
    }

    std::size_t UICodeLayout::LineEnd( std::size_t line ) const
    {
        if ( line + 1 < m_Starts.size() )
            return m_Starts[line + 1] - 1;
        return m_Text.size();
    }

    std::size_t UICodeLayout::LineOf( std::size_t offset ) const
    {
        const auto after = std::upper_bound( m_Starts.begin(), m_Starts.end(), offset );
        return static_cast<std::size_t>( std::distance( m_Starts.begin(), after ) ) - 1;
    }

    void UICodeLayout::Runs( std::size_t line, std::vector<UICodeToken>& out ) const
    {
        const std::size_t begin = LineStart( line );
        const std::size_t end   = LineEnd( line );
        if ( end <= begin )
            return;
        std::vector<UICodeToken> tokens;
        if ( m_Tokenizer != nullptr )
            (void)m_Tokenizer->Tokenize( std::string_view( m_Text ).substr( begin, end - begin ),
                                         m_Entry[std::min( line, m_Entry.size() - 1 )], tokens );
        std::size_t at = begin;
        for ( const UICodeToken& t : tokens )
        {
            const std::size_t from = begin + t.Begin;
            const std::size_t to   = std::min( begin + t.End, end );
            if ( from < at || to <= from )
                continue; // a tokenizer that overlaps itself loses the overlap, never the text
            if ( from > at )
                out.push_back( { at, from, UICodeTokenKind::Text } );
            out.push_back( { from, to, t.Kind } );
            at = to;
        }
        if ( at < end )
            out.push_back( { at, end, UICodeTokenKind::Text } );
    }

    std::pair<std::size_t, std::size_t> UICodeLayout::VisibleLines( float scrollY, float heightPx,
                                                                    float lineH ) const
    {
        if ( lineH <= 0.0f )
            return { 0, LineCount() };
        const float       top   = std::max( 0.0f, scrollY );
        const std::size_t first = std::min( static_cast<std::size_t>( std::floor( top / lineH ) ), LineCount() );
        const std::size_t last  = std::min(
             static_cast<std::size_t>( std::ceil( ( top + std::max( 0.0f, heightPx ) ) / lineH ) ), LineCount() );
        return { first, std::max( first, last ) };
    }
} // namespace Desert::UI
