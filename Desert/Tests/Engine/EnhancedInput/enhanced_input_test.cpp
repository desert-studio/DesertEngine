// GP1a: Enhanced Input as in UE — the Input Action and Input Mapping Context assets, the modifier stack, the
// triggers and the context stack. What must hold: both assets read back as written with a GUID that survives
// a re-save, and a file the subsystem could not evaluate is refused by name; modifiers apply IN ORDER with UE's
// math; over a scripted sequence of frames each trigger fires its events on exactly UE's frames; and a key a
// higher-priority context maps is consumed before a lower context sees it.

#include <Engine/Assets/Serialization/InputAssets.hpp>
#include <Engine/Input/EnhancedInputSubsystem.hpp>
#include <Engine/Input/LocalPlayerInput.hpp>
#include <Engine/Input/UserKeyBindings.hpp>
#include <Engine/ECS/Components.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <Common/Content/ContentKinds.hpp>
#include <Common/Content/TextAssetHeader.hpp>

#include <gtest/gtest.h>

#include <algorithm>

#include <string>
#include <vector>
#include "../../TestSupport/runner.hpp"

using namespace Desert::Assets::Serialization;
using Desert::Input::EnhancedInputSubsystem;
using Desert::Input::RawInputFrame;
using Desert::Input::TriggerEvents;
using Desert::Input::TriggerState;

namespace
{
    Common::Content::AssetGuid Guid( uint64_t n )
    {
        return Common::Content::AssetGuid{ 0x6900ull + n, 0x1ull };
    }

    std::string GuidText( uint64_t n )
    {
        return Common::Content::AssetGuidToText( Guid( n ) );
    }

    InputModifierData Negate( bool x, bool y, bool z )
    {
        InputModifierData m;
        m.Type   = InputModifierType::Negate;
        m.Negate = InputNegateParams{ x, y, z };
        return m;
    }

    InputModifierData Swizzle( InputSwizzleOrder order )
    {
        InputModifierData m;
        m.Type    = InputModifierType::Swizzle;
        m.Swizzle = InputSwizzleParams{ order };
        return m;
    }

    InputModifierData DeadZone( InputDeadZoneType type, float lower, float upper )
    {
        InputModifierData m;
        m.Type     = InputModifierType::DeadZone;
        m.DeadZone = InputDeadZoneParams{ type, lower, upper };
        return m;
    }

    InputModifierData Scalar( glm::vec3 s )
    {
        InputModifierData m;
        m.Type   = InputModifierType::Scalar;
        m.Scalar = InputScalarParams{ s };
        return m;
    }

    InputTriggerData Trigger( InputTriggerType type )
    {
        InputTriggerData t;
        t.Type = type;
        return t;
    }

    InputTriggerData Hold( float seconds, bool oneShot )
    {
        InputTriggerData t;
        t.Type = InputTriggerType::Hold;
        t.Hold = InputHoldParams{ seconds, oneShot };
        return t;
    }

    InputKeyMappingData Mapping( uint64_t action, const std::string& key, std::vector<InputModifierData> mods = {},
                                 std::vector<InputTriggerData> triggers = {} )
    {
        return InputKeyMappingData{
             { GuidText( action ), "Input/IA_" + std::to_string( action ) + ".deinputaction" },
             key,
             std::move( mods ),
             std::move( triggers ) };
    }

    // A context as the subsystem gets it from disk: written (header stamped, a GUID minted) and read back.
    InputMappingContextData Loaded( std::vector<InputKeyMappingData> mappings )
    {
        InputMappingContextData data;
        data.Mappings     = std::move( mappings );
        const auto parsed = ParseInputMappingContext( WriteInputMappingContext( data ) );
        EXPECT_TRUE( parsed ) << ( parsed ? "" : parsed.GetError() );
        return parsed ? parsed.GetValue() : InputMappingContextData{};
    }

    Common::Content::AssetGuid ContextGuid( const InputMappingContextData& context )
    {
        const auto guid = Common::Content::AssetGuidFromText( context.Header->Guid );
        EXPECT_TRUE( guid ) << ( guid ? "" : guid.GetError() );
        return guid ? guid.GetValue() : Common::Content::AssetGuid{};
    }

    RawInputFrame Keys( std::vector<Common::KeyCode> down )
    {
        RawInputFrame frame;
        frame.KeysDown = std::move( down );
        return frame;
    }

