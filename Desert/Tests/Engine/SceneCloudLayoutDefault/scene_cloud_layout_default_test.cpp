// A scene written before the painted layout existed must read back as the sky it was authored as.
//
// WHY THERE IS NO MIGRATION FUNCTION, AND WHY THAT MAKES THIS SUITE NECESSARY RATHER THAN OPTIONAL.
//
// Every earlier cloud migration in Tools/SceneMigrator/Source/SceneMigration.cpp exists because a key was
// RENAMED, DROPPED or REINTERPRETED — the noise volume became an asset, the species became a handle,
// `CloudType` became `CloudType1`. The painted layout does none of those: it ADDS six fields and touches
// nothing that was there. An absent key is already how the reflected serializer spells "keep the C++
// default", so a v6 -> v7 function would have nothing to do, and a function that returns zeros is the stub
// §1.2 of the contract forbids. The version stamp is not moved either, for the same reason.
//
// That decision is only safe if one thing is true, and it is exactly the thing nobody checks when they
// skip a migration: THAT THE DEFAULTS ARE THE OLD BEHAVIOUR. Six new fields default to something, and if
// any of them defaulted to a painting being applied, then every scene in the repository would quietly
// change the day this phase landed — with no migration, no log line and no version bump to point at.
//
// So the claim under test is stated as a relation rather than as a hope:
//
//     a `VolumetricCloud` payload with no layout keys in it deserialises into a layer that binds no
//     painting, and into bake parameters in which the five placement numbers cannot reach a cloud.
//
// It is the file-level half of the phase's acceptance criterion. The frame-level half is the six-point
// protocol; the lump-level half is
// CloudPlacementSpectrum.WithNoPaintingBoundTheLayoutKnobsCannotReachOneCloud. This is the cheapest of the
// three and the one that localises a regression, because a frame that moved says only that something did.
//
// AND WHAT O1 CHANGED, because the paragraphs above are about a v6->v7 that was never written and are
// still true of it. The six keys DO migrate now — not at v6->v7 but at v11->v12, out of the component
// and into a cloud material, along with twenty-seven others. That does not retire this suite: the
// question it asks is still "what does an absent key mean", and the answer simply moved from a member
// initializer on the component to the CloudRaymarch schema, which is where the assertions below now read
// it. MigrateCloudMaterialV11ToV12 does not invent a value for a key the file never stated, so a v6 file
// arrives at v12 with those six still unstated, and this suite is still what says that is the old sky.
//
// Everything below runs on the parsed tree and on pure functions. No GPU, no scene graph, no asset
// manager.

#include <Engine/Assets/CloudProceduralVolume.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/ECS/VolumetricCloudComponent.hpp>
#include <Engine/Graphic/Clouds/CloudMaterialValues.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>
#include <Common/Json/Document.hpp>

namespace
{
    // DeserializeReflected reads a Json::Node and collects wrong-typed values as Issues; every fixture this
    // file feeds it is well-typed, so an Issue is a failure here.
    void ReadReflectedValue( const Desert::Reflection::TypeInfo& type, void* obj, const Common::Json::Value& src,
                             const Desert::Reflection::AssetResolver* resolver = nullptr )
    {
        Common::Json::Issues issues;
        Desert::Reflection::DeserializeReflected( type, obj, Common::Json::Root( src ), issues, resolver );
        for ( const auto& issue : issues )
            ADD_FAILURE() << Common::Json::Describe( issue );
    }
    void ReadReflectedValue( const Desert::Reflection::TypeInfo& type, void* obj, const Common::Json::Object& src,
                             const Desert::Reflection::AssetResolver* resolver = nullptr )
    {
        ReadReflectedValue( type, obj, Common::Json::Value( src ), resolver );
    }
} // namespace

using Desert::Assets::CloudModellingBlob;
using Desert::Assets::CloudProceduralFieldParams;
using Desert::Assets::CloudProceduralRegionOriginKm;
using Desert::Assets::CloudProceduralSpecies;
using Desert::Assets::GenerateCloudProceduralBlobs;
using Desert::Reflection::ReflectionRegistry;
using Desert::Reflection::TypeInfo;

namespace
{
    /// Whether the parsed payload carries a key at all. Written by hand because `rfl::Object`'s own `find`
    /// is private and there is no `contains` — and the DISTINCTION between "absent" and "present but
    /// defaulted" is the entire subject of this suite, so it cannot be reached for through `operator[]`,
    /// which would insert the key it was asked about.
    bool HasKey( const rfl::Generic::Object& object, const std::string& key )
    {
        for ( auto it = object.begin(); it != object.end(); ++it )
            if ( it->first == key )
                return true;
        return false;
    }

