// DynamicMeshAttributeSet / DynamicMeshOverlay / per-triangle attributes / PolygroupSet (ported from UE
// GeometryCore and GeometryProcessing), driven through DynamicMesh3's edit operators. The mesh carries every
// layer Desert's EditMesh stores: a UV overlay with a real seam, normals, colours, MaterialID and a polygroup
// layer. After every accepted edit -- split/flip/collapse/poke/merge and CompactInPlace -- the mesh must pass
// CheckValidity WITH attributes (DynamicMesh3::CheckValidity descends into every overlay); a split on the UV seam
// must leave two UV elements at the new vertex and a split off it one; per-triangle values must be inherited the
// way UE's handlers inherit them (a new triangle copies its source triangle's MaterialID and group).
#include <gtest/gtest.h>

#include <Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp>
#include <Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp>
#include <Engine/Geometry/MeshCore/Polygroups/PolygroupSet.hpp>

#include <random>
#include <vector>

using namespace Desert::Geometry;

namespace
{
    constexpr int N    = 4; // N x N quads, 10 cm cells
    constexpr int Seam = 2; // UV seam runs along the vertex column x == Seam

    int GridV( int x, int y )
    {
        return y * ( N + 1 ) + x;
    }

    bool Valid( const DynamicMesh3& Mesh )
    {
        return Mesh.CheckValidity( DynamicMesh3::ValidityOptions(), ValidityCheckFailMode::ReturnOnly );
    }

    int Material( const DynamicMesh3& Mesh, int Tid )
    {
        return Mesh.Attributes()->GetMaterialID()->GetValue( Tid );
    }

    int Group( const DynamicMesh3& Mesh, int Tid )
    {
        return Mesh.Attributes()->GetPolygroupLayer( 0 )->GetValue( Tid );
    }

    // Grid in the XY plane with every attribute layer. Triangles right of the seam column use their own UV
    // elements on it (UV x shifted by +1), so the column's interior edges are UV seam edges; MaterialID is 1 on
    // the left and 2 on the right; polygroup layer 0 is the quad row.
    DynamicMesh3 MakeAttributedPlane()
    {
        DynamicMesh3 Mesh;
        for ( int y = 0; y <= N; ++y )
            for ( int x = 0; x <= N; ++x )
                Mesh.AppendVertex( glm::dvec3( x * 10.0, y * 10.0, 0.0 ) );
        Mesh.EnableAttributes();
        DynamicMeshAttributeSet* Attr = Mesh.Attributes();
        Attr->EnablePrimaryColors();
        Attr->EnableMaterialID();
        Attr->SetNumPolygroupLayers( 1 );

        DynamicMeshUVOverlay*     UV = Attr->PrimaryUV();
        DynamicMeshNormalOverlay* Nm = Attr->PrimaryNormals();
        DynamicMeshColorOverlay*  Cl = Attr->PrimaryColors();
        std::vector<int>          UVLeft;
        std::vector<int>          UVRight;
        std::vector<int>          NmE;
        std::vector<int>          ClE;
        for ( int y = 0; y <= N; ++y )
            for ( int x = 0; x <= N; ++x )
            {
                UVLeft.push_back(
                     UV->AppendElement( glm::vec2( static_cast<float>( x ) / static_cast<float>( N ),
                                                   static_cast<float>( y ) / static_cast<float>( N ) ) ) );
                UVRight.push_back( x == Seam ? UV->AppendElement( glm::vec2(
                                                    1.0f + static_cast<float>( x ) / static_cast<float>( N ),
                                                    static_cast<float>( y ) / static_cast<float>( N ) ) )
                                             : UVLeft.back() );
                NmE.push_back( Nm->AppendElement( glm::vec3( 0, 0, 1 ) ) );
                ClE.push_back(
                     Cl->AppendElement( glm::vec4( static_cast<float>( x ) / static_cast<float>( N ),
                                                   static_cast<float>( y ) / static_cast<float>( N ), 0, 1 ) ) );
            }

        for ( int y = 0; y < N; ++y )
            for ( int x = 0; x < N; ++x )
            {
                const Index3i  Tris[2] = { Index3i( GridV( x, y ), GridV( x + 1, y ), GridV( x + 1, y + 1 ) ),
                                           Index3i( GridV( x, y ), GridV( x + 1, y + 1 ), GridV( x, y + 1 ) ) };
                const bool     bRight  = x >= Seam;
                for ( const Index3i& T : Tris )
                {
                    const int         Tid = Mesh.AppendTriangle( T );
                    std::vector<int>& UVE = bRight ? UVRight : UVLeft;
                    UV->SetTriangle( Tid, Index3i( UVE[T.A], UVE[T.B], UVE[T.C] ) );
                    Nm->SetTriangle( Tid, Index3i( NmE[T.A], NmE[T.B], NmE[T.C] ) );
                    Cl->SetTriangle( Tid, Index3i( ClE[T.A], ClE[T.B], ClE[T.C] ) );
                    Attr->GetMaterialID()->SetValue( Tid, bRight ? 2 : 1 );
                    Attr->GetPolygroupLayer( 0 )->SetValue( Tid, 10 + y );
                }
            }
        return Mesh;
    }

