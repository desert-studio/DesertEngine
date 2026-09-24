// A BLOCK SHORT OF ONE FIELD USED TO COST THE ENTITY THE WHOLE COMPONENT, IN SILENCE.
//
// Eight components in ComponentRegistry.cpp travel as a reflect-cpp mirror struct. The read was
// `rfl::json::read<T>` with no processor, and reflect-cpp refuses a MISSING field even when the struct
// declares a default for it — so `if ( !parsed.has_value() ) return;`, written eight times with no log
// line between them, turned "this file predates one field" into "this entity has no AnimationComponent".
//
// That contradicted the rule ForeignKeys.hpp states and builds its whole preservation argument on: "a
// field ADDED needs no version bump, a missing key already defaults". For these eight it did not
// default, it deleted. Both files described the same load and only one of them was right.
//
// These are the assertions that hold the fix down, on the REAL mirror structs rather than a synthetic
// one — the synthetic version of this test would have passed on the old code just as happily, because
// what makes it real is that `AnimationComponentSer` has six non-optional fields and `Text` has six.

#include <gtest/gtest.h>

#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Core/Serialize/GenericBlock.hpp>
// PrimitiveType, which an ISM block states instead of a mesh path.
#include <Engine/Geometry/PrimitiveType.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using Desert::Core::Serialize::ReadBlock;
using Desert::Core::Serialize::WriteBlock;

namespace Assets = Desert::Assets;

namespace
{
    rfl::Generic FromJsonText( const std::string& text )
    {
        auto parsed = rfl::json::read<rfl::Generic>( text );
        EXPECT_TRUE( parsed.has_value() ) << "the test's own fixture is not valid JSON: " << text;
        return parsed.has_value() ? parsed.value() : rfl::Generic( rfl::Generic::Object{} );
    }
} // namespace

// ── THE DEFECT ─────────────────────────────────────────────────────────────────────────────────────
//
// The exact block an older build wrote: a field was added after it. Before the fix this returned
// nothing and the caller dropped AnimationComponent off the entity.
//
// The field in question WAS `GraphJson`, which schema step 21 retired — the graph is a `.danimgraph`
// named by `Graph` now. The fixtures keep the retired spelling on purpose: a key this build does not
// declare is exactly what the second test below is about, and the case is only real if something in
// this file still writes one.
TEST( GenericBlockRead, AnAnimationBlockMissingTwoFieldsKeepsTheComponentAndDefaultsThem )
{
    const auto older = FromJsonText( R"({"CurrentClip":"Run","Playing":true,"Loop":false,"PlaybackSpeed":2.5})" );

    const auto parsed = ReadBlock<Assets::AnimationComponentSer>( older, "Animation" );

    ASSERT_TRUE( parsed.has_value() )
         << "one absent field cost the entity its whole AnimationComponent — the character stops being "
            "animated and nothing in the log says why";
    EXPECT_EQ( parsed.value().CurrentClip, "Run" ) << "a field that WAS present did not survive";
    EXPECT_FLOAT_EQ( parsed.value().PlaybackSpeed, 2.5f );
    EXPECT_FALSE( parsed.value().Loop );
    // The absent one takes the struct's own default, which is what every call site already believed.
    EXPECT_FALSE( parsed.value().Graph.has_value() )
         << "an absent graph key came back as something other than 'no graph' — absence is how this "
            "format says an entity has no state machine";
}

TEST( GenericBlockRead, ATextBlockMissingEverythingButItsTextIsStillAText )
{
    const auto older = FromJsonText( R"({"Text":"Press Start"})" );

    const auto parsed = ReadBlock<Assets::TextComponentSer>( older, "Text" );

    ASSERT_TRUE( parsed.has_value() ) << "a text element lost its text because it lacked five other keys";
    EXPECT_EQ( parsed.value().Text, "Press Start" );
    EXPECT_FLOAT_EQ( parsed.value().Size, 1.0f );
    EXPECT_FALSE( parsed.value().Billboard );
}

TEST( GenericBlockRead, AUIAnimBlockMissingItsPlaybackFlagsKeepsItsTracks )
{
    const auto older = FromJsonText( R"({"Tracks":[],"Duration":4.0})" );

    const auto parsed = ReadBlock<Assets::UIAnimComponentSer>( older, "UIAnim" );

    ASSERT_TRUE( parsed.has_value() );
    EXPECT_FLOAT_EQ( parsed.value().Duration, 4.0f );
    EXPECT_FALSE( parsed.value().Loop );
    EXPECT_TRUE( parsed.value().Playing );
}