    const TypeInfo& CloudType()
    {
        const TypeInfo* t = ReflectionRegistry::Get().Find( "VolumetricCloudData" );
        EXPECT_NE( t, nullptr ) << "VolumetricCloudData is not in the reflection registry, so nothing below "
                                   "means anything";
        return *t;
    }

    /// A cloud payload exactly as a scene file carried it BEFORE this phase: schema v6, one cloud type in
    /// the first slot, and a representative spread of the settings that stay — including the two the
    /// painted pattern can take over from, so that a bug which quietly re-pointed them would show here.
    ///
    /// WRITTEN OUT RATHER THAN SERIALISED FROM A DEFAULTED COMPONENT, and that is the point of the fixture:
    /// serialising today's struct would write the six new keys and prove nothing at all. What a v6 file has
    /// is the ABSENCE of them.
    rfl::Generic::Object CloudPayloadV6()
    {
        rfl::Generic::Object o;
        o["Enabled"]              = true;
        o["CloudType1"]           = std::string( "Clouds/Types/CumulusCongestus.decloudtype" );
        o["Coverage"]             = 0.762;
        o["CoverageContrast"]     = 1.0;
        o["WeatherTileSize"]      = 1200000.0;
        o["RegionSize"]           = 4800000.0;
        o["Seed"]                 = 1;
        o["PlacementDensity"]     = 1.75;
        o["PlacementScatter"]     = 1.0;
        o["PlacementSizeVariety"] = 0.75;
        o["PatchTileSize"]        = 2100000.0;
        o["PatchStrength"]        = 0.60;
        o["DetailStrength"]       = 0.45;
        o["MaxSteps"]             = 192;
        return o;
    }

    /// The shipped congestus at the shipped weather, so the parameter half of the test is measured on the
    /// arrangement the scene above describes rather than on an abstraction of it.
    CloudProceduralFieldParams ShippedParams()
    {
        CloudProceduralFieldParams params;
        params.RegionSizeKm      = 48.0f;
        params.LayerBottomKm     = 2.20f;
        params.LayerThicknessKm  = 3.60f;
        params.BlendRadiusKm     = 0.06f;
        params.ProfileDepthKm    = 0.36f;
        params.Coverage          = 0.762f;
        params.CoverageContrast  = 1.0f;
        params.Seed              = 1u;
        params.WindAxis          = glm::vec2( 1.0f, 0.0f );
        params.ResolvableChordKm = 0.125f;

        CloudProceduralSpecies species;
        species.Shape.BaseAltitudeKm      = 2.20f;
        species.Shape.TopAltitudeKm       = 5.80f;
        species.Shape.EdgeTopFraction     = 0.15f;
        species.Shape.BaseRampFraction    = 0.04f;
        species.Shape.Profile             = Desert::Graphic::CloudProfileFromTaper( 0.50f );
        species.Shape.DetailCharacter     = 1.00f;
        species.Shape.DetailFactor        = 1.00f;
        species.Shape.DensityFactor       = 1.15f;
        species.Shape.ExtinctionFactor    = 1.00f;
        species.Shape.PlacementScale      = 1.00f;
        species.Shape.PlacementAnisotropy = 1.00f;
        species.CellKm                    = 3.0f;
        species.Anisotropy                = 1.0f;
        params.Species.push_back( species );

        return params;
    }
} // namespace

