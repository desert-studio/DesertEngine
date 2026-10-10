// UITextEdit — the edit-state invariants of an input field (UI/UITextEditState.hpp), decided from the string
// alone: the caret never leaves a codepoint boundary, an edit and its undo are a round trip, the rules
// (password, max length, filter, single-line) refuse what they say they refuse, and the keys map to the
// operations UE's FSlateEditableTextLayout maps them to.

#include <Engine/UI/Ecs/UICanvasRendererEcs.hpp>
#include <UI/UITextEditState.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace Desert::UI;
using Common::KeyCode;

namespace
{
    struct FakeClipboard final : IUIClipboard
    {
        std::string Text;
        std::string GetText() const override
        {
            return Text;
        }
        void SetText( const std::string& text ) override
        {
            Text = text;
        }
    };

    UIKeyEvent Key( KeyCode k, UIKeyMods m = UIKeyMods::None )
    {
        return { k, m, false };
    }

    bool OnBoundary( const std::string& s, std::size_t i )
    {
        return i == s.size() || ( i < s.size() && ( static_cast<unsigned char>( s[i] ) & 0xC0u ) != 0x80u );
    }
} // namespace

TEST( UITextEdit, InsertAtCaretReplacesSelectionAndUndoRoundTrips )
{
    UITextEditState st;
    std::string     text = "hello world";
    st.SetCaret( text, 5, false );
    EXPECT_TRUE( st.Insert( text, ",", {} ) );
    EXPECT_EQ( text, "hello, world" );
    EXPECT_EQ( st.Caret(), 6u );

    st.SelectWordAt( text, 8 ); // "world"
    EXPECT_EQ( st.SelectedText( text ), "world" );
    EXPECT_TRUE( st.Insert( text, "there", {} ) );
    EXPECT_EQ( text, "hello, there" );

    EXPECT_TRUE( st.Undo( text ) );
    EXPECT_EQ( text, "hello, world" );
    EXPECT_EQ( st.SelectedText( text ), "world" ); // the selection comes back with the text
    EXPECT_TRUE( st.Undo( text ) );
    EXPECT_EQ( text, "hello world" );
    EXPECT_FALSE( st.Undo( text ) );
    EXPECT_TRUE( st.Redo( text ) );
    EXPECT_TRUE( st.Redo( text ) );
    EXPECT_EQ( text, "hello, there" );
}

TEST( UITextEdit, CaretStaysOnCodepointBoundaries )
{
    UITextEditState st;
    std::string     text = "Привет мир"; // two-byte codepoints
    st.SetCaret( text, 3, false );       // inside "р"
    EXPECT_TRUE( OnBoundary( text, st.Caret() ) );
    for ( int i = 0; i < 20; ++i )
    {
        st.Move( text, UITextMove::Right, false );
        EXPECT_TRUE( OnBoundary( text, st.Caret() ) );
    }
    EXPECT_EQ( st.Caret(), text.size() );
    EXPECT_TRUE( st.Backspace( text, false ) );
    EXPECT_EQ( text, "Привет ми" );
    st.Move( text, UITextMove::WordLeft, false );
    EXPECT_EQ( text.substr( st.Caret() ), "ми" );

    // An outside write shortens the text: the caret is pulled back inside, onto a boundary.
    text = "При";
    st.Sync( text );
    EXPECT_LE( st.Caret(), text.size() );
    EXPECT_TRUE( OnBoundary( text, st.Caret() ) );
}

TEST( UITextEdit, WordJumpsAndWordDeletes )
{
    UITextEditState st;
    std::string     text = "one two, three";
    st.SetCaret( text, 0, false );
    st.Move( text, UITextMove::WordRight, false );
    EXPECT_EQ( st.Caret(), 4u );
    st.Move( text, UITextMove::End, false );
    EXPECT_TRUE( st.Backspace( text, true ) );
    EXPECT_EQ( text, "one two, " );
    st.SetCaret( text, 0, false );
    EXPECT_TRUE( st.Delete( text, true ) );
    EXPECT_EQ( text, "two, " );
}

TEST( UITextEdit, ShiftExtendsAndArrowCollapsesSelection )
{
    UITextEditState st;
    std::string     text = "abcdef";
    st.SetCaret( text, 1, false );
    st.Move( text, UITextMove::Right, true );
    st.Move( text, UITextMove::Right, true );
    EXPECT_EQ( st.SelectedText( text ), "bc" );
    st.Move( text, UITextMove::Left, false ); // collapses to the low edge, does not step
    EXPECT_FALSE( st.HasSelection() );
    EXPECT_EQ( st.Caret(), 1u );
}

