#include <Engine/UI/UITextEditState.hpp>

#include <Engine/Text/Utf8.hpp>
#include <Engine/UI/UICanvasRenderer2D.hpp>

#include <algorithm>

namespace Desert::UI
{
    namespace
    {
        bool IsContinuation( char c )
        {
            return ( static_cast<unsigned char>( c ) & 0xC0u ) == 0x80u;
        }

        std::size_t Snap( const std::string& text, std::size_t offset )
        {
            offset = std::min( offset, text.size() );
            while ( offset > 0 && offset < text.size() && IsContinuation( text[offset] ) )
                --offset;
            return offset;
        }

        std::size_t NextBoundary( const std::string& text, std::size_t i )
        {
            if ( i >= text.size() )
                return text.size();
            ++i;
            while ( i < text.size() && IsContinuation( text[i] ) )
                ++i;
            return i;
        }

        std::size_t PrevBoundary( const std::string& text, std::size_t i )
        {
            if ( i == 0 )
                return 0;
            --i;
            while ( i > 0 && IsContinuation( text[i] ) )
                --i;
            return i;
        }

        uint32_t CodepointAt( const std::string& text, std::size_t i )
        {
            return i < text.size() ? Text::Utf8Next( text, i ) : 0u;
        }

        // Word classes for the word jumps (UE asks the ICU word-break iterator; three classes give the same
        // answer for the text a game field holds): 0 = blank, 1 = punctuation, 2 = word. Every non-ASCII
        // codepoint is a word character, so Cyrillic jumps by words as Latin does.
        int WordClass( uint32_t cp )
        {
            if ( cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' )
                return 0;
            if ( cp >= 0x80u || ( cp >= '0' && cp <= '9' ) || ( cp >= 'a' && cp <= 'z' ) ||
                 ( cp >= 'A' && cp <= 'Z' ) || cp == '_' )
                return 2;
            return 1;
        }

        std::size_t WordLeftOf( const std::string& text, std::size_t i )
        {
            while ( i > 0 && WordClass( CodepointAt( text, PrevBoundary( text, i ) ) ) == 0 )
                i = PrevBoundary( text, i );
            if ( i == 0 )
                return 0;
            const int cls = WordClass( CodepointAt( text, PrevBoundary( text, i ) ) );
            while ( i > 0 && WordClass( CodepointAt( text, PrevBoundary( text, i ) ) ) == cls )
                i = PrevBoundary( text, i );
            return i;
        }

        std::size_t WordRightOf( const std::string& text, std::size_t i )
        {
            if ( i >= text.size() )
                return text.size();
            const int cls = WordClass( CodepointAt( text, i ) );
            if ( cls != 0 )
                while ( i < text.size() && WordClass( CodepointAt( text, i ) ) == cls )
                    i = NextBoundary( text, i );
            while ( i < text.size() && WordClass( CodepointAt( text, i ) ) == 0 )
                i = NextBoundary( text, i );
            return i;
        }

        std::size_t LineEndOf( const std::string& text, std::size_t i )
        {
            const std::size_t nl = text.find( '\n', i );
            return nl == std::string::npos ? text.size() : nl;
        }

        std::size_t CountCodepoints( const std::string& text, std::size_t from, std::size_t to )
        {
            std::size_t n = 0;
            for ( std::size_t i = from; i < to; ++i )
                if ( !IsContinuation( text[i] ) )
                    ++n;
            return n;
        }

        std::size_t AdvanceCodepoints( const std::string& text, std::size_t i, std::size_t n, std::size_t limit )
        {
            while ( n-- > 0 && i < limit )
                i = NextBoundary( text, i );
            return std::min( i, limit );
        }

        // "\r\n" and a lone "\r" are both one line break.
        std::string NormalizeBreaks( std::string_view in )
        {
            std::string out;
            out.reserve( in.size() );
            for ( std::size_t i = 0; i < in.size(); ++i )
            {
                if ( in[i] == '\r' )
                {
                    out += '\n';
                    if ( i + 1 < in.size() && in[i + 1] == '\n' )
                        ++i;
                }
                else
                    out += in[i];
            }
            return out;
        }

        bool IsDigit( uint32_t cp )
        {
            return cp >= '0' && cp <= '9';
        }
    } // namespace

    UITextEditRules UITextEditRules::Of( const UIInputFieldData& f )
    {
        UITextEditRules r;
        r.MultiLine = f.MultiLine;
        r.Password  = f.Password;
        r.MaxLength = f.MaxLength > 0 ? static_cast<std::size_t>( f.MaxLength ) : 0;
        r.Filter    = f.CharFilter;
        r.NewLine   = f.NewLineKey;
        return r;
    }

