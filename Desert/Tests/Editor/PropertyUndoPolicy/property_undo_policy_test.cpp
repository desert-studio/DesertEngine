// WHICH PROPERTY EDITS CAN BE TAKEN BACK — and the two opposite ways one expression got it wrong.
//
// Until this suite existed the answer lived inside PropertyEditorBuilder::DrawField, a function that
// draws ImGui and is therefore compiled by no suite in the repository. It read:
//
//     trackUndo = !ReadOnly && Type != AssetHandle && Type != Struct && Size > 0 && Size <= 64
//
// and it was wrong in both directions at once:
//
//   * ASSET SLOTS WERE EXCLUDED. Assigning a sprite to a UI canvas, a font to a label, an icon or a
//     video was irreversible — Ctrl+Z skipped the edit entirely and went back to whatever was changed
//     before it. Ю2's brief calls this "any sprite assignment in the UI is irreversible", and it was.
//   * EVERY std::string WAS INCLUDED, and the entry those pushed is a byte copy of the field
//     (CommandHistory::ByteCommand). Undoing a text edit memcpy'd the raw bytes of a std::string —
//     its heap pointer among them — over a live string whose buffer the edit had already freed.
//     Seventeen reflected string fields could reach it, `UIText::Text` among them.
//
// The decision now lives in Editor/Panels/PropertyEditor/PropertyUndoPolicy.hpp as two pure functions,
// which is what makes it assertable at all. Nothing from the editor or the engine is linked here: the
// policy is a header of constexpr functions over one enum.

#include <gtest/gtest.h>

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Panels/PropertyEditor/PropertyUndoPolicy.hpp>

#include <array>
#include <string>

using Desert::Editor::PropertyUndoKind;
using Desert::Editor::PropertyUndoKindFor;
using Desert::Editor::PropertyUndoStorage;
using Desert::Editor::PropertyUndoStorageFor;
using Desert::Reflection::FieldType;

namespace
{
    // Every FieldType the reflection layer defines. Written out rather than derived from a count, so
    // adding an enumerator makes THIS list fail to compile against the switch it feeds rather than
    // quietly shrinking the census — the same reason ComponentReflection pins rows and not a number.
    constexpr std::array<FieldType, 13> kAllFieldTypes = {
         FieldType::Unknown, FieldType::Bool,   FieldType::Int,         FieldType::UInt, FieldType::Float,
         FieldType::Double,  FieldType::String, FieldType::Vec2,        FieldType::Vec3, FieldType::Vec4,
         FieldType::Enum,    FieldType::Struct, FieldType::AssetHandle,
    };

    const char* Name( FieldType t )
    {
        switch ( t )
        {
            case FieldType::Unknown:
                return "Unknown";
            case FieldType::Bool:
                return "Bool";
            case FieldType::Int:
                return "Int";
            case FieldType::UInt:
                return "UInt";
            case FieldType::Float:
                return "Float";
            case FieldType::Double:
                return "Double";
            case FieldType::String:
                return "String";
            case FieldType::Vec2:
                return "Vec2";
            case FieldType::Vec3:
                return "Vec3";
            case FieldType::Vec4:
                return "Vec4";
            case FieldType::Enum:
                return "Enum";
            case FieldType::Struct:
                return "Struct";
            case FieldType::AssetHandle:
                return "AssetHandle";
        }
        return "?";
    }

    // A plausible sizeof for each type, so the size guard is never what decides an answer below.
    std::size_t SizeOf( FieldType t )
    {
        switch ( t )
        {
            case FieldType::Bool:
                return sizeof( bool );
            case FieldType::Int:
                return sizeof( int32_t );
            case FieldType::UInt:
                return sizeof( uint32_t );
            case FieldType::Float:
                return sizeof( float );
            case FieldType::Double:
                return sizeof( double );
            case FieldType::String:
                return sizeof( std::string );
            case FieldType::Vec2:
                return 8;
            case FieldType::Vec3:
                return 12;
            case FieldType::Vec4:
                return 16;
            case FieldType::Enum:
                return sizeof( int32_t );
            case FieldType::Struct:
                return 64;
            case FieldType::AssetHandle:
                return sizeof( uint64_t );
            case FieldType::Unknown:
                return 4;
        }
        return 4;
    }
} // namespace