    // Both new triangles of a split / poke carry the values of the triangle they were cut from.
    void ExpectInherited( const DynamicMesh3& Mesh, int Source, int New )
    {
        ASSERT_TRUE( Mesh.IsTriangle( New ) );
        EXPECT_EQ( Material( Mesh, New ), Material( Mesh, Source ) );
        EXPECT_EQ( Group( Mesh, New ), Group( Mesh, Source ) );
    }
} // namespace

TEST( DynamicMesh3Attributes, FixtureIsValidAndHasItsSeam )
{
    DynamicMesh3 Mesh = MakeAttributedPlane();
    ASSERT_TRUE( Valid( Mesh ) );
    const DynamicMeshUVOverlay* UV = Mesh.Attributes()->PrimaryUV();
    EXPECT_TRUE( UV->IsSeamEdge( Mesh.FindEdge( GridV( Seam, 1 ), GridV( Seam, 2 ) ) ) );
    EXPECT_FALSE( UV->IsSeamEdge( Mesh.FindEdge( GridV( 1, 1 ), GridV( 1, 2 ) ) ) );
    EXPECT_EQ( UV->CountVertexElements( GridV( Seam, 2 ) ), 2 );
    EXPECT_EQ( UV->CountVertexElements( GridV( 1, 2 ) ), 1 );
    EXPECT_TRUE(
         Mesh.Attributes()->IsMaterialBoundaryEdge( Mesh.FindEdge( GridV( Seam, 1 ), GridV( Seam, 2 ) ) ) );
}

TEST( DynamicMesh3Attributes, SplitOnTheSeamKeepsTwoElements )
{
    DynamicMesh3  Mesh = MakeAttributedPlane();
    const int     Eid  = Mesh.FindEdge( GridV( Seam, 1 ), GridV( Seam, 2 ) );
    ASSERT_TRUE( Mesh.Attributes()->PrimaryUV()->IsSeamEdge( Eid ) );

    DynamicMesh3::EdgeSplitInfo Info;
    ASSERT_EQ( Mesh.SplitEdge( Eid, Info, 0.5 ), MeshResult::Ok );
    ASSERT_TRUE( Valid( Mesh ) );
    const DynamicMeshUVOverlay* UV = Mesh.Attributes()->PrimaryUV();
    EXPECT_EQ( UV->CountVertexElements( Info.NewVertex ), 2 );
    EXPECT_EQ( Mesh.Attributes()->PrimaryNormals()->CountVertexElements( Info.NewVertex ), 1 );
    // both halves of the split edge are still seams
    EXPECT_TRUE( UV->IsSeamEdge( Info.NewEdges.A ) );
    EXPECT_TRUE( UV->IsSeamEdge( Mesh.FindEdge( Info.OriginalVertices.A, Info.NewVertex ) ) );
    ExpectInherited( Mesh, Info.OriginalTriangles.A, Info.NewTriangles.A );
    ExpectInherited( Mesh, Info.OriginalTriangles.B, Info.NewTriangles.B );
    EXPECT_NE( Material( Mesh, Info.NewTriangles.A ), Material( Mesh, Info.NewTriangles.B ) );
}