// ── THE DIRECTION THAT WAS ALREADY SAFE, PINNED SO IT STAYS SAFE ───────────────────────────────────
//
// An older build opening a NEWER build's scene meets keys it has never heard of. That has to keep
// working: it is half of what ForeignKeys.hpp exists to protect, and a processor that tightened this
// direction while loosening the other would be a straight trade rather than a fix.
// `EnableRootMotion` is now one of those keys FOR REAL and not only in a fixture: it was removed from
// AnimationComponentSer when it turned out nothing read it, so every scene written before that carries a
// key this build does not declare. Keeping it in this fixture is the removal's own regression guard.
TEST( GenericBlockRead, AnUnknownExtraFieldDoesNotRefuseTheBlock )
{
    const auto newer =
         FromJsonText( R"({"CurrentClip":"Run","Playing":true,"Loop":true,"PlaybackSpeed":1.0,)"
                       R"("EnableRootMotion":false,"GraphJson":"","SomethingAFutureBuildAdded":7})" );

    const auto parsed = ReadBlock<Assets::AnimationComponentSer>( newer, "Animation" );

    ASSERT_TRUE( parsed.has_value() )
         << "a key this build does not declare made it refuse the whole block, so an older worktree "
            "can no longer open a scene a newer one saved";
    EXPECT_EQ( parsed.value().CurrentClip, "Run" );
}

// ── WHAT MUST STILL BE REFUSED ─────────────────────────────────────────────────────────────────────
//
// `DefaultIfMissing` is not `accept anything`. A field of the wrong type is not a version difference,
// it is a corrupt or hand-broken file, and reading it as a default would be the silent substitution
// the contract forbids. It refuses, and the refusal is logged with the component's key by ReadBlock.
TEST( GenericBlockRead, AFieldOfTheWrongTypeIsStillRefused )
{
    const auto broken = FromJsonText( R"({"CurrentClip":"Run","Playing":"yes","Loop":true,"PlaybackSpeed":1.0,)"
                                      R"("EnableRootMotion":false,"GraphJson":""})" );

    EXPECT_FALSE( ReadBlock<Assets::AnimationComponentSer>( broken, "Animation" ).has_value() )
         << "a string where a boolean belongs was accepted — a malformed block is now indistinguishable "
            "from an old one";
}

// ── AND THE WHOLE TRIP, SO THE TWO HALVES ARE KNOWN TO AGREE ───────────────────────────────────────
TEST( GenericBlockRead, WhatWriteBlockWritesIsWhatReadBlockReads )
{
    Assets::AnimationComponentSer written;
    written.CurrentClip   = "Anim_Idle";
    written.Playing       = false;
    written.Loop          = false;
    written.PlaybackSpeed = 0.25f;
    written.Graph         = "AnimGraphs/Probe.danimgraph";

    const auto parsed =
         ReadBlock<Assets::AnimationComponentSer>( WriteBlock( written, "Animation" ), "Animation" );

    ASSERT_TRUE( parsed.has_value() );
    EXPECT_EQ( parsed.value().CurrentClip, written.CurrentClip );
    EXPECT_EQ( parsed.value().Playing, written.Playing );
    EXPECT_EQ( parsed.value().Loop, written.Loop );
    EXPECT_FLOAT_EQ( parsed.value().PlaybackSpeed, written.PlaybackSpeed );
    EXPECT_EQ( parsed.value().Graph, written.Graph );
}

// ── THE INSTANCED STATIC MESH, END TO END THROUGH THE BLOCK A .desce CARRIES ───────────────────────
//
// WHY THIS IS HERE AND WHAT IT DOES *NOT* PROVE. Nine instances authored in a scene file did not appear
// on screen while a plain StaticMesh beside them, at the same matrices, did (Г25's control frame). Two
// halves could have lost them — the file->component trip, and the component->frame trip — and the honest
// thing is to test the one that can be tested purely and to say where the defect actually was. It was
// the SECOND half: MeshRenderer gated its instanced queue on `!m_DeferredGeometry`, so in the deferred
// path the queue was never drained and no per-object path existed to catch it. That is fixed by the
// (Instanced x GBuffer) cell, and it is pinned by Desert/Tests/Engine/MeshVertexPath, not here.
//
// The trip below is the OTHER half, and it was never covered by anything: an ISM block is the only
// component payload in this engine that carries a `std::vector<std::array<float, 16>>`, i.e. a nesting
// depth no other mirror struct exercises, plus an optional enum. A silent loss there would look exactly
// like the defect that was actually found, which is why "the ends look right" is not an argument.

namespace
{
    constexpr std::size_t kMatrixElements = 16; // a mat4, flattened column-major, as a .desce stores it
    constexpr std::size_t kTranslationX   = 12; // ...so the translation is elements 12, 13, 14
    constexpr std::size_t kTranslationY   = 13;
    constexpr std::size_t kTranslationZ   = 14;