    std::string Mutated( std::string text, const std::string& from, const std::string& to )
    {
        const auto at = text.find( from );
        EXPECT_NE( at, std::string::npos ) << "'" << from << "' is not in the written text";
        if ( at != std::string::npos )
            text.replace( at, from.size(), to );
        return text;
    }

    void ExpectVec( const glm::vec3& got, const glm::vec3& want )
    {
        EXPECT_NEAR( got.x, want.x, 1e-5f );
        EXPECT_NEAR( got.y, want.y, 1e-5f );
        EXPECT_NEAR( got.z, want.z, 1e-5f );
    }
} // namespace

// ---- Assets ----

TEST( EnhancedInputAssets, ActionRoundTripKeepsEveryFieldAndItsGuid )
{
    InputActionData action;
    action.ValueType    = InputValueType::Axis2D; // both off their defaults
    action.ConsumeInput = false;

    const auto first = ParseInputAction( WriteInputAction( action ) );
    ASSERT_TRUE( first ) << first.GetError();
    EXPECT_EQ( first.GetValue().ValueType, InputValueType::Axis2D );
    EXPECT_FALSE( first.GetValue().ConsumeInput );
    ASSERT_TRUE( first.GetValue().Header );
    EXPECT_EQ( first.GetValue().Header->Kind, "InputAction" );

    const auto second = ParseInputAction( WriteInputAction( first.GetValue() ) );
    ASSERT_TRUE( second ) << second.GetError();
    EXPECT_EQ( second.GetValue(), first.GetValue() ); // the re-save keeps the GUID
}

TEST( EnhancedInputAssets, ContextRoundTripKeepsMappingsModifiersTriggersAndStatesItsActions )
{
    InputMappingContextData context;
    context.Mappings = {
         Mapping( 1, "W", { Swizzle( InputSwizzleOrder::ZXY ) }, { Trigger( InputTriggerType::Pressed ) } ),
         Mapping( 1, "S", { Swizzle( InputSwizzleOrder::YXZ ), Negate( false, true, false ) },
                  { Trigger( InputTriggerType::Released ) } ),
         Mapping( 2, "Mouse2D",
                  { DeadZone( InputDeadZoneType::Axial, 0.1f, 0.9f ), Scalar( { 2.5f, -1.0f, 3.0f } ) },
                  { Hold( 0.75f, true ) } ),
    };
    context.Mappings[0].Triggers[0].ActuationThreshold = 0.25f;

    const auto first = ParseInputMappingContext( WriteInputMappingContext( context ) );
    ASSERT_TRUE( first ) << first.GetError();
    InputMappingContextData read = first.GetValue();
    ASSERT_TRUE( read.Header );
    EXPECT_EQ( read.Header->Dependencies, ( std::vector<std::string>{ GuidText( 1 ), GuidText( 2 ) } ) );
    read.Header.reset();
    EXPECT_EQ( read, context );

    const auto second = ParseInputMappingContext( WriteInputMappingContext( first.GetValue() ) );
    ASSERT_TRUE( second ) << second.GetError();
    EXPECT_EQ( second.GetValue(), first.GetValue() );
}

TEST( EnhancedInputAssets, ContextRefusesWhatTheSubsystemCouldNotEvaluate )
{
    InputMappingContextData context;
    context.Mappings       = { Mapping( 1, "Space", { Negate( true, true, true ) }, { Hold( 1.0f, false ) } ) };
    const std::string text = WriteInputMappingContext( context );
    ASSERT_TRUE( ParseInputMappingContext( text ) );

    EXPECT_FALSE( ParseInputMappingContext( Mutated( text, "\"Space\"", "\"Spacebar\"" ) ) ); // no such key
    EXPECT_FALSE( ParseInputMappingContext( Mutated( text, "\"INMC\":1", "\"INMC\":2" ) ) );  // other build
    EXPECT_FALSE( ParseInputMappingContext( Mutated( text, "\"Type\":\"Hold\"", "\"Type\":\"Down\"" ) ) );

    InputMappingContextData twoBlocks         = context;
    twoBlocks.Mappings[0].Modifiers[0].Scalar = InputScalarParams{};
    EXPECT_FALSE( ValidateInputMappingContext( twoBlocks ) ); // a block its Type does not read

    InputMappingContextData badZone = context;
    badZone.Mappings[0].Modifiers   = { DeadZone( InputDeadZoneType::Radial, 0.5f, 0.5f ) };
    EXPECT_FALSE( ValidateInputMappingContext( badZone ) );

    EXPECT_FALSE( ParseInputAction( text ) ); // a context is not an action
}