TEST( DynamicMesh3Attributes, SplitOffTheSeamSharesOneInterpolatedElement )
{
    DynamicMesh3                Mesh = MakeAttributedPlane();
    const int                   A    = GridV( 1, 1 );
    const int                   B    = GridV( 1, 2 );
    DynamicMesh3::EdgeSplitInfo Info;
    ASSERT_EQ( Mesh.SplitEdge( A, B, Info ), MeshResult::Ok );
    ASSERT_TRUE( Valid( Mesh ) );
    const DynamicMeshUVOverlay* UV = Mesh.Attributes()->PrimaryUV();
    ASSERT_EQ( UV->CountVertexElements( Info.NewVertex ), 1 );
    glm::vec2 Mid{};
    UV->GetElementAtVertex( Info.NewTriangles.A, Info.NewVertex, Mid );
    EXPECT_NEAR( Mid.x, 0.25f, 1e-6f );
    EXPECT_NEAR( Mid.y, 0.375f, 1e-6f );
    ExpectInherited( Mesh, Info.OriginalTriangles.A, Info.NewTriangles.A );
    ExpectInherited( Mesh, Info.OriginalTriangles.B, Info.NewTriangles.B );
}

TEST( DynamicMesh3Attributes, FlipAndCollapseKeepOverlaysValid )
{
    DynamicMesh3 Mesh = MakeAttributedPlane();
    // interior diagonal of quad (0,1): not on the seam
    DynamicMesh3::EdgeFlipInfo Flip;
    ASSERT_EQ( Mesh.FlipEdge( GridV( 0, 1 ), GridV( 1, 2 ), Flip ), MeshResult::Ok );
    ASSERT_TRUE( Valid( Mesh ) );
    // UE's triangle attributes keep their value through a flip (the two triangles keep their ids)
    EXPECT_EQ( Material( Mesh, Flip.Triangles.A ), 1 );
    EXPECT_EQ( Material( Mesh, Flip.Triangles.B ), 1 );

    DynamicMesh3::EdgeCollapseInfo Collapse;
    ASSERT_EQ( Mesh.CollapseEdge( GridV( 3, 2 ), GridV( 3, 3 ), Collapse ), MeshResult::Ok );
    ASSERT_TRUE( Valid( Mesh ) );
    EXPECT_FALSE( Mesh.IsTriangle( Collapse.RemovedTris.A ) );
}

TEST( DynamicMesh3Attributes, PokeInheritsFromThePokedTriangle )
{
    DynamicMesh3                     Mesh = MakeAttributedPlane();
    const int                        Tid  = 2 * ( 1 * N + Seam ); // first triangle of quad (Seam, 1): right side
    DynamicMesh3::PokeTriangleInfo   Info;
    ASSERT_EQ( Mesh.PokeTriangle( Tid, Info ), MeshResult::Ok );
    ASSERT_TRUE( Valid( Mesh ) );
    EXPECT_EQ( Material( Mesh, Tid ), 2 );
    ExpectInherited( Mesh, Tid, Info.NewTriangles.A );
    ExpectInherited( Mesh, Tid, Info.NewTriangles.B );
    EXPECT_EQ( Mesh.Attributes()->PrimaryUV()->CountVertexElements( Info.NewVertex ), 1 );
}

TEST( DynamicMesh3Attributes, MergeEdgesWeldsOverlaysValid )
{
    // Two separate triangles whose edges (1,2) and (3,4) coincide with opposite orientation.
    DynamicMesh3     Mesh;
    const glm::dvec3 P[6] = { { 0, 0, 0 }, { 10, 0, 0 }, { 0, 10, 0 }, { 10, 0, 0 }, { 0, 10, 0 }, { 10, 10, 0 } };
    for ( const glm::dvec3& Pos : P )
        Mesh.AppendVertex( Pos );
    Mesh.EnableAttributes();
    Mesh.Attributes()->EnableMaterialID();
    const int              T0 = Mesh.AppendTriangle( 0, 1, 2 );
    const int              T1 = Mesh.AppendTriangle( 4, 3, 5 );
    DynamicMeshUVOverlay*  UV = Mesh.Attributes()->PrimaryUV();
    for ( int const Tid : { T0, T1 } )
    {
        const Index3i T = Mesh.GetTriangle( Tid );
        UV->SetTriangle( Tid, Index3i( UV->AppendElement( glm::vec2( P[T.A].x, P[T.A].y ) ),
                                       UV->AppendElement( glm::vec2( P[T.B].x, P[T.B].y ) ),
                                       UV->AppendElement( glm::vec2( P[T.C].x, P[T.C].y ) ) ) );
    }
    Mesh.Attributes()->GetMaterialID()->SetValue( T1, 7 );
    ASSERT_TRUE( Valid( Mesh ) );

    DynamicMesh3::MergeEdgesInfo Info;
    ASSERT_EQ( Mesh.MergeEdges( Mesh.FindEdge( 1, 2 ), Mesh.FindEdge( 3, 4 ), Info, true ), MeshResult::Ok );
    ASSERT_TRUE( Valid( Mesh ) );
    EXPECT_EQ( Mesh.VertexCount(), 4 );
    EXPECT_EQ( Material( Mesh, T1 ), 7 );
    // the overlay keeps its own elements: the welded edge is a UV seam (each side kept its element)
    EXPECT_TRUE( UV->IsSeamEdge( Info.KeptEdge ) );
}