    // Two arbitrary but DISTINCT 64-bit handles: the point of the assertion is that each comes back as
    // itself, so the values only have to be recognisable and different from one another.
    constexpr uint64_t kMeshGuid     = 0x0123456789ABCDEFULL;
    constexpr const char* kMaterialGuid = "fedcba98765432100123456789abcdef"; // header GUID text (SCNE 27)

    constexpr float kGridStep  = 200.0F; // the 3x3 grid Г25's control frame used, in world units (cm)
    constexpr float kGridStart = 200.0F;
    constexpr int   kGridSide  = 3;

    // The diagonal of a column-major mat4: elements 0, 5, 10 and 15.
    constexpr std::array<std::size_t, 4> kDiagonal = { 0, 5, 10, 15 };

    std::array<float, kMatrixElements> Identity()
    {
        std::array<float, kMatrixElements> mat{};
        for ( const std::size_t element : kDiagonal )
        {
            mat.at( element ) = 1.0F;
        }
        return mat;
    }

    // Nine instances on a 3x3 grid. Every translation is distinct on purpose: a trip that returned nine
    // copies of the first matrix would satisfy a size check and lose eight instances.
    std::vector<std::array<float, kMatrixElements>> NineDistinctInstances()
    {
        std::vector<std::array<float, kMatrixElements>> flat;
        for ( int col = 0; col < kGridSide; ++col )
        {
            for ( int row = 0; row < kGridSide; ++row )
            {
                std::array<float, kMatrixElements> mat = Identity();
                mat.at( kTranslationX )                = kGridStart + ( static_cast<float>( col ) * kGridStep );
                mat.at( kTranslationY )                = -kGridStart + ( static_cast<float>( row ) * kGridStep );
                mat.at( kTranslationZ )                = static_cast<float>( ( col * kGridSide ) + row );
                flat.push_back( mat );
            }
        }
        return flat;
    }
    // Element by element, because a size check passes on nine copies of one matrix. Out of the TEST body
    // so the body stays one statement per claim.
    void ExpectSameMatrices( const std::vector<std::array<float, kMatrixElements>>& back,
                             const std::vector<std::array<float, kMatrixElements>>& expected )
    {
        ASSERT_EQ( back.size(), expected.size() );
        for ( std::size_t instance = 0; instance < expected.size(); ++instance )
        {
            for ( std::size_t element = 0; element < kMatrixElements; ++element )
            {
                EXPECT_FLOAT_EQ( back.at( instance ).at( element ), expected.at( instance ).at( element ) )
                     << "instance " << instance << ", element " << element;
            }
        }
    }
} // namespace

TEST( GenericBlockRead, AnInstancedStaticMeshSurvivesTheTripWithEveryMatrixIntact )
{
    const std::vector<std::array<float, kMatrixElements>> flat = NineDistinctInstances();

    Assets::InstancedStaticMeshComponentSer written;
    written.Primitive          = Desert::Geometry::PrimitiveType::Cube;
    written.InstanceTransforms = flat;

    const auto parsed = ReadBlock<Assets::InstancedStaticMeshComponentSer>(
         WriteBlock( written, "InstancedStaticMesh" ), "InstancedStaticMesh" );
    ASSERT_TRUE( parsed.has_value() );
    const Assets::InstancedStaticMeshComponentSer& read =
         parsed.value(); // NOLINT(bugprone-unchecked-optional-access)

    ASSERT_TRUE( read.Primitive.has_value() );
    EXPECT_EQ( read.Primitive, Desert::Geometry::PrimitiveType::Cube );

    ASSERT_TRUE( read.InstanceTransforms.has_value() );
    const std::vector<std::array<float, kMatrixElements>>& back =
         read.InstanceTransforms.value(); // NOLINT(bugprone-unchecked-optional-access)
    ExpectSameMatrices( back, flat );
}

// The shape a real `.desce` states, read as text rather than round-tripped — because a round trip can
// agree with itself while disagreeing with the files on disk. This is the block
// Editor/Resources/Assets/Scenes/G26_ISMProbe.desce carries under its "InstancedStaticMesh" key, cut to
// two instances. (ReadBlock is handed the block's CONTENTS; EntitySerializer looks the key up.)
TEST( GenericBlockRead, TheBlockShapeASceneFileStatesIsTheOneThisStructReads )
{
    const auto parsed = ReadBlock<Assets::InstancedStaticMeshComponentSer>(
         FromJsonText( R"({"Primitive":"Cube","InstanceTransforms":[)"
                       R"([1.0,0.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,0.0,1.0,0.0,200.0,-200.0,0.0,1.0],)"
                       R"([1.0,0.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,0.0,1.0,0.0,400.0,0.0,0.0,1.0]]})" ),
         "InstancedStaticMesh" );
    ASSERT_TRUE( parsed.has_value() );
    const Assets::InstancedStaticMeshComponentSer& read =
         parsed.value(); // NOLINT(bugprone-unchecked-optional-access)

    ASSERT_TRUE( read.Primitive.has_value() );
    EXPECT_EQ( read.Primitive, Desert::Geometry::PrimitiveType::Cube );

    ASSERT_TRUE( read.InstanceTransforms.has_value() );
    const std::vector<std::array<float, kMatrixElements>>& back =
         read.InstanceTransforms.value(); // NOLINT(bugprone-unchecked-optional-access)
    ASSERT_EQ( back.size(), 2U );
    EXPECT_FLOAT_EQ( back.at( 0 ).at( kTranslationX ), 200.0F );
    EXPECT_FLOAT_EQ( back.at( 1 ).at( kTranslationX ), 400.0F );
}