TEST( EnhancedInputAssets, EachKindHasItsOneRegistryRow )
{
    int actions = 0, contexts = 0;
    for ( const auto& row : Common::Content::ContentKinds() )
    {
        actions += row.Extension == std::string_view( kInputActionExtension ) ? 1 : 0;
        contexts += row.Extension == std::string_view( kInputMappingContextExtension ) ? 1 : 0;
    }
    EXPECT_EQ( actions, 1 );
    EXPECT_EQ( contexts, 1 );
}

// ---- Modifiers ----

TEST( EnhancedInputModifiers, AppliedInTheMappingsOrderWithUEMath )
{
    // Axial dead zone 0.2..1: x (0.6-0.2)/0.8 = 0.5, y -(0.3-0.2)/0.8 = -0.125; swizzle YXZ -> (-0.125, 0.5);
    // negate X -> (0.125, 0.5); scalar (2, 3, 4) -> (0.25, 1.5); Axis2D drops Z.
    const std::vector<InputModifierData> stack = { DeadZone( InputDeadZoneType::Axial, 0.2f, 1.0f ),
                                                   Swizzle( InputSwizzleOrder::YXZ ), Negate( true, false, false ),
                                                   Scalar( { 2.0f, 3.0f, 4.0f } ) };
    ExpectVec( Desert::Input::ApplyModifiers( { 0.6f, -0.3f, 0.7f }, InputValueType::Axis2D, stack ),
               { 0.25f, 1.5f, 0.0f } );

    // The same modifiers scalar-first: (1.2, -0.9) through the dead zone is (1, -0.875), swizzled (-0.875, 1),
    // negated (0.875, 1) — order is honoured.
    const std::vector<InputModifierData> reversed = {
         Scalar( { 2.0f, 3.0f, 4.0f } ), DeadZone( InputDeadZoneType::Axial, 0.2f, 1.0f ),
         Swizzle( InputSwizzleOrder::YXZ ), Negate( true, false, false ) };
    ExpectVec( Desert::Input::ApplyModifiers( { 0.6f, -0.3f, 0.7f }, InputValueType::Axis2D, reversed ),
               { 0.875f, 1.0f, 0.0f } );
}

TEST( EnhancedInputModifiers, RadialDeadZoneScalesTheLengthNotEachAxis )
{
    // |(0.3, 0.4)| = 0.5 -> (0.5-0.2)/0.8 = 0.375 along the same direction: (0.225, 0.3).
    ExpectVec( Desert::Input::ApplyModifiers( { 0.3f, 0.4f, 0.0f }, InputValueType::Axis2D,
                                              { DeadZone( InputDeadZoneType::Radial, 0.2f, 1.0f ) } ),
               { 0.225f, 0.3f, 0.0f } );
    // Inside the lower threshold: zero.
    ExpectVec( Desert::Input::ApplyModifiers( { 0.1f, 0.1f, 0.0f }, InputValueType::Axis2D,
                                              { DeadZone( InputDeadZoneType::Radial, 0.2f, 1.0f ) } ),
               { 0.0f, 0.0f, 0.0f } );
}

TEST( EnhancedInputModifiers, ValueTypeShapesTheResult )
{
    // A key on a 2D action is (1, 0); swizzled it moves forward (W on a move action).
    ExpectVec( Desert::Input::ApplyModifiers( { 1.0f, 0.0f, 0.0f }, InputValueType::Axis2D,
                                              { Swizzle( InputSwizzleOrder::YXZ ) } ),
               { 0.0f, 1.0f, 0.0f } );
    // A 1D action keeps only X: the swizzled key is gone.
    ExpectVec( Desert::Input::ApplyModifiers( { 1.0f, 0.0f, 0.0f }, InputValueType::Axis1D,
                                              { Swizzle( InputSwizzleOrder::YXZ ) } ),
               { 0.0f, 0.0f, 0.0f } );
    // A scalar does nothing to a Bool; a negated Bool is still pressed.
    ExpectVec( Desert::Input::ApplyModifiers( { 1.0f, 0.0f, 0.0f }, InputValueType::Bool,
                                              { Scalar( { 5.0f, 5.0f, 5.0f } ), Negate( true, true, true ) } ),
               { 1.0f, 0.0f, 0.0f } );
}