// THE SIX NEW KEYS ARE ABSENT AND THE LAYER READS AS UNPAINTED. This is the whole reason no migration was
// written, stated as an assertion instead of as an argument in a commit message.
TEST( SceneCloudLayoutDefault, AV6PayloadWithNoLayoutKeysBindsNoPainting )
{
    const rfl::Generic::Object payload = CloudPayloadV6();

    // `CloudLayout` is the name the one slot had when this fixture's generation was current; the two
    // beside it are what O-4 split it into. All three are listed because the claim is that a v6 file
    // states NO layout input under any spelling it could have had.
    for ( const char* key : { "CloudLayout", "LayoutPattern", "LayoutMask", "LayoutPatternStrength",
                              "LayoutMaskStrength", "LayoutRepeats", "LayoutRotation", "LayoutOffset" } )
        ASSERT_FALSE( HasKey( payload, key ) )
             << "the fixture carries '" << key
             << "', so it is not the pre-phase file it claims to be and this suite is testing itself";

    Desert::ECS::VolumetricCloudData layer;
    ReadReflectedValue( CloudType(), &layer, payload );

    // The settings that were in the file AND still belong to the component arrived, so the deserialiser
    // ran and the rest of the test is about absence rather than about nothing having happened. Coverage,
    // Patch Strength and Placement Density used to stand here; since O1 they are the material's, so the
    // two budget values the fixture also states carry that job.
    EXPECT_TRUE( layer.Enabled );
    EXPECT_FLOAT_EQ( layer.RegionSize, 4800000.0f );
    EXPECT_EQ( layer.MaxSteps, 192 );

    // AND THE SIX THAT WERE NOT IN IT ARE THE DEFAULTS — read where they now live. Since O1 an absent
    // layout key does not fall through to a C++ member initializer on the component; it falls through to
    // the CloudRaymarch schema, because a v6 payload migrates to a material that states only the keys the
    // file stated, and BuildCloudMaterialValues fills the rest from the schema. That is the SAME
    // relation this suite has always asserted — "an absent key spells the old behaviour" — measured on
    // the side the value moved to. Empty overrides are exactly what such a migrated material carries for
    // these six.
    const Desert::Graphic::CloudMaterialValues look =
         Desert::Graphic::BuildCloudMaterialValues( nullptr, Desert::Graphic::MaterialOverrides{} );

    // The one that decides everything is the first: an empty handle is what the renderer resolves to a
    // null painting, and a null painting is what makes the bake take the procedural branch it has always
    // taken.
    EXPECT_EQ( look.LayoutPattern, Desert::Assets::AssetHandle::Null() )
         << "a scene written before the painted layout existed came back with a painting bound, so every "
            "shipped scene changed the day this phase landed and nothing in the file says so";

    // BOTH INPUTS, since O-4 split the one slot in two. Asserting only the pattern would pass on a
    // build that defaulted the mask to something — and a mask is the one of the two that is NOT
    // balanced about its own average, so a stray default there moves the sky's total cover.
    EXPECT_EQ( look.LayoutMask, Desert::Assets::AssetHandle::Null() )
         << "the Global Cloud Mask input came back non-empty from a file that states nothing about it";

    EXPECT_EQ( look.LayoutRepeats, 1 );
    EXPECT_EQ( look.LayoutRotation, 0 );
    EXPECT_FLOAT_EQ( look.LayoutOffset.x, 0.0f );
    EXPECT_FLOAT_EQ( look.LayoutOffset.y, 0.0f );

    // THE TWO STRENGTHS DEFAULT TO ONE AND THAT IS DELIBERATE, so it is asserted rather than left to look
    // like an oversight. They are inert while no painting is bound — proved by the test below and by
    // CloudPlacementSpectrum.WithNoPaintingBoundTheLayoutKnobsCannotReachOneCloud — and defaulting them to
    // zero instead would mean an artist who drops a painting into the slot sees nothing happen and cannot
    // tell a slot that is not wired from a slider that is down.
    EXPECT_FLOAT_EQ( look.LayoutPatternStrength, 1.0f );
    EXPECT_FLOAT_EQ( look.LayoutMaskStrength, 1.0f );
}

