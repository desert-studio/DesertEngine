// WHAT THE FOLD MUST REFUSE, AND WHAT IT MUST PRESERVE.
//
// The fold DESTROYS the entities it collapses. Everything below is therefore about the two ways that
// can go wrong quietly: it can drop something the source carried and the ISM cannot, or it can move
// something that has to stay put. Both are invisible in a frame that still has the right number of
// boxes in it.

#include <Editor/Core/Commands/InstanceFold.hpp>

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <set>
#include <string>

using namespace Desert::Editor::Commands;
namespace Geometry = Desert::Geometry;

namespace
{
    FoldMeshIdentity CubeIdentity()
    {
        FoldMeshIdentity identity;
        identity.Primitive = Geometry::PrimitiveType::Cube;
        return identity;
    }

    FoldCandidate At( const char* name, uint64_t id, const glm::vec3& position,
                      FoldMeshIdentity identity = CubeIdentity() )
    {
        FoldCandidate candidate;
        candidate.Entity   = ::Common::UUID( id );
        candidate.Name     = name;
        candidate.Identity = std::move( identity );
        candidate.World    = glm::translate( glm::mat4( 1.0f ), position );
        return candidate;
    }
} // namespace

// ---------------------------------------------------------------- what it produces

TEST( InstanceFold, TheInstanceTransformsAreTheSourcesOwnWorldMatricesInSelectionOrder )
{
    // THE RELATION, NOT THE COUNT. A plan with the right NUMBER of matrices and the wrong ones in it
    // draws the right number of boxes in the wrong places, and that is the failure a "9 instances"
    // assertion passes. Every matrix is compared against the candidate it came from.
    const std::vector<FoldCandidate> candidates = { At( "A", 1, { 200.0f, -200.0f, 0.0f } ),
                                                    At( "B", 2, { 200.0f, 0.0f, 0.0f } ),
                                                    At( "C", 3, { 600.0f, 200.0f, -50.0f } ) };

    const auto planned = PlanInstanceFold( candidates );
    ASSERT_TRUE( planned.IsSuccess() ) << planned.GetError();
    const FoldPlan& plan = planned.GetValue();

    ASSERT_EQ( plan.InstanceTransforms.size(), candidates.size() );
    ASSERT_EQ( plan.Sources.size(), candidates.size() );
    for ( size_t i = 0; i < candidates.size(); ++i )
    {
        EXPECT_EQ( plan.Sources[i], candidates[i].Entity );
        EXPECT_EQ( plan.InstanceTransforms[i], candidates[i].World ) << "instance " << i;
    }
}

TEST( InstanceFold, ARotatedAndScaledSourceKeepsItsWholeMatrix )
{
    // The first shape this could have taken is "collect the positions", which is what a fold looks like
    // if you only ever test it on a grid of axis-aligned cubes. A rotated, non-uniformly scaled prop is
    // the case that separates a transform from a translation.
    glm::mat4 world = glm::translate( glm::mat4( 1.0f ), glm::vec3( 10.0f, 20.0f, 30.0f ) );
    world           = glm::rotate( world, 0.7f, glm::vec3( 0.0f, 1.0f, 0.0f ) );
    world           = glm::scale( world, glm::vec3( 2.0f, 0.5f, 3.0f ) );

    std::vector<FoldCandidate> candidates = { At( "A", 1, { 0.0f, 0.0f, 0.0f } ),
                                              At( "B", 2, { 0.0f, 0.0f, 0.0f } ) };
    candidates[1].World                   = world;

    const auto planned = PlanInstanceFold( candidates );
    ASSERT_TRUE( planned.IsSuccess() ) << planned.GetError();
    EXPECT_EQ( planned.GetValue().InstanceTransforms[1], world );
}

// ---------------------------------------------------------------- what it refuses

TEST( InstanceFold, OneMeshIsNotAFoldAndTheRefusalSaysHowManyItFound )
{
    const auto planned = PlanInstanceFold( { At( "Only", 1, { 0.0f, 0.0f, 0.0f } ) } );
    ASSERT_FALSE( planned.IsSuccess() );
    EXPECT_NE( planned.GetError().find( "1" ), std::string::npos ) << planned.GetError();
}

TEST( InstanceFold, TwoDifferentMeshesAreRefusedAndBothAreNamed )
{
    FoldMeshIdentity sphere;
    sphere.Primitive = Geometry::PrimitiveType::Sphere;

    const auto planned =
         PlanInstanceFold( { At( "A", 1, { 0.0f, 0.0f, 0.0f } ), At( "B", 2, { 0.0f, 0.0f, 0.0f }, sphere ) } );
    ASSERT_FALSE( planned.IsSuccess() );
    EXPECT_NE( planned.GetError().find( "Cube" ), std::string::npos ) << planned.GetError();
    EXPECT_NE( planned.GetError().find( "Sphere" ), std::string::npos ) << planned.GetError();
}

TEST( InstanceFold, DifferentMaterialSlotsAreADifferentIdentityEvenOnTheSameMesh )
{
    // An ISM has ONE material list for every instance it holds. Two cubes with different materials
    // folded together would take one of the two looks, silently.
    FoldMeshIdentity painted = CubeIdentity();
    painted.Materials        = { ::Common::AssetHandle::FromKey( "Materials/Red.demat" ) };

    const auto planned =
         PlanInstanceFold( { At( "A", 1, { 0.0f, 0.0f, 0.0f } ), At( "B", 2, { 0.0f, 0.0f, 0.0f }, painted ) } );
    EXPECT_FALSE( planned.IsSuccess() );
}