// ---- Triggers over a scripted frame sequence ----

namespace
{
    struct Step
    {
        bool          Down;
        TriggerEvents Expected;
    };

    TriggerEvents E( bool started, bool ongoing, bool triggered, bool completed, bool canceled )
    {
        return TriggerEvents{ started, ongoing, triggered, completed, canceled };
    }

    void RunSequence( const std::vector<InputTriggerData>& triggers, const std::vector<Step>& steps, float dt )
    {
        EnhancedInputSubsystem input;
        input.RegisterAction( Guid( 1 ), InputActionData{} );
        ASSERT_TRUE( input.AddMappingContext( Loaded( { Mapping( 1, "Space", {}, triggers ) } ), 0 ) );
        for ( std::size_t i = 0; i < steps.size(); ++i )
        {
            RawInputFrame frame;
            if ( steps[i].Down )
                frame.KeysDown.push_back( Common::KeyCode::Space );
            input.Tick( frame, dt );
            EXPECT_EQ( input.GetTriggerEvents( Guid( 1 ) ), steps[i].Expected ) << "frame " << i;
        }
    }
} // namespace

TEST( EnhancedInputTriggers, PressedFiresOnceOnTheFrameThePressBegins )
{
    RunSequence( { Trigger( InputTriggerType::Pressed ) },
                 { { false, E( false, false, false, false, false ) },
                   { true, E( true, false, true, false, false ) },  // Started + Triggered, once
                   { true, E( false, false, false, true, false ) }, // still held: the trigger is over
                   { true, E( false, false, false, false, false ) },
                   { false, E( false, false, false, false, false ) },
                   { true, E( true, false, true, false, false ) } }, // a new press fires again
                 1.0f / 60.0f );
}

TEST( EnhancedInputTriggers, HoldFiresAfterItsSecondsAndCompletesOnRelease )
{
    // 0.5 s at 0.2 s frames: held 0.2 and 0.4 are Ongoing, 0.6 crosses the threshold.
    RunSequence( { Hold( 0.5f, false ) },
                 { { true, E( true, false, false, false, false ) },
                   { true, E( false, true, false, false, false ) },
                   { true, E( false, false, true, false, false ) },
                   { true, E( false, false, true, false, false ) }, // not one-shot: every held frame
                   { false, E( false, false, false, true, false ) } },
                 0.2f );
    // Released before the threshold: Canceled, never Triggered.
    RunSequence(
         { Hold( 0.5f, false ) },
         { { true, E( true, false, false, false, false ) }, { false, E( false, false, false, false, true ) } },
         0.2f );
    // One-shot: one Triggered frame, then Completed although still held.
    RunSequence( { Hold( 0.5f, true ) },
                 { { true, E( true, false, false, false, false ) },
                   { true, E( false, true, false, false, false ) },
                   { true, E( false, false, true, false, false ) },
                   { true, E( false, false, false, true, false ) } },
                 0.2f );
}

TEST( EnhancedInputTriggers, ReleasedFiresOnTheFrameThePressEnds )
{
    RunSequence( { Trigger( InputTriggerType::Released ) },
                 { { true, E( true, false, false, false, false ) },
                   { true, E( false, true, false, false, false ) },
                   { false, E( false, false, true, false, false ) },
                   { false, E( false, false, false, true, false ) } },
                 1.0f / 60.0f );
}

TEST( EnhancedInputTriggers, DownTriggersEveryHeldFrameAndReportsTheValue )
{
    EnhancedInputSubsystem input;
    input.RegisterAction( Guid( 1 ), InputActionData{} );
    ASSERT_TRUE( input.AddMappingContext(
         Loaded( { Mapping( 1, "E", {}, { Trigger( InputTriggerType::Down ) } ) } ), 0 ) );
    input.Tick( Keys( { Common::KeyCode::E } ), 0.25f );
    input.Tick( Keys( { Common::KeyCode::E } ), 0.25f );
    EXPECT_EQ( input.GetTriggerState( Guid( 1 ) ), TriggerState::Triggered );
    EXPECT_FLOAT_EQ( input.GetTriggeredSeconds( Guid( 1 ) ), 0.25f );
    ExpectVec( input.GetActionValue( Guid( 1 ) ), { 1.0f, 0.0f, 0.0f } );
    input.Tick( Keys( {} ), 0.25f );
    ExpectVec( input.GetActionValue( Guid( 1 ) ), { 0.0f, 0.0f, 0.0f } );
}

