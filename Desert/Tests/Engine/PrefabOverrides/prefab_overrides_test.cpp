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

#include <Common/Json/Json.hpp>
#include <gtest/gtest.h>

#include <Engine/Assets/Prefab/PrefabOverrides.hpp>

#include <string>
#include <vector>

using Common::UUID;
using Desert::Assets::DiffPayload;
using Desert::Assets::DiffPrefabEntity;
using Desert::Assets::EntityData;
using Desert::Assets::LayerOverrideOntoRecord;
using Desert::Assets::MergePayload;
using Desert::Assets::OverrideMetaAsEntityData;
using Desert::Assets::PrefabDiffReport;
using Desert::Assets::PrefabOverrideData;
using Desert::Assets::PrefabPathKey;

namespace
{
    // A component payload, as a component payload actually reaches these functions: an Common::Json::Value
    // object tree, the same thing ComponentRegistry hands EntityData.
    Common::Json::Value Payload( const std::string& key, double value )
    {
        Common::Json::Object object;
        object[key] = Common::Json::Value( value );
        return { object };
    }

    // A component payload with MORE THAN ONE FIELD. Ю20 is entirely about what happens to the other
    // fields when one of them is overridden, and a one-field fixture cannot tell the two granularities
    // apart — which is why the suite could be green through the whole of Ю19 while the defect was there.
    Common::Json::Value Payload2( const std::string& a, double av, const std::string& b, double bv )
    {
        Common::Json::Object object;
        object[a] = Common::Json::Value( av );
        object[b] = Common::Json::Value( bv );
        return { object };
    }

    // One field of a payload, as text, so a test can say which field moved and which did not.
    std::string FieldOf( const Common::Json::Value& payload, const std::string& field )
    {
        const auto object = payload.to_object();
        if ( !object )
        {
            return "<not an object>";
        }
        const auto value = object.value().get( field );
        if ( !value )
        {
            return "<absent>";
        }
        return Common::Json::Write( value.value() );
    }