    std::pair<std::size_t, std::size_t> UITextEditState::Selection() const
    {
        return { std::min( m_Caret, m_Anchor ), std::max( m_Caret, m_Anchor ) };
    }

    std::string UITextEditState::SelectedText( const std::string& text ) const
    {
        const auto [lo, hi] = Selection();
        return lo < hi && hi <= text.size() ? text.substr( lo, hi - lo ) : std::string{};
    }

    void UITextEditState::Sync( const std::string& text )
    {
        m_Caret  = Snap( text, m_Caret );
        m_Anchor = Snap( text, m_Anchor );
    }

    void UITextEditState::SetCaret( const std::string& text, std::size_t offset, bool extend )
    {
        m_Caret = Snap( text, offset );
        if ( !extend )
            m_Anchor = m_Caret;
    }

    void UITextEditState::Move( const std::string& text, UITextMove move, bool extend )
    {
        Sync( text );
        const auto [lo, hi] = Selection();
        std::size_t to      = m_Caret;
        switch ( move )
        {
            // A selection collapses to its own edge before an unextended arrow moves anything (UE MoveCursor).
            case UITextMove::Left:
                to = ( HasSelection() && !extend ) ? lo : PrevBoundary( text, m_Caret );
                break;
            case UITextMove::Right:
                to = ( HasSelection() && !extend ) ? hi : NextBoundary( text, m_Caret );
                break;
            case UITextMove::WordLeft:
                to = WordLeftOf( text, m_Caret );
                break;
            case UITextMove::WordRight:
                to = WordRightOf( text, m_Caret );
                break;
            case UITextMove::LineStart:
                to = UITextLineStart( text, m_Caret );
                break;
            case UITextMove::LineEnd:
                to = LineEndOf( text, m_Caret );
                break;
            case UITextMove::Start:
                to = 0;
                break;
            case UITextMove::End:
                to = text.size();
                break;
            case UITextMove::Up:
            case UITextMove::Down:
            {
                const std::size_t start  = UITextLineStart( text, m_Caret );
                const std::size_t column = CountCodepoints( text, start, m_Caret );
                if ( move == UITextMove::Up )
                {
                    if ( start == 0 )
                    {
                        to = 0;
                        break;
                    }
                    const std::size_t prevStart = UITextLineStart( text, start - 1 );
                    to                          = AdvanceCodepoints( text, prevStart, column, start - 1 );
                }
                else
                {
                    const std::size_t end = LineEndOf( text, m_Caret );
                    if ( end >= text.size() )
                    {
                        to = text.size();
                        break;
                    }
                    to = AdvanceCodepoints( text, end + 1, column, LineEndOf( text, end + 1 ) );
                }
                break;
            }
        }
        m_Caret = to;
        if ( !extend )
            m_Anchor = m_Caret;
    }

    void UITextEditState::SelectAll( const std::string& text )
    {
        m_Anchor = 0;
        m_Caret  = text.size();
    }

    void UITextEditState::SelectWordAt( const std::string& text, std::size_t offset )
    {
        offset = Snap( text, offset );
        if ( text.empty() )
        {
            m_Caret = m_Anchor = 0;
            return;
        }
        // The codepoint under the click: the one starting at offset, or the last one at the very end.
        const std::size_t at  = offset < text.size() ? offset : PrevBoundary( text, offset );
        const int         cls = WordClass( CodepointAt( text, at ) );
        std::size_t       lo  = at;
        std::size_t       hi  = NextBoundary( text, at );
        while ( lo > 0 && WordClass( CodepointAt( text, PrevBoundary( text, lo ) ) ) == cls )
            lo = PrevBoundary( text, lo );
        while ( hi < text.size() && WordClass( CodepointAt( text, hi ) ) == cls )
            hi = NextBoundary( text, hi );
        m_Anchor = lo;
        m_Caret  = hi;
    }

    void UITextEditState::PushUndo( const std::string& text )
    {
        m_Undo.push_back( { text, m_Caret, m_Anchor } );
        if ( m_Undo.size() > kMaxUndo )
            m_Undo.erase( m_Undo.begin() );
        m_Redo.clear();
    }

    bool UITextEditState::EraseSelection( std::string& text )
    {
        const auto [lo, hi] = Selection();
        if ( lo >= hi )
            return false;
        text.erase( lo, hi - lo );
        m_Caret = m_Anchor = lo;
        return true;
    }