// --- The defect this task is about ---------------------------------------------------------------
//
// An asset handle is a value like any other and the user changes it with one click. It has to be on
// the stack, and it has to arrive there by the DISCRETE route: the pickers commit from a Selectable
// inside a popup, from a drag-and-drop accept or from a Clear button, and none of those is the field's
// own ImGui item — so the activate/deactivate pair the sliders use never fires for them. Asking for
// Interactive here would compile, would look like a fix, and would record nothing.
TEST( PropertyUndoPolicy, AnAssetHandleIsRecordedAndIsCommittedDiscretely )
{
    EXPECT_EQ( PropertyUndoKindFor( FieldType::AssetHandle, false, sizeof( uint64_t ) ),
               PropertyUndoKind::Discrete )
         << "an asset slot edit is not on the undo stack: assigning a sprite/font/icon/video is "
            "irreversible, which is the defect Ю2 came from";
    EXPECT_EQ( PropertyUndoStorageFor( FieldType::AssetHandle, false, sizeof( uint64_t ) ),
               PropertyUndoStorage::Bytes )
         << "a handle is a trivially copyable 64-bit value; bytes are exactly what restores it";
}

// The enum combo commits the same way and had the same silent gap: BeginCombo/EndCombo never marks the
// combo's own item as edited, so IsItemDeactivatedAfterEdit does not fire for it either. It was inside
// the old condition and therefore LOOKED covered.
TEST( PropertyUndoPolicy, AnEnumIsRecordedAndIsCommittedDiscretely )
{
    EXPECT_EQ( PropertyUndoKindFor( FieldType::Enum, false, sizeof( int32_t ) ), PropertyUndoKind::Discrete );
}

// --- The defect found next to it ------------------------------------------------------------------
//
// The value of a std::string and the bytes of a std::string are different things, and only one of them
// can be stored and put back. A byte entry for a string is not a weaker undo, it is a use-after-free.
TEST( PropertyUndoPolicy, AStringIsRecordedByValueAndNeverByItsBytes )
{
    EXPECT_EQ( PropertyUndoKindFor( FieldType::String, false, sizeof( std::string ) ),
               PropertyUndoKind::Interactive )
         << "a text edit must still be undoable — the fix is the storage, not dropping the entry";
    EXPECT_EQ( PropertyUndoStorageFor( FieldType::String, false, sizeof( std::string ) ),
               PropertyUndoStorage::StringValue )
         << "restoring a string's BYTES hands the live object a heap pointer the edit already freed";
}

// The property the two enums exist to keep: a field is either recorded WITH something to restore it
// from, or not recorded at all. "Recorded with nothing" is what a byte entry over a string was.
TEST( PropertyUndoPolicy, RecordingAndStorageAgreeForEveryFieldType )
{
    for ( const FieldType t : kAllFieldTypes )
    {
        const bool recorded = PropertyUndoKindFor( t, false, SizeOf( t ) ) != PropertyUndoKind::None;
        const bool stored   = PropertyUndoStorageFor( t, false, SizeOf( t ) ) != PropertyUndoStorage::None;
        EXPECT_EQ( recorded, stored ) << Name( t ) << " is recorded on one side and not the other";
    }
}

// Only String may take the value route. Anything else asking for it would be a field whose bytes are
// not its value — which is a type the byte entry must not be given either, and would need its own row.
TEST( PropertyUndoPolicy, OnlyAStringUsesValueStorage )
{
    for ( const FieldType t : kAllFieldTypes )
    {
        if ( t == FieldType::String )
            continue;
        EXPECT_NE( PropertyUndoStorageFor( t, false, SizeOf( t ) ), PropertyUndoStorage::StringValue )
             << Name( t ) << " claims string storage";
    }
}

// The two rows that are deliberately NOT recorded, each for a reason that is not "we forgot".
TEST( PropertyUndoPolicy, AStructRowAndAnUnknownFieldRecordNothing )
{
    EXPECT_EQ( PropertyUndoKindFor( FieldType::Struct, false, 64 ), PropertyUndoKind::None )
         << "a struct row draws no widget; its leaves record themselves and would be doubled";
    EXPECT_EQ( PropertyUndoKindFor( FieldType::Unknown, false, 4 ), PropertyUndoKind::None )
         << "the editor draws '(unsupported)' for it and no value can change";
}

