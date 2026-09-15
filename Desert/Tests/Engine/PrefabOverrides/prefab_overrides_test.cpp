// DOES AN INSTANCE KEEP ITS OWN EDITS, AND DOES IT STILL FOLLOW ITS SOURCE?
//
// Those two sentences are the whole definition of a prefab, and until Ю19 this engine honoured neither
// for anything below a prefab instance's root: SceneSerializer skipped every entity under a
// PrefabComponent when writing a `.desce`, and on load it copied back the root's translation, rotation
// and scale and discarded everything else. Recolour a button inside an instance, save, reload: the
// colour is gone and no line anywhere says so. The corpus never caught it because the corpus contains
// ZERO `.deprefab` files — the whole subsystem had never been exercised on a file.
//
// What is asserted here is the RELATION, not either side of it: an override must record exactly what
// differs (so everything else keeps following the source) and must reproduce exactly what was edited
// (so the edit is not lost). A diff that records too much and a diff that records too little both pass
// a test of either half alone, and they are the two ways this kind of system leaks.
//
// It is pure by construction. PrefabOverrides.cpp takes two parsed records and returns a difference;
// no scene, no entity, no asset manager, no GPU. The walk over a live scene is
// Core::Serialize::CapturePrefabInstance, which reaches the entity serializer and through it the whole
// engine — no suite can link that, which is exactly why the decision lives where a suite can.

#include <gtest/gtest.h>

#include <Engine/Assets/Prefab/PrefabOverrides.hpp>

#include <rflcpp/rfl/json.hpp>

#include <string>
#include <vector>

using Common::UUID;
using Desert::Assets::DiffPrefabEntity;
using Desert::Assets::EntityData;
using Desert::Assets::LayerOverrideOntoRecord;
using Desert::Assets::OverrideAsEntityData;
using Desert::Assets::PrefabDiffReport;
using Desert::Assets::PrefabOverrideData;
using Desert::Assets::PrefabPathKey;

namespace
{
    // A component payload, as a component payload actually reaches these functions: an rfl::Generic
    // object tree, the same thing ComponentRegistry hands EntityData.
    rfl::Generic Payload( const std::string& key, double value )
    {
        rfl::Generic::Object object;
        object[key] = rfl::Generic( value );
        return { object };
    }

    std::string Text( const rfl::Generic& g )
    {
        return rfl::json::write( g );
    }

    // A UI button record as the capture sees one: a tag, a transform, and two component payloads.
    EntityData BaseRecord()
    {
        EntityData data;
        data.id                     = UUID( 7001 );
        data.Tag                    = "Play Button";
        data.Translation            = glm::vec3( 0.0f, 0.0f, 0.0f );
        data.Rotation               = glm::vec3( 0.0f );
        data.Scale                  = glm::vec3( 1.0f );
        data.Components["UIPanel"]  = Payload( "Color", 0.25 );
        data.Components["UILayout"] = Payload( "OffsetMinX", 10.0 );
        return data;
    }
} // namespace

// gtest's ASSERT_TRUE returns from the enclosing function, so every `*over` and `.value()` below is
// guarded — the analyser cannot see that, and marking each of the twenty sites individually would bury
// the assertions in annotations. Named once, for the whole body, with the reason.
// NOLINTBEGIN(bugprone-unchecked-optional-access)

// --- The two halves of the definition ---------------------------------------------------------------

TEST( PrefabOverrides, UntouchedInstanceRecordsNothing )
{
    const EntityData base = BaseRecord();
    const EntityData live = BaseRecord();

    // An empty override record is a pinned copy of nothing: it costs the file a line and the reader a
    // question, and — worse — any field it happens to state stops following the source.
    EXPECT_FALSE( DiffPrefabEntity( base, live, { UUID( 7001 ) } ).has_value() );
}

