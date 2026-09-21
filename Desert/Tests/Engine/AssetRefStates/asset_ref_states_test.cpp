// THREE STATES, AND THE WHOLE VALUE OF THIS SUITE IS THAT IT REDDENS WHEN TWO OF THEM ARE MERGED.
//
// Before anything in this engine was demand-driven, a consumer asked a service for a raw pointer and
// branched on null — and null answered two questions at once: "the artist chose nothing" and "the
// artist chose something that has not arrived". The second could not happen while every content kind
// was read before the first frame, so the conflation cost nothing and nobody saw it. The moment a
// kind becomes demand-driven it happens on every scene load, and the existing branch turns an
// ordinary in-flight read into the diagnostic for a broken reference: `EnsureNoiseVolumes` logs "the
// clouds will not render for this view until one is registered" and the sky falls back to its
// procedural form without anyone being told a load was in progress.
//
// So the property under test is not "the predicates return the right bool". It is that the three
// states are MUTUALLY EXCLUSIVE AND EXHAUSTIVE, and that each of the three pairs is distinguishable.
// `NullAndPendingAreDistinguishable` is the positive control named in this task's brief: implement
// `IsNull()` as `return m_Payload == nullptr;` -- the obvious spelling, and the merge -- and that
// test alone goes red while every other assertion in this file still passes.

#include <Engine/Assets/AssetRef.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using Desert::Assets::AssetHandle;
using Desert::Assets::AssetRef;

namespace
{
    /// A payload with an identity, so a test can tell WHICH thing came back rather than only that
    /// something did. Deliberately not an asset: `AssetRef` is over "a named thing that may not be
    /// here", and the three cloud services hand back a GPU image or a decoded layout rather than the
    /// asset object.
    struct Payload
    {
        explicit Payload( int value ) : Value( value )
        {
        }
        int Value = 0;
    };

    AssetHandle SomeHandle()
    {
        return AssetHandle{ 0x5EEDu };
    }

    /// The three predicates as one readable triple, so an assertion can name the whole state rather
    /// than three booleans a reader has to reassemble.
    std::string StateOf( const AssetRef<Payload>& ref )
    {
        std::string text;
        if ( ref.IsNull() )
            text += "Null ";
        if ( ref.IsPending() )
            text += "Pending ";
        if ( ref.IsValid() )
            text += "Valid ";
        return text.empty() ? "NONE" : text;
    }
} // namespace

// ── EXCLUSIVITY AND EXHAUSTIVENESS, OVER EVERY WAY ONE CAN BE BUILT ──────────────────────────────

TEST( AssetRefStates, ExactlyOneOfTheThreeHoldsForEveryConstruction )
{
    std::vector<std::pair<std::string, AssetRef<Payload>>> refs;
    refs.emplace_back( "default-constructed", AssetRef<Payload>() );
    refs.emplace_back( "Null()", AssetRef<Payload>::Null() );
    refs.emplace_back( "Pending()", AssetRef<Payload>::Pending( SomeHandle() ) );
    refs.emplace_back( "Ready()", AssetRef<Payload>::Ready( SomeHandle(), std::make_shared<Payload>( 7 ) ) );
    refs.emplace_back( "Ready() with a null payload", AssetRef<Payload>::Ready( SomeHandle(), nullptr ) );

    for ( const auto& [name, ref] : refs )
    {
        const int held = ( ref.IsNull() ? 1 : 0 ) + ( ref.IsPending() ? 1 : 0 ) + ( ref.IsValid() ? 1 : 0 );
        EXPECT_EQ( held, 1 ) << name << " held the states { " << StateOf( ref )
                             << "}. Exactly one must hold: a construction holding none is a state no "
                                "caller can branch on, and one holding two is the conflation this type "
                                "exists to remove.";
    }
}

// ── THE THREE PAIRS. EACH IS A MERGE SOMEBODY COULD MAKE, AND EACH HAS ITS OWN TEST ──────────────

TEST( AssetRefStates, NullAndPendingAreDistinguishable )
{
    const auto nothing = AssetRef<Payload>::Null();
    const auto coming  = AssetRef<Payload>::Pending( SomeHandle() );

    // THE POSITIVE CONTROL. Both of these have no payload, so any predicate written only against the
    // payload answers the same for both -- which is precisely the pre-existing raw-pointer behaviour
    // and precisely the defect. `IsNull()` implemented as `!m_Payload` turns the second EXPECT red and
    // leaves every other assertion in this file green.
    EXPECT_TRUE( nothing.IsNull() );
    EXPECT_FALSE( coming.IsNull() ) << "a pending reference reported itself as Null. A caller reading "
                                       "that answer takes its fallback path -- the procedural sky, the "
                                       "default volume, no hero cloud -- for content that is on its way, "
                                       "and logs a broken reference that is not broken.";

    EXPECT_TRUE( coming.IsPending() );
    EXPECT_FALSE( nothing.IsPending() ) << "an empty slot reported itself as Pending. A caller reading "
                                           "that answer waits forever for content nobody asked for.";
}