// A mesh-asset ISM states no Primitive at all, and a component whose optional mesh path is the only
// thing set must come back with its transforms — the case grass-as-an-asset takes. This is also the
// BACK-COMPATIBLE half of the GUID change: a block written before Г26 carries paths and no GUIDs, and
// has to keep resolving.
TEST( GenericBlockRead, AnAssetBackedInstancedStaticMeshKeepsItsTransformsWithNoPrimitive )
{
    const auto parsed = ReadBlock<Assets::InstancedStaticMeshComponentSer>(
         FromJsonText( R"({"MeshPath":"Cooked/Meshes/Grass.stmesh",)"
                       R"("MaterialPaths":["Materials/M_Grass.demat"],"InstanceTransforms":[)"
                       R"([1.0,0.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,0.0,1.0,0.0,7.0,8.0,9.0,1.0]]})" ),
         "InstancedStaticMesh" );
    ASSERT_TRUE( parsed.has_value() );
    const Assets::InstancedStaticMeshComponentSer& read =
         parsed.value(); // NOLINT(bugprone-unchecked-optional-access)

    EXPECT_FALSE( read.Primitive.has_value() );
    ASSERT_TRUE( read.MeshPath.has_value() );
    EXPECT_EQ( read.MeshPath, "Cooked/Meshes/Grass.stmesh" );

    ASSERT_TRUE( read.MaterialPaths.has_value() );
    EXPECT_EQ( read.MaterialPaths.value().size(), 1U ); // NOLINT(bugprone-unchecked-optional-access)

    ASSERT_TRUE( read.InstanceTransforms.has_value() );
    const std::vector<std::array<float, kMatrixElements>>& back =
         read.InstanceTransforms.value(); // NOLINT(bugprone-unchecked-optional-access)
    ASSERT_EQ( back.size(), 1U );
    EXPECT_FLOAT_EQ( back.at( 0 ).at( kTranslationZ ), 9.0F );
}

// THE RENAME-SAFE HALF, and the reason it was added. The static mesh mirror has carried
// `MeshGuid`/`MaterialGuids` since the asset database existed; the INSTANCED mirror -- the one grass
// assets are about to be scattered through -- carried the path alone, so renaming an instanced mesh
// unresolved every instance of it while the identical reference on a plain StaticMesh survived. Both
// fields must round-trip, and the GUID must be what a loader prefers.
TEST( GenericBlockRead, AnInstancedStaticMeshCarriesTheRenameSafeGuidsAsWellAsThePaths )
{
    Assets::InstancedStaticMeshComponentSer written;
    written.MeshPath           = "Cooked/Meshes/Grass.stmesh";
    written.MeshGuid           = kMeshGuid;
    written.MaterialPaths      = std::vector<std::string>{ "Materials/M_Grass.demat" };
    written.MaterialGuids      = std::vector<std::string>{ kMaterialGuid };
    written.InstanceTransforms = NineDistinctInstances();

    const auto parsed = ReadBlock<Assets::InstancedStaticMeshComponentSer>(
         WriteBlock( written, "InstancedStaticMesh" ), "InstancedStaticMesh" );
    ASSERT_TRUE( parsed.has_value() );
    const Assets::InstancedStaticMeshComponentSer& read =
         parsed.value(); // NOLINT(bugprone-unchecked-optional-access)

    ASSERT_TRUE( read.MeshGuid.has_value() );
    EXPECT_EQ( read.MeshGuid, kMeshGuid );
    ASSERT_TRUE( read.MeshPath.has_value() );
    EXPECT_EQ( read.MeshPath, "Cooked/Meshes/Grass.stmesh" );

    ASSERT_TRUE( read.MaterialGuids.has_value() );
    const std::vector<std::string>& guids =
         read.MaterialGuids.value(); // NOLINT(bugprone-unchecked-optional-access)
    ASSERT_EQ( guids.size(), 1U );
    EXPECT_EQ( guids.at( 0 ), kMaterialGuid );

    ASSERT_TRUE( read.InstanceTransforms.has_value() );
    ExpectSameMatrices( read.InstanceTransforms.value(), // NOLINT(bugprone-unchecked-optional-access)
                        NineDistinctInstances() );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