TEST( PrefabOverrides, OnlyTheEditedKeyIsRecorded )
{
    const EntityData base      = BaseRecord();
    EntityData       live      = BaseRecord();
    live.Components["UIPanel"] = Payload( "Color", 0.90 ); // the user recoloured the panel

    const auto over = DiffPrefabEntity( base, live, { UUID( 7001 ) } );
    ASSERT_TRUE( over.has_value() );

    EXPECT_EQ( over->Components.size(), 1u );
    ASSERT_TRUE( over->Components.get( "UIPanel" ).has_value() );
    EXPECT_EQ( Text( over->Components.get( "UIPanel" ).value() ), Text( Payload( "Color", 0.90 ) ) );

    // THE ASSERTION THAT MAKES IT A PREFAB: the layout was not touched, so the override must not mention
    // it. If it did, a later edit to the prefab's layout would reach no instance that had ever been
    // recoloured — which is copy-paste with extra steps.
    EXPECT_FALSE( over->Components.get( "UILayout" ).has_value() );
    EXPECT_FALSE( over->Tag.has_value() );
    EXPECT_FALSE( over->Translation.has_value() );
    EXPECT_FALSE( over->Rotation.has_value() );
    EXPECT_FALSE( over->Scale.has_value() );
}

TEST( PrefabOverrides, SourceEditsReachAnOverriddenInstance )
{
    // 1. An instance is recoloured.
    const EntityData base      = BaseRecord();
    EntityData       live      = BaseRecord();
    live.Components["UIPanel"] = Payload( "Color", 0.90 );
    const auto over            = DiffPrefabEntity( base, live, { UUID( 7001 ) } );
    ASSERT_TRUE( over.has_value() );

    // 2. The PREFAB is then edited — somewhere else: its layout moves, and its own colour changes too.
    EntityData newBase             = BaseRecord();
    newBase.Components["UILayout"] = Payload( "OffsetMinX", 64.0 );
    newBase.Components["UIPanel"]  = Payload( "Color", 0.10 );
    newBase.Tag                    = "Primary Button";

    // 3. The instance is rebuilt from the new source with its override on top.
    EntityData result = newBase;
    LayerOverrideOntoRecord( result, *over );

    // The untouched key followed the source...
    EXPECT_EQ( Text( result.Components.get( "UILayout" ).value() ), Text( Payload( "OffsetMinX", 64.0 ) ) );
    EXPECT_EQ( result.Tag, "Primary Button" );
    // ... and the edited one did not.
    EXPECT_EQ( Text( result.Components.get( "UIPanel" ).value() ), Text( Payload( "Color", 0.90 ) ) );
}

// --- The transform is three fields, not one ---------------------------------------------------------

TEST( PrefabOverrides, MovingAnInstanceDoesNotPinItsScale )
{
    const EntityData base = BaseRecord();
    EntityData       live = BaseRecord();
    live.Translation      = glm::vec3( 120.0f, 0.0f, 0.0f );

    const auto over = DiffPrefabEntity( base, live, { UUID( 7001 ) } );
    ASSERT_TRUE( over.has_value() );
    ASSERT_TRUE( over->Translation.has_value() );
    EXPECT_EQ( over->Translation->x, 120.0f );
    // Pinning the whole transform because one axis moved would freeze an instance's scale against every
    // later edit of the prefab's — the same leak as the component case, one level down.
    EXPECT_FALSE( over->Scale.has_value() );
    EXPECT_FALSE( over->Rotation.has_value() );
}

// --- The two appliers must agree --------------------------------------------------------------------

TEST( PrefabOverrides, RecordApplierAndEntityApplierCarryTheSameValues )
{
    const EntityData base      = BaseRecord();
    EntityData       live      = BaseRecord();
    live.Tag                   = "Quit Button";
    live.Components["UIPanel"] = Payload( "Color", 0.5 );
    const auto over            = DiffPrefabEntity( base, live, { UUID( 7001 ) } );
    ASSERT_TRUE( over.has_value() );

    // One applier writes a record (the capture's base), the other builds the EntityData the live-entity
    // deserializer consumes. They are two functions and one meaning; a suite is the only thing that can
    // keep them that way.
    EntityData layered = BaseRecord();
    LayerOverrideOntoRecord( layered, *over );
    const EntityData asEntityData = OverrideAsEntityData( *over );

    EXPECT_EQ( layered.Tag, asEntityData.Tag );
    EXPECT_EQ( Text( layered.Components.get( "UIPanel" ).value() ),
               Text( asEntityData.Components.get( "UIPanel" ).value() ) );
    // What the override does not state, the entity form does not state either — that is what makes
    // "apply on top of the instantiated base" mean the same thing on both paths.
    EXPECT_FALSE( asEntityData.Components.get( "UILayout" ).has_value() );
    EXPECT_FALSE( asEntityData.Translation.has_value() );
}