    bool UITextEditState::Insert( std::string& text, std::string_view typed, const UITextEditRules& rules )
    {
        Sync( text );
        const auto [lo, hi]  = Selection();
        const std::string in = NormalizeBreaks( typed );

        // What the text will look like around the insertion, for the filter's positional rules.
        const bool atStart   = lo == 0;
        const bool restMinus = hi < text.size() && text[hi] == '-';
        // A decimal point already kept OUTSIDE the selection (the selection is about to be replaced).
        bool        hasPoint = text.find( '.' ) < lo || text.find( '.', hi ) != std::string::npos;
        std::size_t count    = CountCodepoints( text, 0, text.size() ) - CountCodepoints( text, lo, hi );
        std::string accepted;
        for ( std::size_t i = 0; i < in.size(); )
        {
            const uint32_t cp = Text::Utf8Next( in, i );
            if ( rules.MaxLength != 0 && count >= rules.MaxLength )
                break;
            const bool lineBreak = cp == '\n';
            if ( lineBreak && !rules.MultiLine )
                continue;
            if ( !lineBreak && ( cp < 0x20u || cp == 0x7Fu || ( cp >= 0x80u && cp < 0xA0u ) ) )
                continue;
            bool ok = true;
            switch ( rules.Filter )
            {
                case UITextCharFilter::Any:
                    break;
                case UITextCharFilter::Integer:
                case UITextCharFilter::Decimal:
                    if ( cp == '-' )
                        ok = atStart && accepted.empty() && !restMinus;
                    else if ( cp == '.' && rules.Filter == UITextCharFilter::Decimal )
                        ok = !hasPoint;
                    else
                        ok = IsDigit( cp );
                    break;
                case UITextCharFilter::Alphanumeric:
                    ok = cp >= 0x80u || IsDigit( cp ) || ( cp >= 'a' && cp <= 'z' ) || ( cp >= 'A' && cp <= 'Z' );
                    break;
            }
            if ( !ok )
                continue;
            if ( cp == '.' )
                hasPoint = true;
            Text::Utf8Append( accepted, cp );
            ++count;
        }
        if ( accepted.empty() )
            return false;

        PushUndo( text );
        EraseSelection( text );
        text.insert( m_Caret, accepted );
        m_Caret += accepted.size();
        m_Anchor = m_Caret;
        return true;
    }

    bool UITextEditState::Backspace( std::string& text, bool word )
    {
        Sync( text );
        if ( HasSelection() )
        {
            PushUndo( text );
            return EraseSelection( text );
        }
        if ( m_Caret == 0 )
            return false;
        const std::size_t from = word ? WordLeftOf( text, m_Caret ) : PrevBoundary( text, m_Caret );
        PushUndo( text );
        text.erase( from, m_Caret - from );
        m_Caret = m_Anchor = from;
        return true;
    }

    bool UITextEditState::Delete( std::string& text, bool word )
    {
        Sync( text );
        if ( HasSelection() )
        {
            PushUndo( text );
            return EraseSelection( text );
        }
        if ( m_Caret >= text.size() )
            return false;
        const std::size_t to = word ? WordRightOf( text, m_Caret ) : NextBoundary( text, m_Caret );
        PushUndo( text );
        text.erase( m_Caret, to - m_Caret );
        m_Anchor = m_Caret;
        return true;
    }

    bool UITextEditState::Copy( const std::string& text, const UITextEditRules& rules,
                                IUIClipboard& clipboard ) const
    {
        if ( rules.Password || !HasSelection() )
            return false;
        clipboard.SetText( SelectedText( text ) );
        return true;
    }

    bool UITextEditState::Cut( std::string& text, const UITextEditRules& rules, IUIClipboard& clipboard )
    {
        Sync( text );
        if ( !Copy( text, rules, clipboard ) )
            return false;
        PushUndo( text );
        return EraseSelection( text );
    }

    bool UITextEditState::Paste( std::string& text, const UITextEditRules& rules, const IUIClipboard& clipboard )
    {
        std::string pasted = NormalizeBreaks( clipboard.GetText() );
        // A single-line field takes a multi-line paste as one line (UE SEditableText strips the breaks);
        // a space keeps the words apart.
        if ( !rules.MultiLine )
            std::replace( pasted.begin(), pasted.end(), '\n', ' ' );
        return Insert( text, pasted, rules );
    }

    bool UITextEditState::Undo( std::string& text )
    {
        if ( m_Undo.empty() )
            return false;
        m_Redo.push_back( { text, m_Caret, m_Anchor } );
        Snapshot s = std::move( m_Undo.back() );
        m_Undo.pop_back();
        text     = std::move( s.Text );
        m_Caret  = s.Caret;
        m_Anchor = s.Anchor;
        Sync( text );
        return true;
    }

    bool UITextEditState::Redo( std::string& text )
    {
        if ( m_Redo.empty() )
            return false;
        m_Undo.push_back( { text, m_Caret, m_Anchor } );
        Snapshot s = std::move( m_Redo.back() );
        m_Redo.pop_back();
        text     = std::move( s.Text );
        m_Caret  = s.Caret;
        m_Anchor = s.Anchor;
        Sync( text );
        return true;
    }