TEST( InstanceFold, ShadowsOffAndShadowsOnAreADifferentIdentity )
{
    // The easiest one to forget: CastShadows is a single flag on the component, so folding a prop
    // whose shadow somebody turned OFF together with one that casts would put that shadow back.
    FoldMeshIdentity noShadow = CubeIdentity();
    noShadow.CastShadows      = false;

    const auto planned =
         PlanInstanceFold( { At( "A", 1, { 0.0f, 0.0f, 0.0f } ), At( "B", 2, { 0.0f, 0.0f, 0.0f }, noShadow ) } );
    ASSERT_FALSE( planned.IsSuccess() );
    EXPECT_NE( planned.GetError().find( "shadows off" ), std::string::npos ) << planned.GetError();
}

TEST( InstanceFold, ABlockerOnOneCandidateRefusesTheWHOLEFoldAndNamesTheEntity )
{
    // FOLDING "THE ONES THAT FIT" IS THE DEFECT, not the feature. The fold destroys its sources, so a
    // selection where one prop carries a collider must come back as a refusal a person can act on --
    // not as eight folded props and one survivor nobody notices.
    std::vector<FoldCandidate> candidates = { At( "Crate", 1, { 0.0f, 0.0f, 0.0f } ),
                                              At( "Crate With Physics", 2, { 200.0f, 0.0f, 0.0f } ),
                                              At( "Crate", 3, { 400.0f, 0.0f, 0.0f } ) };
    candidates[1].Blockers                = { "a Collider", "a RigidBody" };

    const auto planned = PlanInstanceFold( candidates );
    ASSERT_FALSE( planned.IsSuccess() );
    EXPECT_NE( planned.GetError().find( "Crate With Physics" ), std::string::npos ) << planned.GetError();
    EXPECT_NE( planned.GetError().find( "a Collider" ), std::string::npos ) << planned.GetError();
    EXPECT_NE( planned.GetError().find( "a RigidBody" ), std::string::npos ) << planned.GetError();
}

TEST( InstanceFold, ABlockerIsReportedBeforeAMixedIdentityBecauseItIsTheActionableOne )
{
    // Both faults at once. A person told "these are two different meshes" would go and make them the
    // same mesh, and still be refused -- for the collider nobody mentioned.
    FoldMeshIdentity sphere;
    sphere.Primitive                      = Geometry::PrimitiveType::Sphere;
    std::vector<FoldCandidate> candidates = { At( "A", 1, { 0.0f, 0.0f, 0.0f } ),
                                              At( "B", 2, { 0.0f, 0.0f, 0.0f }, sphere ) };
    candidates[0].Blockers                = { "children" };

    const auto planned = PlanInstanceFold( candidates );
    ASSERT_FALSE( planned.IsSuccess() );
    EXPECT_NE( planned.GetError().find( "children" ), std::string::npos ) << planned.GetError();
}

// ---------------------------------------------------------------- the label and the enumerator

TEST( InstanceFold, EveryPrimitiveEnumeratorHasItsOwnName )
{
    // WHY THIS LIVES HERE. The Instanced Static Mesh editor offered { "Cube", "Sphere", "Plane",
    // "Pyramid" } against an enum that runs Cube, Sphere, Pyramid, Plane, so picking "Plane" built a
    // pyramid -- for as long as that editor has existed, because nothing in a frame says which
    // enumerator a combo box meant. PrimitiveTypeName is now the single list, and a ninth shape added
    // without a case falls through to "Count", which this reds on.
    std::set<std::string> names;
    for ( int i = 0; i < static_cast<int>( Geometry::PrimitiveType::Count ); ++i )
    {
        const std::string name = Geometry::PrimitiveTypeName( static_cast<Geometry::PrimitiveType>( i ) );
        EXPECT_NE( name, "Count" ) << "enumerator " << i << " has no case in PrimitiveTypeName";
        EXPECT_TRUE( names.insert( name ).second ) << "two enumerators both answer " << name;
    }
    EXPECT_EQ( names.size(), static_cast<size_t>( Geometry::PrimitiveType::Count ) );
}

TEST( InstanceFold, TheAuthorablePrimitivesAreAPrefixOfTheEnumAndExcludeTheGeneratedOnes )
{
    // Terrain is built from a heightfield and LightCube by the light gizmo; offering either in a shape
    // combo would be a knob that cannot move anything. The positional relation is what matters: a combo
    // fills itself from this array, so a shape inserted in the middle of the enum must not shift the
    // meaning of a row.
    for ( size_t i = 0; i < Geometry::kAuthorablePrimitives.size(); ++i )
    {
        EXPECT_EQ( static_cast<int>( Geometry::kAuthorablePrimitives[i] ), static_cast<int>( i ) );
    }
    for ( const auto shape : Geometry::kAuthorablePrimitives )
    {
        EXPECT_NE( shape, Geometry::PrimitiveType::Terrain );
        EXPECT_NE( shape, Geometry::PrimitiveType::LightCube );
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
