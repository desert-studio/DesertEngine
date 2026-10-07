#include <Engine/Geometry/AutoRig.hpp>

#include <gtest/gtest.h>

using Desert::SkinnedVertex;
using Desert::Vertex;
using Desert::Geometry::AutoSkinVertices;
using Desert::Geometry::DistancePointToSegment;
using Desert::Geometry::RigBone;

namespace
{
    Vertex At( float x, float y, float z )
    {
        Vertex v{};
        v.Position = { x, y, z };
        return v;
    }

    float WeightOf( const SkinnedVertex& sv, uint32_t bone )
    {
        float w = 0.0f;
        for ( size_t i = 0; i < SkinnedVertex::MAX_BONE_INFLUENCES; ++i )
            if ( sv.BoneIDs[i] == bone )
                w += sv.BoneWeights[i];
        return w;
    }

    float WeightSum( const SkinnedVertex& sv )
    {
        float s = 0.0f;
        for ( float w : sv.BoneWeights )
            s += w;
        return s;
    }
} // namespace

TEST( AutoRig, DistancePointToSegment )
{
    EXPECT_FLOAT_EQ( DistancePointToSegment( { 0, 5, 0 }, { 0, 0, 0 }, { 0, 10, 0 } ), 0.0f ); // on segment
    EXPECT_FLOAT_EQ( DistancePointToSegment( { 3, 5, 0 }, { 0, 0, 0 }, { 0, 10, 0 } ), 3.0f ); // beside it
    EXPECT_FLOAT_EQ( DistancePointToSegment( { 0, 20, 0 }, { 0, 0, 0 }, { 0, 10, 0 } ),
                     10.0f ); // past the end -> distance to endpoint
    EXPECT_FLOAT_EQ( DistancePointToSegment( { 3, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } ), 3.0f ); // degenerate
}

TEST( AutoRig, VertexBindsToNearestBone )
{
    // An L-shaped chain: root(0,0,0) -> mid(0,2,0) -> tip(3,2,0). Bone 1's body is the VERTICAL segment
    // root->mid; bone 2's body is the HORIZONTAL segment mid->tip. A vertex hugging one segment must weight
    // mostly to that bone. (A bone's body is parent-head -> own-head, matching the drawn parent->child link.)
    const std::vector<RigBone> bones = {
         { { 0, 0, 0 }, -1 }, // 0 root (point)
         { { 0, 2, 0 }, 0 },  // 1 vertical bone   (0,0,0)->(0,2,0)
         { { 3, 2, 0 }, 1 },  // 2 horizontal bone (0,2,0)->(3,2,0)
    };

    const auto nearVertical   = AutoSkinVertices( { At( 0.05f, 1.0f, 0.0f ) }, bones )[0];
    const auto nearHorizontal = AutoSkinVertices( { At( 1.5f, 2.05f, 0.0f ) }, bones )[0];

    EXPECT_GT( WeightOf( nearVertical, 1 ), WeightOf( nearVertical, 2 ) );     // hugs vertical -> bone 1
    EXPECT_GT( WeightOf( nearHorizontal, 2 ), WeightOf( nearHorizontal, 1 ) ); // hugs horizontal -> bone 2
    EXPECT_NEAR( WeightSum( nearVertical ), 1.0f, 1e-4f );
    EXPECT_NEAR( WeightSum( nearHorizontal ), 1.0f, 1e-4f );
}

TEST( AutoRig, KeepsTopFourInfluencesNormalized )
{
    // Six bones fanned around a vertex — only the 4 nearest may carry weight, still summing to 1.
    std::vector<RigBone> bones;
    for ( int i = 0; i < 6; ++i )
        bones.push_back( { { static_cast<float>( i ), 0.0f, 0.0f }, -1 } );

    const auto skinned = AutoSkinVertices( { At( 0.0f, 0.0f, 0.0f ) }, bones );
    ASSERT_EQ( skinned.size(), 1u );

    int influences = 0;
    for ( float w : skinned[0].BoneWeights )
        if ( w > 0.0f )
            ++influences;
    EXPECT_LE( influences, 4 );
    EXPECT_NEAR( WeightSum( skinned[0] ), 1.0f, 1e-4f );
    EXPECT_EQ( skinned[0].BoneIDs[0], 0u ); // bone 0 is the closest (distance 0)
}

TEST( AutoRig, EmptyRigFallsBackToRoot )
{
    const auto skinned = AutoSkinVertices( { At( 1, 2, 3 ) }, {} );
    ASSERT_EQ( skinned.size(), 1u );
    EXPECT_EQ( skinned[0].BoneIDs[0], 0u );
    EXPECT_FLOAT_EQ( skinned[0].BoneWeights[0], 1.0f );
    EXPECT_EQ( skinned[0].StaticVertex.Position, ( glm::vec3{ 1, 2, 3 } ) ); // static data preserved
}