// ---- Context stack ----

TEST( EnhancedInputContexts, HigherPriorityContextConsumesAKeyTheLowerOneAlsoMaps )
{
    EnhancedInputSubsystem input;
    input.RegisterAction( Guid( 1 ), InputActionData{} ); // Jump, consumes
    input.RegisterAction( Guid( 2 ), InputActionData{} ); // Interact
    const InputMappingContextData low  = Loaded( { Mapping( 2, "Space" ), Mapping( 2, "F" ) } );
    const InputMappingContextData high = Loaded( { Mapping( 1, "Space" ) } );
    ASSERT_TRUE( input.AddMappingContext( low, 0 ) ); // added first: priority, not order, decides
    ASSERT_TRUE( input.AddMappingContext( high, 5 ) );

    input.Tick( Keys( { Common::KeyCode::Space } ), 0.016f );
    EXPECT_EQ( input.GetTriggerState( Guid( 1 ) ), TriggerState::Triggered );
    EXPECT_EQ( input.GetTriggerState( Guid( 2 ) ), TriggerState::None ); // Space was consumed

    // A key the high context does not map still reaches the low one.
    input.Tick( Keys( { Common::KeyCode::Space, Common::KeyCode::F } ), 0.016f );
    EXPECT_EQ( input.GetTriggerState( Guid( 1 ) ), TriggerState::Triggered );
    EXPECT_EQ( input.GetTriggerState( Guid( 2 ) ), TriggerState::Triggered );

    ASSERT_TRUE( input.RemoveMappingContext( ContextGuid( high ) ) );
    input.Tick( Keys( { Common::KeyCode::Space } ), 0.016f );
    EXPECT_EQ( input.GetTriggerState( Guid( 2 ) ), TriggerState::Triggered );
    EXPECT_EQ( input.GetTriggerState( Guid( 1 ) ), TriggerState::None );
    EXPECT_TRUE( input.GetTriggerEvents( Guid( 1 ) ).Completed ); // its mapping vanished while held
}

TEST( EnhancedInputContexts, AnActionThatDoesNotConsumeLeavesTheKeyToLowerContexts )
{
    EnhancedInputSubsystem input;
    InputActionData        passThrough;
    passThrough.ConsumeInput = false;
    input.RegisterAction( Guid( 1 ), passThrough );
    input.RegisterAction( Guid( 2 ), InputActionData{} );
    ASSERT_TRUE( input.AddMappingContext( Loaded( { Mapping( 1, "Space" ) } ), 5 ) );
    ASSERT_TRUE( input.AddMappingContext( Loaded( { Mapping( 2, "Space" ) } ), 0 ) );
    input.Tick( Keys( { Common::KeyCode::Space } ), 0.016f );
    EXPECT_EQ( input.GetTriggerState( Guid( 1 ) ), TriggerState::Triggered );
    EXPECT_EQ( input.GetTriggerState( Guid( 2 ) ), TriggerState::Triggered );
}

TEST( EnhancedInputContexts, MappingsOfOneActionCombineByHighestAbsoluteComponent )
{
    EnhancedInputSubsystem input;
    InputActionData        move;
    move.ValueType = InputValueType::Axis2D;
    input.RegisterAction( Guid( 3 ), move );
    ASSERT_TRUE( input.AddMappingContext(
         Loaded( { Mapping( 3, "W", { Swizzle( InputSwizzleOrder::YXZ ) } ),
                   Mapping( 3, "A", { Negate( true, false, false ) } ), Mapping( 3, "D" ) } ),
         0 ) );
    input.Tick( Keys( { Common::KeyCode::W, Common::KeyCode::A } ), 0.016f );
    ExpectVec( input.GetActionValue( Guid( 3 ) ), { -1.0f, 1.0f, 0.0f } );
}