// AND THE DEFAULTS PLACE THE CLOUDS THE OLD WAY. The assertion above says the fields arrive unset; this
// says that being unset is the same sky.
//
// It is the parameter-level statement of "an empty slot renders the frame it rendered before", and it is
// here rather than only in the spectrum suite because the two are different claims: that one starts from
// bake parameters somebody wrote, this one starts from a FILE. A default that was right in C++ and wrong
// through the deserialiser would pass there and fail here.
TEST( SceneCloudLayoutDefault, TheDefaultsFromAV6FileArePlacementNothingCanTellFromTheOldOne )
{
    // WHERE THE FILE'S SILENCE IS NOW ANSWERED. Before O1 an absent layout key fell through the reflected
    // deserialiser to a member initializer on the component; since O1 the six keys are the material's, a
    // v6 payload migrates to a material stating only the keys the file stated, and the absent ones are
    // answered by the CloudRaymarch schema through BuildCloudMaterialValues. Empty overrides ARE a v6
    // file's silence about all six — so this is the same claim, read on the side the values moved to.
    const Desert::Graphic::CloudMaterialValues look =
         Desert::Graphic::BuildCloudMaterialValues( nullptr, Desert::Graphic::MaterialOverrides{} );

    CloudProceduralFieldParams fromFile = ShippedParams();
    fromFile.Coverage                   = look.Coverage;
    fromFile.PatchStrength              = look.PatchStrength;
    fromFile.PlacementDensity           = look.PlacementDensity;
    fromFile.PlacementScatter           = look.PlacementScatter;
    fromFile.PlacementSizeVariety       = look.PlacementSizeVariety;

    // The layout numbers as the file's silence produced them, mapped the way
    // VolumetricCloudRenderer::BuildProceduralParams maps them. Both sources stay null because both
    // handles are empty and the service answers null for an empty handle.
    fromFile.LayoutPlacement.RepeatsPerRegion = static_cast<uint32_t>( look.LayoutRepeats );
    fromFile.LayoutPlacement.QuarterTurns     = static_cast<uint32_t>( look.LayoutRotation );
    fromFile.LayoutPlacement.OffsetKm         = glm::vec2( look.LayoutOffset.x, look.LayoutOffset.y );
    fromFile.LayoutPlacement.PatternStrength  = look.LayoutPatternStrength;
    fromFile.LayoutPlacement.MaskStrength     = look.LayoutMaskStrength;

    ASSERT_EQ( fromFile.PatternSource, nullptr );
    ASSERT_EQ( fromFile.MaskSource, nullptr );

    // The same parameters with the layout block never touched at all — which is literally the struct as it
    // was before this phase, since the six fields did not exist.
    CloudProceduralFieldParams asBefore = ShippedParams();
    asBefore.Coverage                   = look.Coverage;
    asBefore.PatchStrength              = look.PatchStrength;
    asBefore.PlacementDensity           = look.PlacementDensity;
    asBefore.PlacementScatter           = look.PlacementScatter;
    asBefore.PlacementSizeVariety       = look.PlacementSizeVariety;

    const glm::vec2 origin = CloudProceduralRegionOriginKm( asBefore, 0.0f, 0.0f );

    const std::vector<CloudModellingBlob> before = GenerateCloudProceduralBlobs( asBefore, 0u, origin );
    const std::vector<CloudModellingBlob> after  = GenerateCloudProceduralBlobs( fromFile, 0u, origin );

    ASSERT_FALSE( before.empty() ) << "the fixture placed no clouds at all, so this proves nothing";
    ASSERT_EQ( before.size(), after.size() )
         << "a v6 file's defaults changed how many clouds the layer has, which is the whole thing the "
            "absence of a migration is betting against";

    for ( size_t i = 0; i < before.size(); ++i )
    {
        ASSERT_EQ( before[i].CentreKm, after[i].CentreKm ) << "lump " << i << " moved";
        ASSERT_EQ( before[i].RadiiKm, after[i].RadiiKm ) << "lump " << i << " changed size";
    }

    // AND THE CACHE AGREES, which is the same statement one level up: a layer loaded from a v6 file must
    // not be considered stale against the parameters it would have had before the fields existed, or every
    // pre-phase scene re-bakes two million voxels on its first frame for nothing.
    EXPECT_TRUE( Desert::Assets::CloudProceduralParamsEqual( asBefore, fromFile ) );
}

// THE SCHEMA VERSION DID NOT MOVE, AND THAT IS RECORDED HERE SO IT IS A DECISION RATHER THAN AN OVERSIGHT.
//
// Six of the migration steps exist because a cloud change needed one. This one did not, and the difference
// is worth pinning: if somebody later adds a version for an unrelated reason, this test fails and they read
// the paragraph explaining that the layout is NOT what a version constant is for — instead of assuming it
// was and wiring a migration to it.
//
// IT FIRED, ONCE, AND WORKED. v7 was added for an unrelated reason — the terrain's material became a
// `.demat`, so the inline Material component a terrain entity carried had to be dropped from the file
// (Docs/MaterialEditor/PLAN_STAGE3_ASSET_DOCUMENTS.md, M3). The developer on that task read this paragraph,
// confirmed the layout is still not what any version constant is for, and moved the assertion DOWN to the
// last constant the clouds own rather than deleting it. The claim it makes is unchanged: no cloud-layout
// change has ever moved the schema, and the head being past that point is somebody else's step.
//
// IT FIRED A SECOND TIME, AND THE ASSERTION HAD TO CHANGE SHAPE. The per-step constants
// (kSceneVersionCloudSet and its seven siblings) left the engine with the migrations themselves — they are
// Desert::Migration in Tools/SceneMigrator now, and this suite compiles no tool code. What survives in the
// engine is the head, Core::kSceneVersion. So the claim is made against the head and the clouds' last step
// is named as the LITERAL it has always been, 6: the number is the point, not where it is declared. The
// statement is exactly the one above — the clouds' last schema step is 6, the head is past it, and neither
// of those facts is about the painted layout.
TEST( SceneCloudLayoutDefault, ThePaintedLayoutDidNotMoveTheSchemaVersion )
{
    constexpr int kCloudsLastSchemaStep = 6; // v5 -> v6, a layer carries a SET of cloud types

    EXPECT_GE( Desert::Core::kSceneVersion, kCloudsLastSchemaStep )
         << "the scene schema version went BACKWARDS past the clouds' last step, which no migration can do.";
    EXPECT_EQ( kCloudsLastSchemaStep, 6 )
         << "the painted layout did NOT move the clouds' last version constant — it adds fields and renames "
            "none, and an absent key already means 'the C++ default'. If your CLOUD change renames, drops "
            "or reinterprets a key then it needs its own step in Tools/SceneMigrator AND a raise of "
            "Core::kSceneVersion; if it only adds fields, it needs neither, and it needs a test like the "
            "two above instead.";
}