TEST( UITextEdit, RulesRefuseWhatTheySay )
{
    UITextEditState st;
    std::string     text;
    UITextEditRules r;
    r.MaxLength = 3;
    EXPECT_TRUE( st.Insert( text, "abcdef", r ) );
    EXPECT_EQ( text, "abc" );
    EXPECT_FALSE( st.Insert( text, "x", r ) ); // full: no change, no undo step
    st.Undo( text );
    EXPECT_EQ( text, "" );

    UITextEditRules num;
    num.Filter = UITextCharFilter::Decimal;
    text.clear();
    UITextEditState n;
    n.Insert( text, "-1a2.3.4-", num );
    EXPECT_EQ( text, "-12.34" );

    UITextEditRules single; // single-line: a typed break is dropped, a pasted one becomes a space
    text.clear();
    UITextEditState s;
    s.Insert( text, "a\nb\x01", single );
    EXPECT_EQ( text, "ab" );
    FakeClipboard cb;
    cb.Text = "x\r\ny";
    s.Paste( text, single, cb );
    EXPECT_EQ( text, "abx y" );

    UITextEditRules multi;
    multi.MultiLine = true;
    text.clear();
    UITextEditState m;
    m.Paste( text, multi, cb );
    EXPECT_EQ( text, "x\ny" );
}

TEST( UITextEdit, PasswordRefusesCopyAndCutAndDrawsBullets )
{
    UITextEditState st;
    std::string     text = "secret";
    UITextEditRules r;
    r.Password = true;
    FakeClipboard cb;
    st.SelectAll( text );
    EXPECT_FALSE( st.Copy( text, r, cb ) );
    EXPECT_FALSE( st.Cut( text, r, cb ) );
    EXPECT_EQ( text, "secret" );
    EXPECT_TRUE( cb.Text.empty() );
    EXPECT_EQ( UITextDisplayString( "ая", true ), "\xE2\x80\xA2\xE2\x80\xA2" );
    EXPECT_EQ( UITextDisplayOffset( "ая", 2, true ), 3u );
}

TEST( UITextEdit, KeysMapToOperations )
{
    UITextEditState   st;
    std::string       text;
    FakeClipboard     cb;
    UITextEditRules   r;
    UITextEditOutcome o = st.Apply( text, r, "hello", {}, &cb );
    EXPECT_TRUE( o.Changed );
    EXPECT_FALSE( o.Committed );

    std::vector<UIKeyEvent> keys = { Key( KeyCode::A, UIKeyMods::Super ), Key( KeyCode::C, UIKeyMods::Ctrl ) };
    o                            = st.Apply( text, r, "", keys, &cb );
    EXPECT_FALSE( o.Changed );
    EXPECT_EQ( cb.Text, "hello" );

    keys = { Key( KeyCode::End ), Key( KeyCode::V, UIKeyMods::Ctrl ), Key( KeyCode::Enter ) };
    o    = st.Apply( text, r, "", keys, &cb );
    EXPECT_EQ( text, "hellohello" );
    EXPECT_TRUE( o.Changed );
    EXPECT_TRUE( o.Committed );

    keys = { Key( KeyCode::Z, UIKeyMods::Ctrl ) };
    st.Apply( text, r, "", keys, &cb );
    EXPECT_EQ( text, "hello" );
    keys = { Key( KeyCode::Z, UIKeyMods::Ctrl | UIKeyMods::Shift ) };
    st.Apply( text, r, "", keys, &cb );
    EXPECT_EQ( text, "hellohello" );

    // No clipboard: the shortcuts do nothing, and nothing crashes.
    keys = { Key( KeyCode::A, UIKeyMods::Ctrl ), Key( KeyCode::X, UIKeyMods::Ctrl ) };
    o    = st.Apply( text, r, "", keys, nullptr );
    EXPECT_FALSE( o.Changed );
}

TEST( UITextEdit, MultiLineEnterBreaksOrCommitsByNewLineKey )
{
    UITextEditRules chat;
    chat.MultiLine = true; // ShiftEnter: Shift+Enter breaks, Enter commits
    UITextEditState st;
    std::string     text = "a";
    st.SetCaret( text, 1, false );
    std::vector<UIKeyEvent> keys = { Key( KeyCode::Enter, UIKeyMods::Shift ) };
    UITextEditOutcome       o    = st.Apply( text, chat, "", keys, nullptr );
    EXPECT_EQ( text, "a\n" );
    EXPECT_FALSE( o.Committed );
    keys = { Key( KeyCode::Enter ) };
    o    = st.Apply( text, chat, "b", keys, nullptr );
    EXPECT_EQ( text, "a\nb" );
    EXPECT_TRUE( o.Committed );

    // Up keeps the column on the previous line; Down returns.
    text = "abcd\nxy";
    st.SetCaret( text, text.size(), false ); // after "y", column 2
    st.Move( text, UITextMove::Up, false );
    EXPECT_EQ( st.Caret(), 2u );
    st.Move( text, UITextMove::Down, false );
    EXPECT_EQ( st.Caret(), text.size() );
    EXPECT_EQ( UITextLineOf( text, text.size() ), 1u );
    EXPECT_EQ( UITextLineStart( text, text.size() ), 5u );
}