    std::string Text( const Common::Json::Value& g )
    {
        return Common::Json::Write( g );
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
        data.Components["UIPanel"]  = Payload2( "Color", 0.25, "CornerRadius", 8.0 );
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

TEST( PrefabOverrides, OnlyTheEditedFieldIsRecorded )
{
    const EntityData base = BaseRecord();
    EntityData       live = BaseRecord();
    // The user recoloured the panel and touched nothing else — not its corner radius, not its layout.
    live.Components["UIPanel"] = Payload2( "Color", 0.90, "CornerRadius", 8.0 );

    const auto over = DiffPrefabEntity( base, live, { UUID( 7001 ) } );
    ASSERT_TRUE( over.has_value() );

    EXPECT_EQ( over->Components.size(), 1u );
    ASSERT_TRUE( over->Components.get( "UIPanel" ).has_value() );

    // Ю20: the unit is the FIELD. Recording the whole UIPanel payload here is what made an overridden
    // instance stop following its prefab for every other field of that component — the same silent loss
    // of the link that Ю19 fixed one level up, and the reason #146 was filed.
    // BY VALUE. `Result::value()` hands back the temporary Result's contents, so binding a reference to
    // it dangles the moment the full expression ends — which is a crash inside the variant, not a test
    // failure, and it cost a debugging round here.
    const Common::Json::Value panel = over->Components.get( "UIPanel" ).value();
    EXPECT_EQ( FieldOf( panel, "Color" ), "0.9" );
    EXPECT_EQ( FieldOf( panel, "CornerRadius" ), "<absent>" )
         << "the corner radius was not touched, so the override must not state it";

    // And the untouched COMPONENT is still not mentioned at all — the Ю19 property, unchanged.
    EXPECT_FALSE( over->Components.get( "UILayout" ).has_value() );
    EXPECT_FALSE( over->Tag.has_value() );
    EXPECT_FALSE( over->Translation.has_value() );
    EXPECT_FALSE( over->Rotation.has_value() );
    EXPECT_FALSE( over->Scale.has_value() );
}

TEST( PrefabOverrides, SourceEditsReachAnOverriddenInstance )
{
    // 1. An instance is recoloured, and nothing else about it is touched.
    const EntityData base      = BaseRecord();
    EntityData       live      = BaseRecord();
    live.Components["UIPanel"] = Payload2( "Color", 0.90, "CornerRadius", 8.0 );
    const auto over            = DiffPrefabEntity( base, live, { UUID( 7001 ) } );
    ASSERT_TRUE( over.has_value() );

    // 2. The PREFAB is then edited in three places: another component, this component's OTHER FIELD, and
    //    the very field the instance overrode.
    EntityData newBase             = BaseRecord();
    newBase.Components["UILayout"] = Payload( "OffsetMinX", 64.0 );
    newBase.Components["UIPanel"]  = Payload2( "Color", 0.10, "CornerRadius", 24.0 );
    newBase.Tag                    = "Primary Button";

    // 3. The instance is rebuilt from the new source with its override on top.
    EntityData result = newBase;
    LayerOverrideOntoRecord( result, *over );

    // The untouched COMPONENT followed the source — true since Ю19.
    EXPECT_EQ( Text( result.Components.get( "UILayout" ).value() ), Text( Payload( "OffsetMinX", 64.0 ) ) );
    EXPECT_EQ( result.Tag, "Primary Button" );

    const Common::Json::Value panel = result.Components.get( "UIPanel" ).value();
    // THE SIBLING FIELD FOLLOWED THE SOURCE TOO — this is Ю20, and under component-sized overrides this
    // line read 8 instead of 24: the author who recoloured one button stopped receiving every later edit
    // to that button's shape, silently.
    EXPECT_EQ( FieldOf( panel, "CornerRadius" ), "24.0" );
    // ... and the field that was actually overridden did not.
    EXPECT_EQ( FieldOf( panel, "Color" ), "0.9" );
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

TEST( PrefabOverrides, RecordApplierAndLiveApplierCarryTheSameValues )
{
    const EntityData base      = BaseRecord();
    EntityData       live      = BaseRecord();
    live.Tag                   = "Quit Button";
    live.Components["UIPanel"] = Payload2( "Color", 0.5, "CornerRadius", 8.0 );
    const auto over            = DiffPrefabEntity( base, live, { UUID( 7001 ) } );
    ASSERT_TRUE( over.has_value() );

    // TWO APPLIERS, ONE MEANING. One lays the override over a RECORD (the base a nested prefab's next
    // diff is taken against); the other lays it over what a LIVE component currently holds, which
    // PrefabFactory reads back through the component's own serializer. Both go through MergePayload, and
    // this is the assertion that keeps them there — two ways to apply one thing is the defect shape this
    // project pays for most often.
    EntityData layered = BaseRecord();
    LayerOverrideOntoRecord( layered, *over );

    const Common::Json::Value liveSide = MergePayload( BaseRecord().Components.get( "UIPanel" ).value(),
                                                       over->Components.get( "UIPanel" ).value() );

    EXPECT_EQ( Text( layered.Components.get( "UIPanel" ).value() ), Text( liveSide ) );

    // The META form carries the tag and no components at all: a partial payload handed to the entity
    // deserializer would reset every field it does not mention, which is the whole reason components
    // take the merge path instead.
    const EntityData meta = OverrideMetaAsEntityData( *over );
    EXPECT_EQ( meta.Tag, over->Tag );
    EXPECT_EQ( meta.Components.size(), 0u );
    EXPECT_FALSE( meta.Translation.has_value() );
}

// --- The merge, on its own --------------------------------------------------------------------------

TEST( PrefabOverrides, MergeKeepsEveryFieldThePartialDoesNotMention )
{
    const Common::Json::Value current = Payload2( "Color", 0.25, "CornerRadius", 8.0 );
    const Common::Json::Value partial = Payload( "Color", 0.90 );

    const Common::Json::Value merged = MergePayload( current, partial );
    EXPECT_EQ( FieldOf( merged, "Color" ), "0.9" );
    EXPECT_EQ( FieldOf( merged, "CornerRadius" ), "8.0" )
         << "a field the override does not mention must keep the value it had — without this a "
            "field-level override would reset the rest of the component to whatever the caller passed in";
}

TEST( PrefabOverrides, AComponentTheInstanceAddedIsRecordedWhole )
{
    const EntityData base = BaseRecord();
    EntityData       live = BaseRecord();
    // The prefab has no UITween at all; the instance grew one.
    live.Components["UITween"] = Payload2( "Duration", 0.4, "Loop", 1.0 );

    const auto over = DiffPrefabEntity( base, live, { UUID( 7001 ) } );
    ASSERT_TRUE( over.has_value() );
    ASSERT_TRUE( over->Components.get( "UITween" ).has_value() );

    // There is nothing to merge onto, so the whole payload IS the difference — the one case where a
    // component-sized override remains the right answer. Recording only "the fields that differ from
    // nothing" would leave the rest at the component's defaults on the next load.
    EXPECT_EQ( Text( over->Components.get( "UITween" ).value() ),
               Text( Payload2( "Duration", 0.4, "Loop", 1.0 ) ) );
}

TEST( PrefabOverrides, APayloadThatIsNotAnObjectIsComparedAndMergedWhole )
{
    // Not every component payload is an object — one serialized as an array or a scalar has no fields to
    // take apart, and inventing sub-structure there would be a second definition of "field".
    const Common::Json::Value before(
         Common::Json::Value::Array{ Common::Json::Value( 1.0 ), Common::Json::Value( 2.0 ) } );
    const Common::Json::Value after(
         Common::Json::Value::Array{ Common::Json::Value( 1.0 ), Common::Json::Value( 9.0 ) } );

    EXPECT_FALSE( DiffPayload( before, before ).has_value() );
    const auto diff = DiffPayload( before, after );
    ASSERT_TRUE( diff.has_value() );
    EXPECT_EQ( Text( *diff ), Text( after ) );
    EXPECT_EQ( Text( MergePayload( before, *diff ) ), Text( after ) );
}

// --- #148: a field the FILE does not state ----------------------------------------------------------

TEST( PrefabOverrides, AFieldThePrefabDoesNotStateIsPinnedAndCounted )
{
    // A hand-edited `.deprefab` naming one field of two. The engine's own writer never produces this —
    // reflection emits every field — so it means the file was written by something else.
    EntityData base            = BaseRecord();
    base.Components["UIPanel"] = Payload( "Color", 0.25 );

    EntityData live            = BaseRecord();
    live.Components["UIPanel"] = Payload2( "Color", 0.25, "CornerRadius", 8.0 );

    PrefabDiffReport report;
    const auto       over = DiffPrefabEntity( base, live, { UUID( 7001 ) }, &report );

    // RECORDED, not skipped: the file cannot say whether 8.0 is the default the instance was born with
    // or a value its author chose, and dropping it would lose a real edit. Pinning it costs the link to
    // the source for that one field, which is the lesser loss — and it is COUNTED, so the caller can
    // name it instead of leaving it silent. (#148: normalising the base would remove the choice.)
    ASSERT_TRUE( over.has_value() );
    const Common::Json::Value pinned = over->Components.get( "UIPanel" ).value();
    EXPECT_EQ( FieldOf( pinned, "CornerRadius" ), "8.0" );
    EXPECT_EQ( FieldOf( pinned, "Color" ), "<absent>" );
    EXPECT_EQ( report.UnstatedFields, 1u );
}

TEST( PrefabOverrides, AFullyStatedPrefabCountsNoUnstatedFields )
{
    // The negative control for the line above: for a file this engine wrote, the counter is zero, so a
    // non-zero one always means "that file was written by something else".
    EntityData live            = BaseRecord();
    live.Components["UIPanel"] = Payload2( "Color", 0.90, "CornerRadius", 8.0 );

    PrefabDiffReport report;
    (void)DiffPrefabEntity( BaseRecord(), live, { UUID( 7001 ) }, &report );
    EXPECT_EQ( report.UnstatedFields, 0u );
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
    live.Components["UIPanel"] = Payload2( "Color", 0.90, "CornerRadius", 8.0 );
    live.Translation           = glm::vec3( 5.0f, 6.0f, 7.0f );
    const auto over            = DiffPrefabEntity( BaseRecord(), live, { UUID( 4 ), UUID( 7001 ) } );
    ASSERT_TRUE( over.has_value() );

    Desert::Assets::PrefabData tree;
    EntityData                 instanceRoot;
    instanceRoot.id              = UUID( 900 );
    instanceRoot.PrefabPath      = "Resources/Assets/Prefabs/Button.deprefab";
    instanceRoot.PrefabOverrides = std::vector<PrefabOverrideData>{ *over };
    tree.Entities.push_back( instanceRoot );

    const std::string json   = Common::Json::Write( tree );
    const auto        parsed = Common::Json::Read<Desert::Assets::PrefabData>( json );
    ASSERT_TRUE( parsed ) << json;

    ASSERT_EQ( parsed.GetValue().Entities.size(), 1u );
    const auto& back = parsed.GetValue().Entities.front();
    ASSERT_TRUE( back.PrefabOverrides.has_value() );
    ASSERT_EQ( back.PrefabOverrides->size(), 1u );

    const PrefabOverrideData& round = back.PrefabOverrides->front();
    ASSERT_EQ( round.Path.size(), 2u );
    EXPECT_EQ( PrefabPathKey( round.Path ), PrefabPathKey( over->Path ) );
    ASSERT_TRUE( round.Translation.has_value() );
    EXPECT_EQ( round.Translation->z, 7.0f );
    ASSERT_TRUE( round.Components.get( "UIPanel" ).has_value() );
    // A FIELD-level payload has to survive the file just as a component-level one did: what is written
    // is `{"Color":0.9}` and nothing else, and it has to come back as exactly that or the merge on the
    // other side would restore a field nobody overrode.
    EXPECT_EQ( Text( round.Components.get( "UIPanel" ).value() ), Text( Payload( "Color", 0.90 ) ) );
}
// NOLINTEND(bugprone-unchecked-optional-access)

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