// --- What the mechanism cannot express, it counts ---------------------------------------------------

TEST( PrefabOverrides, ARemovedComponentIsCountedRatherThanDropped )
{
    const EntityData base = BaseRecord();
    EntityData       live = BaseRecord();
    // The user deleted UIPanel from this one instance. An override is applied ON TOP of the instantiated
    // base, so there is no value that means "and remove this"; the base's copy comes back on the next
    // load. Saying so is the difference between a known limit and the silent loss this whole task is
    // about.
    live.Components.clear();
    live.Components["UILayout"] = Payload( "OffsetMinX", 10.0 );

    PrefabDiffReport report;
    const auto       over = DiffPrefabEntity( base, live, { UUID( 7001 ) }, &report );

    EXPECT_EQ( report.RemovedComponents, 1u );
    EXPECT_FALSE( over.has_value() ) << "nothing that remains differs, so there is nothing to record";
}

// --- Addressing -------------------------------------------------------------------------------------

TEST( PrefabOverrides, PathsOfDifferentDepthCannotCollide )
{
    // A nested prefab's entity is addressed by the chain of record ids, so the key has to separate them.
    // Concatenating the decimal ids without a separator would make [12, 3] and [1, 23] one address, and
    // an override would then land on the wrong entity of the wrong nested instance.
    const std::vector<UUID> a{ UUID( 12 ), UUID( 3 ) };
    const std::vector<UUID> b{ UUID( 1 ), UUID( 23 ) };
    EXPECT_NE( PrefabPathKey( a ), PrefabPathKey( b ) );

    // And a prefix is not the whole path: the nesting record itself is not the entity inside it.
    const std::vector<UUID> prefix{ UUID( 12 ) };
    EXPECT_NE( PrefabPathKey( prefix ), PrefabPathKey( a ) );
}

// --- The file half ----------------------------------------------------------------------------------

TEST( PrefabOverrides, OverridesSurviveTheFile )
{
    // The override is only worth anything if it survives JSON, because the file is the whole point: the
    // defect being fixed is that an edit did not survive a save and a load.
    EntityData live            = BaseRecord();
    live.Components["UIPanel"] = Payload( "Color", 0.90 );
    live.Translation           = glm::vec3( 5.0f, 6.0f, 7.0f );
    const auto over            = DiffPrefabEntity( BaseRecord(), live, { UUID( 4 ), UUID( 7001 ) } );
    ASSERT_TRUE( over.has_value() );

    Desert::Assets::PrefabData tree;
    EntityData                 instanceRoot;
    instanceRoot.id              = UUID( 900 );
    instanceRoot.PrefabPath      = "Resources/Assets/Prefabs/Button.deprefab";
    instanceRoot.PrefabOverrides = std::vector<PrefabOverrideData>{ *over };
    tree.Entities.push_back( instanceRoot );

    const std::string json   = rfl::json::write( tree );
    const auto        parsed = rfl::json::read<Desert::Assets::PrefabData>( json );
    ASSERT_TRUE( parsed ) << json;

    ASSERT_EQ( parsed.value().Entities.size(), 1u );
    const auto& back = parsed.value().Entities.front();
    ASSERT_TRUE( back.PrefabOverrides.has_value() );
    ASSERT_EQ( back.PrefabOverrides->size(), 1u );

    const PrefabOverrideData& round = back.PrefabOverrides->front();
    ASSERT_EQ( round.Path.size(), 2u );
    EXPECT_EQ( PrefabPathKey( round.Path ), PrefabPathKey( over->Path ) );
    ASSERT_TRUE( round.Translation.has_value() );
    EXPECT_EQ( round.Translation->z, 7.0f );
    ASSERT_TRUE( round.Components.get( "UIPanel" ).has_value() );
    EXPECT_EQ( Text( round.Components.get( "UIPanel" ).value() ), Text( Payload( "Color", 0.90 ) ) );
}
// NOLINTEND(bugprone-unchecked-optional-access)

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