TEST( EnhancedInputContexts, AContextNamingAnUnregisteredActionIsRefused )
{
    EnhancedInputSubsystem input;
    input.RegisterAction( Guid( 1 ), InputActionData{} );
    EXPECT_FALSE( input.AddMappingContext( Loaded( { Mapping( 1, "Space" ), Mapping( 9, "F" ) } ), 0 ) );
    input.Tick( Keys( { Common::KeyCode::Space } ), 0.016f );
    EXPECT_EQ( input.GetTriggerState( Guid( 1 ) ), TriggerState::None ); // nothing of it was added
}

// ---- GP1b: state across a context switch (UE RebuildControlMappings keeps the mappings' instance data) ----

TEST( EnhancedInputContexts, AKeyHeldAcrossAContextSwitchDoesNotPressAgain )
{
    EnhancedInputSubsystem input;
    input.RegisterAction( Guid( 1 ), InputActionData{} );
    const InputMappingContextData onFoot =
         Loaded( { Mapping( 1, "Space", {}, { Trigger( InputTriggerType::Pressed ) } ) } );
    const InputMappingContextData inCar =
         Loaded( { Mapping( 1, "Space", {}, { Trigger( InputTriggerType::Pressed ) } ) } );
    ASSERT_TRUE( input.AddMappingContext( onFoot, 0 ) );
    input.Tick( Keys( { Common::KeyCode::Space } ), 0.016f );
    EXPECT_TRUE( input.GetTriggerEvents( Guid( 1 ) ).Triggered );

    // The switch happens while Space is still held: the same action on the same key is still mapped.
    ASSERT_TRUE( input.AddMappingContext( inCar, 5 ) );
    ASSERT_TRUE( input.RemoveMappingContext( ContextGuid( onFoot ) ) );
    input.Tick( Keys( { Common::KeyCode::Space } ), 0.016f );
    EXPECT_FALSE( input.GetTriggerEvents( Guid( 1 ) ).Triggered ) << "the held key pressed again after the switch";
    EXPECT_FALSE( input.GetTriggerEvents( Guid( 1 ) ).Started );

    // A NEW press through the new context still fires.
    input.Tick( Keys( {} ), 0.016f );
    input.Tick( Keys( { Common::KeyCode::Space } ), 0.016f );
    EXPECT_TRUE( input.GetTriggerEvents( Guid( 1 ) ).Triggered );
}

TEST( EnhancedInputContexts, AHoldKeepsItsTimeAcrossAContextSwitch )
{
    EnhancedInputSubsystem input;
    input.RegisterAction( Guid( 1 ), InputActionData{} );
    const InputMappingContextData a = Loaded( { Mapping( 1, "E", {}, { Hold( 0.5f, false ) } ) } );
    const InputMappingContextData b = Loaded( { Mapping( 1, "E", {}, { Hold( 0.5f, false ) } ) } );
    ASSERT_TRUE( input.AddMappingContext( a, 0 ) );
    input.Tick( Keys( { Common::KeyCode::E } ), 0.2f );
    input.Tick( Keys( { Common::KeyCode::E } ), 0.2f );
    EXPECT_EQ( input.GetTriggerState( Guid( 1 ) ), TriggerState::Ongoing );
    ASSERT_TRUE( input.AddMappingContext( b, 1 ) );
    ASSERT_TRUE( input.RemoveMappingContext( ContextGuid( a ) ) );
    input.Tick( Keys( { Common::KeyCode::E } ), 0.2f ); // 0.6 s held in all
    EXPECT_EQ( input.GetTriggerState( Guid( 1 ) ), TriggerState::Triggered ) << "the hold started over";
}

// ---- GP1b: the player's own keys (UE player-mappable keys), saved per user, never in the asset ----