// The guards that are about the FIELD rather than its type.
TEST( PropertyUndoPolicy, AReadOnlyOrUnsizedOrOversizedFieldRecordsNothing )
{
    EXPECT_EQ( PropertyUndoKindFor( FieldType::Float, true, sizeof( float ) ), PropertyUndoKind::None )
         << "a ReadOnly row cannot be edited, so there is nothing to take back";
    EXPECT_EQ( PropertyUndoKindFor( FieldType::AssetHandle, true, sizeof( uint64_t ) ), PropertyUndoKind::None )
         << "and that holds for the asset slots this task added, not only for the old set";
    EXPECT_EQ( PropertyUndoKindFor( FieldType::Float, false, 0 ), PropertyUndoKind::None );
    EXPECT_EQ( PropertyUndoKindFor( FieldType::Vec4, false, 65 ), PropertyUndoKind::None )
         << "past the byte entry's ceiling, which is 64";
}

// The types that keep the pair they always had. A regression here means a slider stopped being
// undoable, which is the direction nobody would notice from the asset-slot fix alone.
TEST( PropertyUndoPolicy, TheHeldWidgetsAreStillInteractive )
{
    for ( const FieldType t : { FieldType::Bool, FieldType::Int, FieldType::UInt, FieldType::Float,
                                FieldType::Double, FieldType::Vec2, FieldType::Vec3, FieldType::Vec4 } )
    {
        EXPECT_EQ( PropertyUndoKindFor( t, false, SizeOf( t ) ), PropertyUndoKind::Interactive )
             << Name( t ) << " lost its undo entry";
        EXPECT_EQ( PropertyUndoStorageFor( t, false, SizeOf( t ) ), PropertyUndoStorage::Bytes ) << Name( t );
    }
}

// The census. A FieldType added tomorrow gets an answer here or this fails — the same shape as the
// component censuses next door, and the reason is the same: the moment the type is added is the only
// moment anybody is thinking about it.
TEST( PropertyUndoPolicy, EveryFieldTypeHasAnAnswerAndTheListIsComplete )
{
    ASSERT_EQ( kAllFieldTypes.size(), static_cast<std::size_t>( FieldType::AssetHandle ) + 1u )
         << "FieldType gained or lost an enumerator and this census did not follow";

    for ( std::size_t i = 0; i < kAllFieldTypes.size(); ++i )
        EXPECT_EQ( static_cast<std::size_t>( kAllFieldTypes[i] ), i )
             << "kAllFieldTypes is out of order at " << i
             << ", so a type is listed twice and another not "
                "at all";
}

// =================================================================================================
// THE COMMIT DECISION — what the row actually does at the end of a frame.
//
// The block above is about which fields are eligible. This one is about the moment: an asset slot was
// eligible for nothing, and giving it the eligibility without the moment would still record nothing,
// because the activate/deactivate pair the sliders use never fires for a value written from inside a
// popup. That is the mutation that "looks like a fix", so it is asserted separately.
// =================================================================================================

using Desert::Editor::PropertyEditSignals;
using Desert::Editor::PropertyUndoAction;
using Desert::Editor::PropertyUndoActionFor;

namespace
{
    PropertyEditSignals Signals( bool deactivatedAfterEdit, bool changed, bool valueDiffers )
    {
        PropertyEditSignals s;
        s.DeactivatedAfterEdit = deactivatedAfterEdit;
        s.Changed              = changed;
        s.ValueDiffers         = valueDiffers;
        return s;
    }
} // namespace