TEST( DynamicMesh3Attributes, CompactInPlaceRemapsEveryLayer )
{
    DynamicMesh3 Mesh = MakeAttributedPlane();
    ASSERT_EQ( Mesh.RemoveTriangle( 0 ), MeshResult::Ok );
    DynamicMesh3::EdgeCollapseInfo Collapse;
    ASSERT_EQ( Mesh.CollapseEdge( GridV( 1, 2 ), GridV( 1, 3 ), Collapse ), MeshResult::Ok );
    ASSERT_TRUE( Valid( Mesh ) );
    ASSERT_FALSE( Mesh.IsCompact() );

    int MaterialSum = 0;
    for ( int const Tid : Mesh.TriangleIndicesItr() )
        MaterialSum += Material( Mesh, Tid );

    DynamicMeshCompactMaps Maps;
    Mesh.CompactInPlace( &Maps );
    ASSERT_TRUE( Mesh.IsCompact() );
    ASSERT_TRUE( Valid( Mesh ) );
    int MaterialSumAfter = 0;
    for ( int const Tid : Mesh.TriangleIndicesItr() )
        MaterialSumAfter += Material( Mesh, Tid );
    EXPECT_EQ( MaterialSumAfter, MaterialSum );
    // the seam survived the remap
    EXPECT_TRUE( Mesh.Attributes()->PrimaryUV()->IsSeamEdge(
         Mesh.FindEdge( Maps.GetVertexMapping( GridV( Seam, 1 ) ), Maps.GetVertexMapping( GridV( Seam, 2 ) ) ) ) );
}

TEST( DynamicMesh3Attributes, CopyCompactCopyAndAppendCarryAttributes )
{
    DynamicMesh3 Source = MakeAttributedPlane();
    ASSERT_EQ( Source.RemoveTriangle( 0 ), MeshResult::Ok );
    ASSERT_TRUE( Valid( Source ) );

    DynamicMesh3 Copy( Source );
    ASSERT_TRUE( Copy.HasAttributes() );
    ASSERT_TRUE( Valid( Copy ) );
    EXPECT_EQ( Copy.Attributes()->PrimaryUV()->ElementCount(), Source.Attributes()->PrimaryUV()->ElementCount() );

    DynamicMesh3 Moved( std::move( Copy ) );
    ASSERT_TRUE( Valid( Moved ) );
    EXPECT_EQ( Moved.Attributes()->GetParentMesh(), &Moved );

    DynamicMesh3           Compact;
    DynamicMeshCompactMaps Maps;
    Compact.CompactCopy( Source, true, true, true, true, &Maps );
    ASSERT_TRUE( Valid( Compact ) );
    EXPECT_TRUE( Compact.IsCompact() );
    EXPECT_EQ( Compact.Attributes()->GetMaterialID()->GetValue( Maps.GetTriangleMapping( 5 ) ),
               Material( Source, 5 ) );

    DynamicMesh3               Both = MakeAttributedPlane();
    DynamicMesh3::AppendInfo   AppendInfo;
    const int                  ElementsBefore = Both.Attributes()->PrimaryUV()->ElementCount();
    Both.AppendWithOffsets( Source, &AppendInfo );
    ASSERT_TRUE( Valid( Both ) );
    EXPECT_EQ( Both.Attributes()->PrimaryUV()->ElementCount(),
               ElementsBefore + Source.Attributes()->PrimaryUV()->ElementCount() );
    EXPECT_EQ( Material( Both, AppendInfo.TriangleOffset + 5 ), Material( Source, 5 ) );
}