TEST( EnhancedInputUserKeys, ARemappedKeyDrivesTheMappingAndTheDefaultNoLonger )
{
    EnhancedInputSubsystem input;
    input.RegisterAction( Guid( 1 ), InputActionData{} );
    const InputMappingContextData context = Loaded( { Mapping( 1, "Space" ) } );
    ASSERT_TRUE( input.AddMappingContext( context, 0 ) );
    ASSERT_TRUE( input.RemapKey( ContextGuid( context ), Guid( 1 ), "Space", "F" ) );
    input.Tick( Keys( { Common::KeyCode::Space } ), 0.016f );
    EXPECT_EQ( input.GetTriggerState( Guid( 1 ) ), TriggerState::None );
    input.Tick( Keys( { Common::KeyCode::F } ), 0.016f );
    EXPECT_EQ( input.GetTriggerState( Guid( 1 ) ), TriggerState::Triggered );
    ASSERT_EQ( input.MappedKeys().size(), 1u );
    EXPECT_EQ( input.MappedKeys()[0], *Desert::Input::InputKeyFromName( "F" ) );
    EXPECT_EQ( context.Mappings[0].Key, "Space" ) << "the asset's key changed";

    EXPECT_FALSE( input.RemapKey( ContextGuid( context ), Guid( 1 ), "Space", "NoSuchKey" ) );
    ASSERT_TRUE( input.RemapKey( ContextGuid( context ), Guid( 1 ), "Space", "Space" ) );
    EXPECT_TRUE( input.GetUserKeyBindings().Overrides.empty() ) << "mapping back to the default kept an override";
}

TEST( EnhancedInputUserKeys, BindingsRoundTripThroughTheUserFileAndRefuseAnUnknownKey )
{
    namespace fs       = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "GP1bUserKeys";
    fs::remove_all( dir );
    const auto missing = Desert::Input::LoadUserKeyBindings( dir / "input.json" );
    ASSERT_TRUE( missing );
    EXPECT_TRUE( missing.GetValue().Overrides.empty() );

    Desert::Input::UserKeyBindings bindings;
    bindings.Overrides.push_back( { GuidText( 7 ), GuidText( 1 ), "Space", "F" } );
    ASSERT_TRUE( Desert::Input::SaveUserKeyBindings( dir / "input.json", bindings ) );
    const auto read = Desert::Input::LoadUserKeyBindings( dir / "input.json" );
    ASSERT_TRUE( read ) << read.GetError();
    EXPECT_EQ( read.GetValue(), bindings );

    bindings.Overrides[0].Key = "NoSuchKey";
    ASSERT_TRUE( Desert::Input::SaveUserKeyBindings( dir / "input.json", bindings ) );
    EXPECT_FALSE( Desert::Input::LoadUserKeyBindings( dir / "input.json" ) );
    fs::remove_all( dir );
}

// ---- GP1b: the local player (UE ULocalPlayer + its Enhanced Input subsystem) ----

TEST( EnhancedInputPlayer, SamplesOnlyTheKeysTheActiveMappingsRead )
{
    const std::vector<Desert::Input::InputKey> keys  = { *Desert::Input::InputKeyFromName( "Space" ),
                                                         *Desert::Input::InputKeyFromName( "LeftMouseButton" ) };
    const RawInputFrame                        frame = Desert::Input::SampleRawInput(
         keys, { 3.0f, -2.0f }, []( Common::KeyCode ) { return true; },
         []( Common::MouseButton ) { return true; } );
    EXPECT_EQ( frame.KeysDown, std::vector<Common::KeyCode>{ Common::KeyCode::Space } );
    EXPECT_EQ( frame.MouseButtonsDown, std::vector<Common::MouseButton>{ Common::MouseButton::Left } );
    EXPECT_EQ( frame.MouseDelta, glm::vec2( 3.0f, -2.0f ) );
}

TEST( EnhancedInputPlayer, ComponentContextsAreAddedHighestPriorityFirst )
{
    Desert::ECS::EnhancedInputPlayerData player;
    player.Contexts     = { Desert::Assets::AssetHandle( uint64_t{ 11 } ),
                            Desert::Assets::AssetHandle( uint64_t{ 22 } ),
                            Desert::Assets::AssetHandle( uint64_t{ 33 } ) };
    player.BasePriority = 10;
    const auto ordered  = Desert::Input::PlayerContextPriorities( player );
    ASSERT_EQ( ordered.size(), 3u );
    EXPECT_EQ( ordered[0].second, 12 );
    EXPECT_EQ( ordered[1].second, 11 );
    EXPECT_EQ( ordered[2].second, 10 );
    EXPECT_EQ( static_cast<uint64_t>( ordered[0].first ), 11u );
}