// The defect, stated as the moment it happens. A picker writes the handle and reports `changed` on
// the same frame; nothing deactivates, ever.
TEST( PropertyUndoPolicy, ADiscreteEditIsRecordedOnTheFrameItChangesTheValue )
{
    EXPECT_EQ( PropertyUndoActionFor( PropertyUndoKind::Discrete, PropertyUndoStorage::Bytes,
                                      Signals( /*deactivated*/ false, /*changed*/ true, /*differs*/ true ) ),
               PropertyUndoAction::PushBytes )
         << "an asset pick recorded nothing, which is what made it irreversible";

    EXPECT_EQ( PropertyUndoActionFor( PropertyUndoKind::Discrete, PropertyUndoStorage::Bytes,
                                      Signals( false, /*changed*/ false, true ) ),
               PropertyUndoAction::Nothing )
         << "a frame in which nothing was picked put a step on the stack";

    EXPECT_EQ( PropertyUndoActionFor( PropertyUndoKind::Discrete, PropertyUndoStorage::Bytes,
                                      Signals( false, true, /*differs*/ false ) ),
               PropertyUndoAction::Nothing )
         << "re-picking the value that was already there is not an edit and must not push a no-op step";
}

// The other direction, and the one a careless fix breaks: a held widget must NOT record per frame.
TEST( PropertyUndoPolicy, AnInteractiveEditIsRecordedOnceAtTheEndAndNotPerFrame )
{
    EXPECT_EQ( PropertyUndoActionFor( PropertyUndoKind::Interactive, PropertyUndoStorage::Bytes,
                                      Signals( /*deactivated*/ false, /*changed*/ true, true ) ),
               PropertyUndoAction::Nothing )
         << "a drag would push one entry per frame and bury the stack";

    EXPECT_EQ( PropertyUndoActionFor( PropertyUndoKind::Interactive, PropertyUndoStorage::Bytes,
                                      Signals( /*deactivated*/ true, false, false ) ),
               PropertyUndoAction::PushBytes )
         << "the commit of a held widget is its deactivation, and ImGui is the authority on whether it "
            "was edited";
}

// A string never takes the byte route, at either moment.
TEST( PropertyUndoPolicy, AStringCommitPushesItsValue )
{
    EXPECT_EQ( PropertyUndoActionFor( PropertyUndoKind::Interactive, PropertyUndoStorage::StringValue,
                                      Signals( true, true, true ) ),
               PropertyUndoAction::PushString );
}

TEST( PropertyUndoPolicy, AFieldThatIsNotRecordedPushesNothingWhateverTheWidgetReports )
{
    for ( const bool deactivated : { false, true } )
        for ( const bool changed : { false, true } )
            EXPECT_EQ( PropertyUndoActionFor( PropertyUndoKind::None, PropertyUndoStorage::None,
                                              Signals( deactivated, changed, true ) ),
                       PropertyUndoAction::Nothing );
}

// =================================================================================================
// AND THAT THE ENTRIES THEMSELVES RESTORE WHAT THEY CLAIM TO.
// =================================================================================================

// The whole point, at the level of the stack: a handle assignment can be taken back.
TEST( PropertyUndoPolicy, AnAssetHandleEntryRestoresThePreviousHandle )
{
    Desert::Editor::CommandHistory& history = Desert::Editor::CommandHistory::Get();
    history.Clear();

    constexpr uint64_t kOld = 5355760296319878840ull; // above 2^53, like every path-derived handle
    constexpr uint64_t kNew = 1234567890123456789ull;

    uint64_t       slot   = kOld;
    const uint64_t before = slot;
    slot                  = kNew;
    history.Push( &slot, &before, &slot, sizeof( slot ) );

    ASSERT_TRUE( history.Undo() );
    EXPECT_EQ( slot, kOld ) << "undo did not bring the previous reference back";
    ASSERT_TRUE( history.Redo() );
    EXPECT_EQ( slot, kNew );
}

// The string entry restores the VALUE. The byte entry this replaces would have restored a pointer the
// allocator had already reclaimed, so the assertion that matters is that a long (heap-allocated)
// string survives the trip — a short one would sit in the small-string buffer and hide the defect.
TEST( PropertyUndoPolicy, AStringEntryRestoresTheValueIncludingHeapAllocatedOnes )
{
    Desert::Editor::CommandHistory& history = Desert::Editor::CommandHistory::Get();
    history.Clear();

    const std::string longOld( 512, 'a' );
    const std::string longNew( 512, 'b' );

    std::string       field  = longOld;
    const std::string before = field;
    field                    = longNew;
    history.PushString( &field, before, field );

    ASSERT_TRUE( history.Undo() );
    EXPECT_EQ( field, longOld );
    ASSERT_TRUE( history.Redo() );
    EXPECT_EQ( field, longNew );
}