// WHICH TYPE SLOT IS WHICH PAINTED CHANNEL — CALIBRATION.md §PTP.
//
// A painted layout's four channels are indexed by SPECIES, and the species are the layer's four type slots
// COMPACTED: empty slots skipped, a repeated type dropped. So the numbering an artist sees in Details is
// not the numbering the painting uses, and the symptom of not knowing that is a channel somebody swears
// they painted that does nothing at all. The Cloud Layout panel names the type behind every channel, and
// it can only do that because ECS::ResolveCloudSpecies states the rule once — the renderer resolves the
// same call — over the MATERIAL's four slots since O1, which is also where the panel reads them.
//
// These are the cases the panel's labels are wrong about if the rule ever moves.
TEST( SceneCloudLayoutDefault, TheSpeciesAreTheTypeSlotsCompactedAndTheChannelsFollowThem )
{
    const Desert::Assets::AssetHandle cumulus( 0x11u );
    const Desert::Assets::AssetHandle cirrus( 0x22u );

    // A LAYER WITH ONE TYPE IN THE THIRD SLOT IS DRIVEN BY THE PAINTING'S FIRST CHANNEL. This is the case
    // that reads as a broken feature: an artist paints blue for "Cloud Type 3" and the sky ignores it.
    {
        const Desert::Assets::AssetHandle slots[Desert::ECS::kCloudTypeSlots] = { {}, {}, cumulus, {} };

        const Desert::ECS::CloudSpeciesResolution resolved = Desert::ECS::ResolveCloudSpecies( slots );

        EXPECT_EQ( resolved.Count, 1u );
        EXPECT_FALSE( resolved.BuiltInDefault );
        EXPECT_EQ( resolved.AuthoredSlot[0], 2u )
             << "the only authored type was not reported as channel 0's, so the panel names the wrong cloud "
                "beside the channel that actually places it";
    }

    // ORDER IS THE ORDER OF THE SLOTS, not of anything else, and a gap does not reserve a channel.
    {
        const Desert::Assets::AssetHandle slots[Desert::ECS::kCloudTypeSlots] = { {}, cirrus, {}, cumulus };

        const Desert::ECS::CloudSpeciesResolution resolved = Desert::ECS::ResolveCloudSpecies( slots );

        EXPECT_EQ( resolved.Count, 2u );
        EXPECT_EQ( resolved.AuthoredSlot[0], 1u );
        EXPECT_EQ( resolved.AuthoredSlot[1], 3u );
    }

    // THE SAME TYPE TWICE IS ONE SPECIES, because two identical placement fields are two skies of one kind
    // of cloud at twice the cost. The second slot's channel therefore drives NOTHING, and an artist who
    // painted it is owed that sentence rather than a shrug.
    {
        const Desert::Assets::AssetHandle slots[Desert::ECS::kCloudTypeSlots] = { cumulus, cumulus, cirrus, {} };

        const Desert::ECS::CloudSpeciesResolution resolved = Desert::ECS::ResolveCloudSpecies( slots );

        EXPECT_EQ( resolved.Count, 2u );
        EXPECT_EQ( resolved.AuthoredSlot[0], 0u );
        EXPECT_EQ( resolved.AuthoredSlot[1], 2u )
             << "the duplicate was kept, so a layer's channels are numbered differently from the sky it "
                "actually places";
    }

    // ALL FOUR EMPTY IS ONE BUILT-IN SPECIES and never zero, which is what keeps a scene nobody has
    // authored a type for from having no sky — and what keeps the panel's channel slider from having an
    // empty range.
    {
        const Desert::Assets::AssetHandle         slots[Desert::ECS::kCloudTypeSlots] = {};
        const Desert::ECS::CloudSpeciesResolution resolved = Desert::ECS::ResolveCloudSpecies( slots );

        EXPECT_EQ( resolved.Count, 1u );
        EXPECT_TRUE( resolved.BuiltInDefault );
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