    UITextEditOutcome UITextEditState::Apply( std::string& text, const UITextEditRules& rules,
                                              std::string_view typed, std::span<const UIKeyEvent> keys,
                                              IUIClipboard* clipboard )
    {
        Sync( text );
        const std::string before = text;
        UITextEditOutcome out;
        if ( !typed.empty() )
            Insert( text, typed, rules );

        using Common::KeyCode;
        for ( const UIKeyEvent& k : keys )
        {
            const bool shift = HasMod( k.Mods, UIKeyMods::Shift );
            const bool super = HasMod( k.Mods, UIKeyMods::Super );
            const bool ctrl  = HasMod( k.Mods, UIKeyMods::Ctrl );
            const bool alt   = HasMod( k.Mods, UIKeyMods::Alt );
            // The shortcut modifier is Cmd on macOS and Ctrl elsewhere; both are honoured on both, as a
            // game field cannot know which keyboard the player learned on. Word jumps: Alt (macOS) or Ctrl.
            const bool command = ctrl || super;
            const bool word    = alt || ctrl;
            switch ( k.Key )
            {
                case KeyCode::Left:
                    Move( text,
                          super  ? UITextMove::LineStart
                          : word ? UITextMove::WordLeft
                                 : UITextMove::Left,
                          shift );
                    break;
                case KeyCode::Right:
                    Move( text,
                          super  ? UITextMove::LineEnd
                          : word ? UITextMove::WordRight
                                 : UITextMove::Right,
                          shift );
                    break;
                case KeyCode::Up:
                    if ( rules.MultiLine )
                        Move( text, super ? UITextMove::Start : UITextMove::Up, shift );
                    break;
                case KeyCode::Down:
                    if ( rules.MultiLine )
                        Move( text, super ? UITextMove::End : UITextMove::Down, shift );
                    break;
                case KeyCode::Home:
                    Move( text, command ? UITextMove::Start : UITextMove::LineStart, shift );
                    break;
                case KeyCode::End:
                    Move( text, command ? UITextMove::End : UITextMove::LineEnd, shift );
                    break;
                case KeyCode::Backspace:
                    Backspace( text, word );
                    break;
                case KeyCode::Delete:
                    Delete( text, word );
                    break;
                case KeyCode::A:
                    if ( command )
                        SelectAll( text );
                    break;
                case KeyCode::C:
                    if ( command && clipboard )
                        Copy( text, rules, *clipboard );
                    break;
                case KeyCode::X:
                    if ( command && clipboard )
                        Cut( text, rules, *clipboard );
                    break;
                case KeyCode::V:
                    if ( command && clipboard )
                        Paste( text, rules, *clipboard );
                    break;
                case KeyCode::Z:
                    if ( command )
                        shift ? Redo( text ) : Undo( text );
                    break;
                case KeyCode::Y:
                    if ( command )
                        Redo( text );
                    break;
                case KeyCode::Enter:
                case KeyCode::KPEnter:
                {
                    const bool breaksLine =
                         rules.MultiLine && ( rules.NewLine == UITextNewLineKey::Enter || shift );
                    if ( breaksLine )
                        Insert( text, "\n", rules );
                    else
                        out.Committed = true;
                    break;
                }
                default:
                    break;
            }
        }
        out.Changed = text != before;
        return out;
    }

    std::string UITextDisplayString( const std::string& text, bool password )
    {
        if ( !password )
            return text;
        std::string       out;
        const std::size_t n = CountCodepoints( text, 0, text.size() );
        out.reserve( n * 3 );
        for ( std::size_t i = 0; i < n; ++i )
            out += "\xE2\x80\xA2"; // U+2022 BULLET, the glyph UE's password run draws
        return out;
    }

    std::size_t UITextDisplayOffset( const std::string& text, std::size_t offset, bool password )
    {
        offset = std::min( offset, text.size() );
        return password ? CountCodepoints( text, 0, offset ) * 3 : offset;
    }

    std::size_t UITextLineOf( const std::string& text, std::size_t offset )
    {
        offset = std::min( offset, text.size() );
        return static_cast<std::size_t>(
             std::count( text.begin(), text.begin() + static_cast<std::ptrdiff_t>( offset ), '\n' ) );
    }

    std::size_t UITextLineStart( const std::string& text, std::size_t offset )
    {
        offset = std::min( offset, text.size() );
        if ( offset == 0 )
            return 0;
        const std::size_t nl = text.rfind( '\n', offset - 1 );
        return nl == std::string::npos ? 0 : nl + 1;
    }
} // namespace Desert::UI