TEST( AssetRefStates, PendingAndReadyAreDistinguishable )
{
    const auto coming = AssetRef<Payload>::Pending( SomeHandle() );
    const auto here   = AssetRef<Payload>::Ready( SomeHandle(), std::make_shared<Payload>( 3 ) );

    EXPECT_TRUE( coming.IsPending() );
    EXPECT_FALSE( here.IsPending() );
    EXPECT_TRUE( here.IsValid() );
    EXPECT_FALSE( coming.IsValid() );
}

TEST( AssetRefStates, NullAndReadyAreDistinguishable )
{
    const auto nothing = AssetRef<Payload>::Null();
    const auto here    = AssetRef<Payload>::Ready( SomeHandle(), std::make_shared<Payload>( 3 ) );

    EXPECT_TRUE( nothing.IsNull() );
    EXPECT_FALSE( here.IsNull() );
    EXPECT_TRUE( here.IsValid() );
    EXPECT_FALSE( nothing.IsValid() );
}

// ── `Get()` NEVER LOADS, WHICH IS ASSERTED AS "IT ANSWERS FROM WHAT IT HOLDS AND NOTHING ELSE" ────

TEST( AssetRefStates, GetAnswersNullUntilTheThingIsHereAndTheReferenceOwnsNoLoader )
{
    EXPECT_EQ( AssetRef<Payload>::Null().Get(), nullptr );
    EXPECT_EQ( AssetRef<Payload>::Pending( SomeHandle() ).Get(), nullptr );

    const auto payload = std::make_shared<Payload>( 42 );
    const auto here    = AssetRef<Payload>::Ready( SomeHandle(), payload );
    ASSERT_NE( here.Get(), nullptr );
    EXPECT_EQ( here.Get()->Value, 42 );
    EXPECT_EQ( here.Get(), payload.get() );

    // THE STRUCTURAL HALF OF "NEVER LOADS", and it is the half a behavioural test cannot reach: the
    // type holds a handle, a `shared_ptr` and a bool, and nothing else. No service, no manager, no
    // filesystem -- so there is no member a `Get()` could reach a file through even if somebody wanted
    // it to. If this ever stops holding, it is because a loader was added, and the reader should be
    // here rather than discovering it from a frame time.
    static_assert( sizeof( AssetRef<Payload> ) <=
                        sizeof( AssetHandle ) + sizeof( std::shared_ptr<Payload> ) + alignof( void* ),
                   "AssetRef grew past a handle, a shared_ptr and a flag. If what was added can load, "
                   "`Get()` can now block and the rule this type exists for is gone." );
}

TEST( AssetRefStates, AReadyReferenceWithNothingInItIsRefusedRatherThanReportedReady )
{
    // "An empty successful answer is a silent wrong answer." A Ready state carrying no payload would
    // make `IsValid()` true and `Get()` null at the same time, which is the one combination no caller
    // in this engine checks for.
    const auto refused = AssetRef<Payload>::Ready( SomeHandle(), nullptr );
    EXPECT_FALSE( refused.IsValid() );
    EXPECT_TRUE( refused.IsNull() );
    EXPECT_EQ( refused.Get(), nullptr );
}

TEST( AssetRefStates, AReferenceCanNameItsAssetInEveryState )
{
    // A pending reference that could not say WHICH asset is late is a diagnostic that says only
    // "something is late", which is what the log used to say and what cost the investigations.
    EXPECT_EQ( AssetRef<Payload>::Pending( SomeHandle() ).Handle(), SomeHandle() );
    EXPECT_EQ( AssetRef<Payload>::Ready( SomeHandle(), std::make_shared<Payload>( 1 ) ).Handle(), SomeHandle() );
    // Null names nothing, because nothing was asked for.
    EXPECT_EQ( AssetRef<Payload>::Null().Handle(), AssetHandle( 0 ) );
}

TEST( AssetRefStates, AReadyReferenceKeepsItsPayloadAlive )
{
    std::weak_ptr<Payload> observer;
    {
        auto payload    = std::make_shared<Payload>( 9 );
        observer        = payload;
        const auto here = AssetRef<Payload>::Ready( SomeHandle(), std::move( payload ) );
        EXPECT_FALSE( observer.expired() );
        EXPECT_TRUE( here.IsValid() );
    }
    EXPECT_TRUE( observer.expired() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