TEST( EnhancedInputPlayer, ActionsAndContextsAreCalledByTheirFileNames )
{
    Desert::Input::LocalPlayerInput player;
    const InputMappingContextData   context = Loaded( { Mapping( 1, "Space" ) } );
    EXPECT_FALSE( player.AddLoadedContext( "IMC_Default", context, {}, 0 ) ) << "an action with no data was added";
    ASSERT_TRUE( player.AddLoadedContext( "IMC_Default", context, { { GuidText( 1 ), InputActionData{} } }, 0 ) );
    ASSERT_TRUE( player.ActionNamed( "IA_1" ).has_value() );
    EXPECT_EQ( *player.ActionNamed( "IA_1" ), Guid( 1 ) );
    player.Subsystem().Tick( Keys( { Common::KeyCode::Space } ), 0.016f );
    EXPECT_EQ( player.Subsystem().GetTriggerState( Guid( 1 ) ), TriggerState::Triggered );
    EXPECT_TRUE( player.RemoveContext( "IMC_Default" ) );
    EXPECT_FALSE( player.RemoveContext( "IMC_Default" ) );
}

namespace
{
    std::string SourceText( const char* path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }
} // namespace

// The player's input is evaluated once per played frame, BEFORE the scripts read it, and ended with Play.
TEST( EnhancedInputPlayer, ScriptSystemTicksThePlayerInputBeforeTheScripts )
{
    const std::string system = SourceText( "Desert/Desert/Source/Engine/ECS/System/ScriptSystem.hpp" );
    ASSERT_FALSE( system.empty() );
    const size_t tick    = system.find( "m_Engine.TickPlayerInput(" );
    const size_t scripts = system.find( "m_Engine.CallUpdate(" );
    ASSERT_NE( tick, std::string::npos ) << "nothing ticks the player's Enhanced Input in Play";
    EXPECT_EQ( system.find( "m_Engine.TickPlayerInput(", tick + 1 ), std::string::npos ) << "ticked twice";
    EXPECT_LT( tick, scripts );
    EXPECT_NE( system.find( "m_Engine.EndPlayerInput()" ), std::string::npos );
}

// The Lua `Input` table keeps its raw keys and gains the actions, contexts and rebinding.
TEST( EnhancedInputPlayer, LuaInputTableNamesActionsContextsAndRebinding )
{
    const std::string lua = SourceText( "Desert/Desert/Source/Engine/Scripting/InputBindings.cpp" );
    ASSERT_FALSE( lua.empty() );
    for ( const char* name :
          { "isKeyDown", "wasPressed", "actionValue", "actionTriggered", "actionStarted", "actionOngoing",
            "actionCompleted", "actionCanceled", "actionSeconds", "addContext", "removeContext", "rebindKey" } )
        EXPECT_NE( lua.find( std::string( "{ \"" ) + name + "\", &" ), std::string::npos ) << name;
    EXPECT_NE( lua.find( "luaL_register( L, \"Input\", kInput )" ), std::string::npos )
         << "the entries are the global Input table's";
}

namespace
{
    // The host steps this suite's process takes before gtest starts (TestSupport/runner.hpp).
    const Desert::TestSupport::SuiteHost kHostSteps{ { .EngineDir = true, .Project = true } };
} // namespace

// GP1c: the key picker of the mapping-context editor lists Desert::Input::InputKeyNames(). Every name it offers
// must be one a mapping may state (InputKeyFromName accepts it), each once, and every key the parser knows must
// be offered: letters, digits, F1..F12 and the 21 named keys. A key added to the parser and not to the list (or
// the reverse) goes red here.
TEST( EnhancedInputKeys, ThePickerListsEveryAcceptedNameOnce )
{
    const std::vector<std::string> names = Desert::Input::InputKeyNames();
    EXPECT_EQ( names.size(), 26u + 10u + 12u + 21u );
    std::vector<std::string> sorted = names;
    std::sort( sorted.begin(), sorted.end() );
    EXPECT_EQ( std::adjacent_find( sorted.begin(), sorted.end() ), sorted.end() ) << "a name is listed twice";
    for ( const std::string& name : names )
        EXPECT_TRUE( Desert::Input::InputKeyFromName( name ).has_value() ) << name;
    std::vector<Desert::Input::InputKey> keys;
    for ( const std::string& name : names )
        keys.push_back( *Desert::Input::InputKeyFromName( name ) );
    for ( std::size_t i = 0; i < keys.size(); ++i )
        for ( std::size_t j = i + 1; j < keys.size(); ++j )
            EXPECT_FALSE( keys[i] == keys[j] ) << names[i] << " and " << names[j] << " are one key";
}