TEST( DynamicMesh3Attributes, SetTriangleRefusesAttributedMeshes )
{
    DynamicMesh3 Mesh = MakeAttributedPlane();
    EXPECT_EQ( Mesh.SetTriangle( 0, Index3i( GridV( 0, 0 ), GridV( 1, 0 ), GridV( 0, 1 ) ), true ),
               MeshResult::Failed_Unsupported );
    Mesh.DiscardAttributes();
    EXPECT_NE( Mesh.SetTriangle( 0, Index3i( GridV( 0, 0 ), GridV( 1, 0 ), GridV( 0, 1 ) ), true ),
               MeshResult::Failed_Unsupported );
}

// Random split / flip / collapse / poke sequence: validity with attributes after every accepted edit, then after
// compaction. Three seeds, 150 edits each.
TEST( DynamicMesh3Attributes, RandomEditSequenceKeepsEveryLayerValid )
{
    for ( unsigned const Seed : { 1u, 7u, 42u } )
    {
        DynamicMesh3  Mesh = MakeAttributedPlane();
        std::mt19937  Rng( Seed );
        int           Accepted = 0;
        for ( int Step = 0; Step < 150; ++Step )
        {
            const int  Op     = static_cast<int>( Rng() % 4 );
            const int  Eid    = static_cast<int>( Rng() % static_cast<unsigned>( Mesh.MaxEdgeID() ) );
            const int  Tid    = static_cast<int>( Rng() % static_cast<unsigned>( Mesh.MaxTriangleID() ) );
            MeshResult Result = MeshResult::Failed_NotAnEdge;
            if ( Op == 0 && Mesh.IsEdge( Eid ) )
            {
                DynamicMesh3::EdgeSplitInfo Info;
                Result = Mesh.SplitEdge( Eid, Info, 0.5 );
            }
            // UE's overlay OnFlipEdge requires the flipped edge not to be a seam (it asserts a shared element
            // edge); its remeshers refuse seam flips before calling FlipEdge, and so does this sequence.
            else if ( Op == 1 && Mesh.IsEdge( Eid ) && !Mesh.Attributes()->IsSeamEdge( Eid ) )
            {
                DynamicMesh3::EdgeFlipInfo Info;
                Result = Mesh.FlipEdge( Eid, Info );
            }
            else if ( Op == 2 && Mesh.IsEdge( Eid ) )
            {
                const Index2i                  EV = Mesh.GetEdgeV( Eid );
                DynamicMesh3::EdgeCollapseInfo Info;
                Result = Mesh.CollapseEdge( EV.A, EV.B, Info );
            }
            else if ( Op == 3 && Mesh.IsTriangle( Tid ) )
            {
                DynamicMesh3::PokeTriangleInfo Info;
                Result = Mesh.PokeTriangle( Tid, Info );
            }
            if ( Result == MeshResult::Ok )
            {
                ++Accepted;
                ASSERT_TRUE( Valid( Mesh ) ) << "seed " << Seed << " step " << Step << " op " << Op;
            }
        }
        EXPECT_GT( Accepted, 50 ) << "seed " << Seed;
        Mesh.CompactInPlace();
        ASSERT_TRUE( Valid( Mesh ) ) << "seed " << Seed << " after compaction";
    }
}

TEST( DynamicMesh3Attributes, PolygroupSetReadsTheLayer )
{
    DynamicMesh3 const Mesh = MakeAttributedPlane();
    PolygroupSet const Default( &Mesh );
    EXPECT_EQ( Default.GetPolygroupIndex(), -1 );
    PolygroupSet Layer( &Mesh, 0 );
    EXPECT_EQ( Layer.GetPolygroupIndex(), 0 );
    for ( int const Tid : Mesh.TriangleIndicesItr() )
        EXPECT_EQ( Layer.GetGroup( Tid ), Group( Mesh, Tid ) );
    // UE's layer-index constructor returns before RecalculateMaxGroupID (MaxGroupID stays 0); ported as is.
    EXPECT_EQ( Layer.MaxGroupID, 0 );
    Layer.RecalculateMaxGroupID();
    EXPECT_EQ( Layer.MaxGroupID, 10 + N );
    EXPECT_TRUE( PolygroupLayer::Layer( 0 ).CheckExists( &Mesh ) );
    EXPECT_FALSE( PolygroupLayer::Layer( 1 ).CheckExists( &Mesh ) );

    DynamicMesh3  Writable = Mesh;
    PolygroupSet  WritableLayer( &Writable, 0 );
    const int     NewGroup = WritableLayer.AllocateNewGroupID();
    WritableLayer.SetGroup( 0, NewGroup, Writable );
    EXPECT_EQ( Group( Writable, 0 ), NewGroup );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
