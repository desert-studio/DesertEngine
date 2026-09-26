// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/DynamicMesh/DynamicMesh3.cpp:1-1615, adapted: UE
// Core via UECore.hpp; shape-generator Copy, IsSameAs,
// MeshInfoString and the debug-mesh cvars/stash (1616-1699) not ported.
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include <Common/Core/Core.hpp>
using namespace Desert::Geometry;

// NB: These have to be here until C++17 allows inline variables
const glm::dvec3 DynamicMesh3::InvalidVertex = glm::dvec3( std::numeric_limits<double>::max(), 0.0, 0.0 );

DynamicMesh3::DynamicMesh3() : DynamicMesh3( false, false, false, false )
{
}

DynamicMesh3::DynamicMesh3( bool bWantNormals, bool bWantColors, bool bWantUVs, bool bWantTriGroups )
{
    if ( bWantNormals )
    {
        VertexNormals = DynamicVector<glm::vec3>{};
    }
    if ( bWantColors )
    {
        VertexColors = DynamicVector<glm::vec3>{};
    }
    if ( bWantUVs )
    {
        VertexUVs = DynamicVector<glm::vec2>{};
    }
    if ( bWantTriGroups )
    {
        TriangleGroups = DynamicVector<int>{};
    }
}

DynamicMesh3::DynamicMesh3( MeshComponents flags )
     : DynamicMesh3( ( (int)flags & (int)MeshComponents::VertexNormals ) != 0,
                     ( (int)flags & (int)MeshComponents::VertexColors ) != 0,
                     ( (int)flags & (int)MeshComponents::VertexUVs ) != 0,
                     ( (int)flags & (int)MeshComponents::FaceGroups ) != 0 )
{
}

// normals/colors/uvs will only be copied if they exist
DynamicMesh3::DynamicMesh3( const DynamicMesh3& Other )
     : Vertices{ Other.Vertices }, VertexRefCounts{ Other.VertexRefCounts }, VertexNormals{ Other.VertexNormals },
       VertexColors{ Other.VertexColors }, VertexUVs{ Other.VertexUVs }, VertexEdgeLists{ Other.VertexEdgeLists },

       Triangles{ Other.Triangles }, TriangleRefCounts{ Other.TriangleRefCounts },
       TriangleEdges{ Other.TriangleEdges }, TriangleGroups{ Other.TriangleGroups },
       GroupIDCounter{ Other.GroupIDCounter },

       Edges{ Other.Edges }, EdgeRefCounts{ Other.EdgeRefCounts }
{
    if ( Other.HasAttributes() )
    {
        EnableAttributes();
        AttributeSet->Copy( *Other.AttributeSet );
    }
    ChangeStampShape.Set( Other.ChangeStampShape.GetValue() );
    ChangeStampTopology.Set( Other.ChangeStampTopology.GetValue() );
}
DynamicMesh3::DynamicMesh3( DynamicMesh3&& Other )
     : Vertices{ std::move( Other.Vertices ) }, VertexRefCounts{ std::move( Other.VertexRefCounts ) },
       VertexNormals{ std::move( Other.VertexNormals ) }, VertexColors{ std::move( Other.VertexColors ) },
       VertexUVs{ std::move( Other.VertexUVs ) }, VertexEdgeLists{ std::move( Other.VertexEdgeLists ) },

       Triangles{ std::move( Other.Triangles ) }, TriangleRefCounts{ std::move( Other.TriangleRefCounts ) },
       TriangleEdges{ std::move( Other.TriangleEdges ) }, TriangleGroups{ std::move( Other.TriangleGroups ) },
       GroupIDCounter{ Other.GroupIDCounter },

       AttributeSet{ std::move( Other.AttributeSet ) },

       Edges{ std::move( Other.Edges ) }, EdgeRefCounts{ std::move( Other.EdgeRefCounts ) }
{
    if ( AttributeSet )
    {
        AttributeSet->Reparent( this );
    }
    ChangeStampShape.Set( Other.ChangeStampShape.GetValue() );
    ChangeStampTopology.Set( Other.ChangeStampTopology.GetValue() );
}
DynamicMesh3::~DynamicMesh3() = default;

const DynamicMesh3& DynamicMesh3::operator=( const DynamicMesh3& CopyMesh )
{
    Copy( CopyMesh );
    return *this;
}

const DynamicMesh3& DynamicMesh3::operator=( DynamicMesh3&& Other )
{
    if ( this != &Other )
    {
        Vertices        = std::move( Other.Vertices );
        VertexRefCounts = std::move( Other.VertexRefCounts );
        VertexNormals   = std::move( Other.VertexNormals );
        VertexColors    = std::move( Other.VertexColors );
        VertexUVs       = std::move( Other.VertexUVs );
        VertexEdgeLists = std::move( Other.VertexEdgeLists );

        Triangles         = std::move( Other.Triangles );
        TriangleRefCounts = std::move( Other.TriangleRefCounts );
        TriangleEdges     = std::move( Other.TriangleEdges );
        TriangleGroups    = std::move( Other.TriangleGroups );
        GroupIDCounter    = Other.GroupIDCounter;

        Edges         = std::move( Other.Edges );
        EdgeRefCounts = std::move( Other.EdgeRefCounts );
        AttributeSet  = std::move( Other.AttributeSet );
        if ( AttributeSet )
        {
            AttributeSet->Reparent( this );
        }
        ChangeStampShape.Set( Other.ChangeStampShape.GetValue() );
        ChangeStampTopology.Set( Other.ChangeStampTopology.GetValue() );
    }

    return *this;
}

void DynamicMesh3::Copy( const DynamicMesh3& copy, bool bNormals, bool bColors, bool bUVs, bool bAttributes )
{
    if ( this != &copy )
    {
        Vertices        = copy.Vertices;
        VertexNormals   = bNormals ? copy.VertexNormals : std::optional<DynamicVector<glm::vec3>>{};
        VertexColors    = bColors ? copy.VertexColors : std::optional<DynamicVector<glm::vec3>>{};
        VertexUVs       = bUVs ? copy.VertexUVs : std::optional<DynamicVector<glm::vec2>>{};
        VertexRefCounts = copy.VertexRefCounts;
        VertexEdgeLists = copy.VertexEdgeLists;

        Triangles         = copy.Triangles;
        TriangleEdges     = copy.TriangleEdges;
        TriangleRefCounts = copy.TriangleRefCounts;
        TriangleGroups    = copy.TriangleGroups;
        GroupIDCounter    = copy.GroupIDCounter;

        Edges         = copy.Edges;
        EdgeRefCounts = copy.EdgeRefCounts;

        // Note that we populate our existing AttributeSet when possible rather than building a new one, so a
        // client may hold on to the AttributeSet pointer across a Copy.
        if ( bAttributes && copy.HasAttributes() )
        {
            EnableAttributes(); // does nothing if already enabled
            AttributeSet->Copy( *copy.AttributeSet );
        }
        else
        {
            DiscardAttributes();
        }
        ChangeStampShape.Set( copy.ChangeStampShape.GetValue() );
        ChangeStampTopology.Set( copy.ChangeStampTopology.GetValue() );
    }
}

void DynamicMesh3::AppendWithOffsets( const DynamicMesh3& ToAppend, AppendInfo* OutAppendInfo )
{
    AppendInfo  LocalAppendInfo;
    AppendInfo* UseAppendInfo = OutAppendInfo ? OutAppendInfo : &LocalAppendInfo;

    UseAppendInfo->VertexOffset   = MaxVertexID();
    UseAppendInfo->TriangleOffset = MaxTriangleID();
    UseAppendInfo->EdgeOffset     = MaxEdgeID();
    UseAppendInfo->GroupOffset    = HasTriangleGroups() ? MaxGroupID() : 0;
    UseAppendInfo->NumVertex      = ToAppend.MaxVertexID();
    UseAppendInfo->NumTriangle    = ToAppend.MaxTriangleID();
    UseAppendInfo->NumEdge        = ToAppend.MaxEdgeID();
    if ( HasAttributes() )
    {
        for ( int32_t NormalLayerIdx = 0, N = std::min( 3, Attributes()->NumNormalLayers() ); NormalLayerIdx < N;
              ++NormalLayerIdx )
        {
            UseAppendInfo->NormalOverlayOffsets[NormalLayerIdx] =
                 Attributes()->GetNormalLayer( NormalLayerIdx )->MaxElementID();
        }
    }
    Vertices.Add( ToAppend.Vertices );
    auto MatchOptional = []<typename T>( std::optional<DynamicVector<T>>&       Src,
                                         const std::optional<DynamicVector<T>>& ToAppend, int32_t NumDefault,
                                         T DefaultValue = T() )
    {
        if ( Src.has_value() )
        {
            if ( ToAppend.has_value() )
            {
                Src->Add( *ToAppend );
            }
            else
            {
                Src->Resize( NumDefault, DefaultValue );
            }
        }
    };
    MatchOptional( VertexNormals, ToAppend.VertexNormals, Vertices.Num(), glm::vec3( 0, 1, 0 ) );
    MatchOptional( VertexColors, ToAppend.VertexColors, Vertices.Num(), glm::vec3( 1 ) );
    MatchOptional( VertexUVs, ToAppend.VertexUVs, Vertices.Num(), glm::vec2( 0 ) );

    VertexRefCounts.Append( ToAppend.VertexRefCounts );
    VertexEdgeLists.AppendWithElementOffset( ToAppend.VertexEdgeLists, UseAppendInfo->EdgeOffset );

    Triangles.Add( ToAppend.Triangles );
    for ( int32_t Idx = UseAppendInfo->TriangleOffset, N = static_cast<int32_t>( Triangles.Num() ); Idx < N;
          ++Idx )
    {
        Triangles[Idx].A += UseAppendInfo->VertexOffset;
        Triangles[Idx].B += UseAppendInfo->VertexOffset;
        Triangles[Idx].C += UseAppendInfo->VertexOffset;
    }
    // TriangleEdges should be 1:1 with Triangles, so this ensure should not fail ...
    // however due to a now-fixed bug, it is possible that a serialized mesh will
    // have too many TriangleEdges; we can recover by resizing to match before appending
    if ( !Common::EnsureOrWarn( TriangleEdges.Num() == UseAppendInfo->TriangleOffset,
                                "TriangleEdges.Num() == UseAppendInfo->TriangleOffset" ) )
    {
        // Resize to recover from a too-large TriangleEdges array
        TriangleEdges.Resize( UseAppendInfo->TriangleOffset );
    }
    TriangleEdges.Add( ToAppend.TriangleEdges );
    for ( int32_t Idx = UseAppendInfo->TriangleOffset, N = static_cast<int32_t>( Triangles.Num() ); Idx < N;
          ++Idx )
    {
        TriangleEdges[Idx].A += UseAppendInfo->EdgeOffset;
        TriangleEdges[Idx].B += UseAppendInfo->EdgeOffset;
        TriangleEdges[Idx].C += UseAppendInfo->EdgeOffset;
    }
    TriangleRefCounts.Append( ToAppend.TriangleRefCounts );

    if ( TriangleGroups.has_value() )
    {
        GroupIDCounter += ToAppend.MaxGroupID();
        if ( ToAppend.TriangleGroups.has_value() )
        {
            TriangleGroups->Add( *ToAppend.TriangleGroups );
            for ( int32_t Idx = UseAppendInfo->TriangleOffset, N = static_cast<int32_t>( Triangles.Num() );
                  Idx < N; ++Idx )
            {
                ( *TriangleGroups )[Idx] += UseAppendInfo->GroupOffset;
            }
        }
        else
        {
            TriangleGroups->Resize( MaxTriangleID(), 0 );
        }
    }

    Edges.Add( ToAppend.Edges );
    for ( int32_t Idx = UseAppendInfo->EdgeOffset, N = static_cast<int32_t>( Edges.Num() ); Idx < N; ++Idx )
    {
        Edges[Idx].Tri.A += UseAppendInfo->TriangleOffset;
        if ( Edges[Idx].Tri.B != INDEX_NONE )
        {
            Edges[Idx].Tri.B += UseAppendInfo->TriangleOffset;
        }
        Edges[Idx].Vert.A += UseAppendInfo->VertexOffset;
        Edges[Idx].Vert.B += UseAppendInfo->VertexOffset;
    }
    EdgeRefCounts.Append( ToAppend.EdgeRefCounts );

    if ( HasAttributes() )
    {
        if ( ToAppend.HasAttributes() )
        {
            AttributeSet->Append( *ToAppend.AttributeSet, *UseAppendInfo );
        }
        else
        {
            AttributeSet->AppendDefaulted( *UseAppendInfo );
        }
    }
    UpdateChangeStamps( true, true );
}

void DynamicMesh3::CompactCopy( const DynamicMesh3& copy, bool bNormals, bool bColors, bool bUVs, bool bAttributes,
                                DynamicMeshCompactMaps* CompactInfo )
{

    // currently cannot re-use existing attribute buffers
    Clear();
    if ( bNormals && copy.HasVertexNormals() )
    {
        EnableVertexNormals( glm::vec3( 0, 1, 0 ) );
    }
    if ( bColors && copy.HasVertexColors() )
    {
        EnableVertexColors( glm::vec3( 1 ) );
    }
    if ( bUVs && copy.HasVertexUVs() )
    {
        EnableVertexUVs( glm::vec2( 0 ) );
    }

    // Use a triangle map if we have a CompactInfo.
    // The triangle map is needed to copy attributes, and is always wanted when the caller asks for maps.
    const bool bUseTriangleMap = CompactInfo != nullptr || ( bAttributes && copy.HasAttributes() );
    // If we don't have a CompactInfo, we'll make it refer to a local one.
    DynamicMeshCompactMaps LocalCompactInfo;
    if ( !CompactInfo )
    {
        CompactInfo = &LocalCompactInfo;
    }

    CompactInfo->ResetVertexMap( copy.MaxVertexID(), false );
    VertexInfo vinfo;
    for ( int vid = 0, NumVid = copy.MaxVertexID(); vid < NumVid; vid++ )
    {
        if ( copy.IsVertex( vid ) )
        {
            copy.GetVertex( vid, vinfo, bNormals, bColors, bUVs );
            CompactInfo->SetVertexMapping( vid, AppendVertex( vinfo ) );
        }
        else
        {
            CompactInfo->SetVertexMapping( vid, DynamicMeshCompactMaps::InvalidID );
        }
    }

    // [TODO] would be much faster to explicitly copy triangle & edge data structures!!
    if ( copy.HasTriangleGroups() )
    {
        EnableTriangleGroups( 0 );
    }

    if ( bUseTriangleMap )
    {
        CompactInfo->ResetTriangleMap( bUseTriangleMap ? copy.MaxTriangleID() : 0, true );
    }
    for ( int tid : copy.TriangleIndicesItr() )
    {
        const Index3i  t      = CompactInfo->GetVertexMapping( copy.GetTriangle( tid ) );
        const int      g      = ( copy.HasTriangleGroups() ) ? copy.GetTriangleGroup( tid ) : InvalidID;
        const int      NewTID = AppendTriangle( t, g );
        GroupIDCounter        = std::max( GroupIDCounter, g + 1 );
        if ( bUseTriangleMap )
        {
            CompactInfo->SetTriangleMapping( tid, NewTID );
        }
    }

    // copy attributes
    if ( bAttributes && copy.HasAttributes() )
    {
        EnableAttributes();
        AttributeSet->EnableMatchingAttributes( *copy.Attributes() );
        AttributeSet->CompactCopy( *CompactInfo, *copy.Attributes() );
    }

    ChangeStampShape.Set( copy.ChangeStampShape.GetValue() );
    ChangeStampTopology.Set( copy.ChangeStampTopology.GetValue() );
}

void DynamicMesh3::Clear()
{
    Vertices.Clear();
    VertexRefCounts.Clear();
    VertexNormals.reset();
    VertexColors.reset();
    VertexUVs.reset();
    VertexEdgeLists.Reset();

    Triangles.Clear();
    TriangleRefCounts.Clear();
    TriangleEdges.Clear();
    TriangleGroups.reset();
    GroupIDCounter = 0;

    Edges.Clear();
    EdgeRefCounts.Clear();
    AttributeSet.reset();
    ChangeStampShape.Set( 1 );
    ChangeStampTopology.Set( 1 );
}

void DynamicMesh3::EnableMatchingAttributes( const DynamicMesh3& ToMatch, bool bClearExisting,
                                             bool bDiscardExtraAttributes )
{
    bool bWantVertexNormals = ( bClearExisting || bDiscardExtraAttributes )
                                   ? ToMatch.HasVertexNormals()
                                   : ( ToMatch.HasVertexNormals() || this->HasVertexNormals() );
    if ( bClearExisting || bWantVertexNormals == false )
    {
        DiscardVertexNormals();
    }
    if ( bWantVertexNormals )
    {
        EnableVertexNormals( glm::vec3( 0, 0, 1 ) );
    }

    bool bWantVertexColors = ( bClearExisting || bDiscardExtraAttributes )
                                  ? ToMatch.HasVertexColors()
                                  : ( ToMatch.HasVertexColors() || this->HasVertexColors() );
    if ( bClearExisting || bWantVertexColors == false )
    {
        DiscardVertexColors();
    }
    if ( bWantVertexColors )
    {
        EnableVertexColors( glm::vec3( 0 ) );
    }

    bool bWantVertexUVs = ( bClearExisting || bDiscardExtraAttributes )
                               ? ToMatch.HasVertexUVs()
                               : ( ToMatch.HasVertexUVs() || this->HasVertexUVs() );
    if ( bClearExisting || bWantVertexUVs == false )
    {
        DiscardVertexUVs();
    }
    if ( bWantVertexUVs )
    {
        EnableVertexUVs( glm::vec2( 0 ) );
    }

    bool bWantTriangleGroups = ( bClearExisting || bDiscardExtraAttributes )
                                    ? ToMatch.HasTriangleGroups()
                                    : ( ToMatch.HasTriangleGroups() || this->HasTriangleGroups() );
    if ( bClearExisting || bWantTriangleGroups == false )
    {
        DiscardTriangleGroups();
    }
    if ( bWantTriangleGroups )
    {
        EnableTriangleGroups();
    }

    bool bWantAttributes = ( bClearExisting || bDiscardExtraAttributes )
                                ? ToMatch.HasAttributes()
                                : ( ToMatch.HasAttributes() || this->HasAttributes() );
    if ( bClearExisting || bWantAttributes == false )
    {
        DiscardAttributes();
    }
    if ( bWantAttributes )
    {
        EnableAttributes();
    }
    if ( HasAttributes() && ToMatch.HasAttributes() )
    {
        Attributes()->EnableMatchingAttributes( *ToMatch.Attributes(), bClearExisting, bDiscardExtraAttributes );
    }
}
void DynamicMesh3::EnableAttributes()
{
    if ( HasAttributes() )
    {
        return;
    }
    AttributeSet = std::make_unique<DynamicMeshAttributeSet>( this );
    AttributeSet->Initialize( MaxVertexID(), MaxTriangleID() );
}

void DynamicMesh3::DiscardAttributes()
{
    AttributeSet = nullptr;
}

int DynamicMesh3::GetComponentsFlags() const
{
    int c = 0;
    if ( HasVertexNormals() )
    {
        c |= (int)MeshComponents::VertexNormals;
    }
    if ( HasVertexColors() )
    {
        c |= (int)MeshComponents::VertexColors;
    }
    if ( HasVertexUVs() )
    {
        c |= (int)MeshComponents::VertexUVs;
    }
    if ( HasTriangleGroups() )
    {
        c |= (int)MeshComponents::FaceGroups;
    }
    return c;
}

void DynamicMesh3::EnableMeshComponents( int MeshComponentsFlags )
{
    if ( int( MeshComponents::FaceGroups ) & MeshComponentsFlags )
    {
        EnableTriangleGroups( 0 );
    }
    else
    {
        DiscardTriangleGroups();
    }
    if ( int( MeshComponents::VertexColors ) & MeshComponentsFlags )
    {
        EnableVertexColors( glm::vec3( 1, 1, 1 ) );
    }
    else
    {
        DiscardVertexColors();
    }
    if ( int( MeshComponents::VertexNormals ) & MeshComponentsFlags )
    {
        EnableVertexNormals( glm::vec3( 0, 1, 0 ) );
    }
    else
    {
        DiscardVertexNormals();
    }
    if ( int( MeshComponents::VertexUVs ) & MeshComponentsFlags )
    {
        EnableVertexUVs( glm::vec2( 0, 0 ) );
    }
    else
    {
        DiscardVertexUVs();
    }
}

void DynamicMesh3::EnableVertexNormals( const glm::vec3& InitialNormal )
{
    if ( HasVertexNormals() )
    {
        return;
    }

    DynamicVector<glm::vec3>  NewNormals;
    int                       NV = MaxVertexID();
    NewNormals.Resize( NV );
    for ( int i = 0; i < NV; ++i )
    {
        NewNormals[i] = InitialNormal;
    }
    VertexNormals = std::move( NewNormals );
}

void DynamicMesh3::DiscardVertexNormals()
{
    VertexNormals.reset();
}

void DynamicMesh3::EnableVertexColors( const glm::vec3& InitialColor )
{
    if ( HasVertexColors() )
    {
        return;
    }
    VertexColors = DynamicVector<glm::vec3>();
    int NV       = MaxVertexID();
    VertexColors->Resize( NV );
    for ( int i = 0; i < NV; ++i )
    {
        VertexColors.value()[i] = InitialColor;
    }
}

void DynamicMesh3::DiscardVertexColors()
{
    VertexColors.reset();
}

void DynamicMesh3::EnableVertexUVs( const glm::vec2& InitialUV )
{
    if ( HasVertexUVs() )
    {
        return;
    }
    VertexUVs = DynamicVector<glm::vec2>();
    int NV    = MaxVertexID();
    VertexUVs->Resize( NV );
    for ( int i = 0; i < NV; ++i )
    {
        VertexUVs.value()[i] = InitialUV;
    }
}

void DynamicMesh3::DiscardVertexUVs()
{
    VertexUVs.reset();
}

void DynamicMesh3::EnableTriangleGroups( int InitialGroup )
{
    if ( HasTriangleGroups() )
    {
        return;
    }
    assert( InitialGroup >= 0 );
    TriangleGroups = DynamicVector<int>();
    int NT         = MaxTriangleID();
    TriangleGroups->Resize( NT );
    for ( int i = 0; i < NT; ++i )
    {
        TriangleGroups.value()[i] = InitialGroup;
    }
    GroupIDCounter = InitialGroup + 1;
}

void DynamicMesh3::DiscardTriangleGroups()
{
    TriangleGroups.reset();
    GroupIDCounter = 0;
}

bool DynamicMesh3::GetVertex( int vID, VertexInfo& vinfo, bool bWantNormals, bool bWantColors,
                              bool bWantUVs ) const
{
    if ( VertexRefCounts.IsValid( vID ) == false )
    {
        return false;
    }
    vinfo.Position = Vertices[vID];
    vinfo.bHaveN = vinfo.bHaveUV = vinfo.bHaveC = false;
    if ( HasVertexNormals() && bWantNormals )
    {
        vinfo.bHaveN                               = true;
        const DynamicVector<glm::vec3>& NormalVec  = VertexNormals.value();
        vinfo.Normal                               = NormalVec[vID];
    }
    if ( HasVertexColors() && bWantColors )
    {
        vinfo.bHaveC                              = true;
        const DynamicVector<glm::vec3>& ColorVec  = VertexColors.value();
        vinfo.Color                               = ColorVec[vID];
    }
    if ( HasVertexUVs() && bWantUVs )
    {
        vinfo.bHaveUV                          = true;
        const DynamicVector<glm::vec2>& UVVec  = VertexUVs.value();
        vinfo.UV                               = UVVec[vID];
    }
    return true;
}

int DynamicMesh3::GetMaxVtxEdgeCount() const
{
    int max = 0;
    for ( int vid : VertexIndicesItr() )
    {
        max = std::max( max, VertexEdgeLists.GetCount( vid ) );
    }
    return max;
}

VertexInfo DynamicMesh3::GetVertexInfo( int i ) const
{
    VertexInfo vi  = VertexInfo();
    vi.Position    = GetVertex( i );
    vi.bHaveN = vi.bHaveC = vi.bHaveUV = false;
    if ( HasVertexNormals() )
    {
        vi.bHaveN = true;
        vi.Normal = GetVertexNormal( i );
    }
    if ( HasVertexColors() )
    {
        vi.bHaveC = true;
        vi.Color  = GetVertexColor( i );
    }
    if ( HasVertexUVs() )
    {
        vi.bHaveUV = true;
        vi.UV      = GetVertexUV( i );
    }
    return vi;
}

Index3i DynamicMesh3::GetTriNeighbourTris( int tID ) const
{
    if ( TriangleRefCounts.IsValid( tID ) )
    {
        Index3i nbr_t = Index3i::Zero();
        for ( int j = 0; j < 3; ++j )
        {
            Edge Edge  = Edges[TriangleEdges[tID][j]];
            nbr_t[j]   = ( Edge.Tri[0] == tID ) ? Edge.Tri[1] : Edge.Tri[0];
        }
        return nbr_t;
    }
    else
    {
        return InvalidTriangle;
    }
}

void DynamicMesh3::EnumerateVertexTriangles( int32_t VertexID, std::function<void( int32_t )> ApplyFunc ) const
{
    assert( VertexRefCounts.IsValid( VertexID ) );
    if ( !IsVertex( VertexID ) )
    {
        return;
    }

    VertexEdgeLists.Enumerate( VertexID,
                               [&]( int32_t eid )
                               {
                                   const Edge  Edge   = Edges[eid];
                                   const int   vOther = Edge.Vert.A == VertexID ? Edge.Vert.B : Edge.Vert.A;
                                   if ( TriHasSequentialVertices( Edge.Tri[0], VertexID, vOther ) )
                                   {
                                       ApplyFunc( Edge.Tri[0] );
                                   }
                                   if ( Edge.Tri[1] != InvalidID &&
                                        TriHasSequentialVertices( Edge.Tri[1], VertexID, vOther ) )
                                   {
                                       ApplyFunc( Edge.Tri[1] );
                                   }
                               } );
}

int32_t DynamicMesh3::GetSingleVertexTriangle( int32_t VID ) const
{
    assert( VertexRefCounts.IsValid( VID ) );
    if ( !IsVertex( VID ) || VertexEdgeLists.GetCount( VID ) == 0 )
    {
        return IndexConstants::InvalidID;
    }
    return VertexEdgeLists.First( VID );
}

void DynamicMesh3::EnumerateEdgeTriangles( int32_t EdgeID, const std::function<void( int32_t )>& ApplyFunc ) const
{
    assert( EdgeRefCounts.IsValid( EdgeID ) );
    if ( IsEdge( EdgeID ) )
    {
        const Edge Edge = Edges[EdgeID];
        ApplyFunc( Edge.Tri.A );
        if ( Edge.Tri.B != IndexConstants::InvalidID )
        {
            ApplyFunc( Edge.Tri.B );
        }
    }
}

size_t DynamicMesh3::GetByteCount() const
{
    size_t const ByteCount = VertexRefCounts.GetByteCount() + VertexEdgeLists.GetByteCount() +
                             TriangleRefCounts.GetByteCount() + EdgeRefCounts.GetByteCount();
    return ByteCount;
}

bool DynamicMesh3::CheckValidity( ValidityOptions Options, ValidityCheckFailMode FailMode ) const
{

    std::vector<int> triToVtxRefs;
    triToVtxRefs.resize( MaxVertexID() );

    bool                    is_ok        = true;
    std::function<void( bool )> CheckOrFailF = [&]( bool b ) { is_ok = is_ok && b; };
    if ( FailMode == ValidityCheckFailMode::Check )
    {
        CheckOrFailF = [&]( bool b )
        {
            assert( ( b ) && "DynamicMesh3::CheckValidity failed!" );
            is_ok = is_ok && b;
        };
    }
    else if ( FailMode == ValidityCheckFailMode::Ensure )
    {
        CheckOrFailF = [&]( bool b )
        {
            DESERT_VERIFY_WARN( b, "DynamicMesh3::CheckValidity failed!" );
            is_ok = is_ok && b;
        };
    }

    // When ref counts are dense, the used ref count must match the size of the vertex/triangle vector.
    CheckOrFailF( !VertexRefCounts.IsDense() ||
                  ( VertexRefCounts.IsDense() && VertexRefCounts.GetCount() == Vertices.Num() ) );
    CheckOrFailF( !TriangleRefCounts.IsDense() ||
                  ( TriangleRefCounts.IsDense() && TriangleRefCounts.GetCount() == Triangles.Num() ) );

    // TriangleEdges should be 1:1 with Triangles
    CheckOrFailF( TriangleEdges.Num() == Triangles.Num() );

    for ( int tID : TriangleIndicesItr() )
    {
        CheckOrFailF( IsTriangle( tID ) );
        CheckOrFailF( TriangleRefCounts.GetRefCount( tID ) == 1 );

        // vertices must exist
        Index3i tv = GetTriangle( tID );
        for ( int j = 0; j < 3; ++j )
        {
            CheckOrFailF( IsVertex( tv[j] ) );
            triToVtxRefs[tv[j]] += 1;
        }

        // edges must exist and reference this tri
        Index3i e;
        for ( int j = 0; j < 3; ++j )
        {
            int a = tv[j], b = tv[( j + 1 ) % 3];
            e[j] = FindEdge( a, b );
            CheckOrFailF( e[j] != InvalidID );
            CheckOrFailF( EdgeHasTriangle( e[j], tID ) );
            CheckOrFailF( e[j] == FindEdgeFromTri( a, b, tID ) );
        }
        CheckOrFailF( e[0] != e[1] && e[0] != e[2] && e[1] != e[2] );

        // tri nbrs must exist and reference this tri, or same edge must be boundary edge
        Index3i te = GetTriEdges( tID );
        for ( int j = 0; j < 3; ++j )
        {
            int eid = te[j];
            CheckOrFailF( IsEdge( eid ) );
            int tOther = GetOtherEdgeTriangle( eid, tID );
            if ( tOther == InvalidID )
            {
                CheckOrFailF( IsBoundaryTriangle( tID ) );
                continue;
            }

            CheckOrFailF( TriHasNeighbourTri( tOther, tID ) == true );

            // edge must have same two verts as tri for same index
            int      a = tv[j], b = tv[( j + 1 ) % 3];
            Index2i  ev = GetEdgeV( te[j] );
            CheckOrFailF( IndexUtil::SamePairUnordered( a, b, ev[0], ev[1] ) );

            // also check that nbr edge has opposite orientation
            if ( Options.bAllowAdjacentFacesReverseOrientation == false )
            {
                Index3i  othertv = GetTriangle( tOther );
                int      found   = IndexUtil::FindTriOrderedEdge( b, a, othertv );
                CheckOrFailF( found != InvalidID );
            }
        }
    }

    if ( HasTriangleGroups() )
    {
        const DynamicVector<int>& Groups = TriangleGroups.value();
        // must have a group per triangle ID
        CheckOrFailF( Groups.Num() == MaxTriangleID() );
        // group IDs must be in range [0, GroupIDCounter)
        for ( int TID : TriangleIndicesItr() )
        {
            CheckOrFailF( Groups[TID] >= 0 );
            CheckOrFailF( Groups[TID] < GroupIDCounter );
        }
    }

    // edge verts/tris must exist
    for ( int eID : EdgeIndicesItr() )
    {
        CheckOrFailF( IsEdge( eID ) );
        CheckOrFailF( EdgeRefCounts.GetRefCount( eID ) == 1 );
        Index2i ev = GetEdgeV( eID );
        Index2i et = GetEdgeT( eID );
        CheckOrFailF( IsVertex( ev[0] ) );
        CheckOrFailF( IsVertex( ev[1] ) );
        CheckOrFailF( et[0] != InvalidID );
        CheckOrFailF( ev[0] < ev[1] );
        CheckOrFailF( IsTriangle( et[0] ) );
        if ( et[1] != InvalidID )
        {
            CheckOrFailF( IsTriangle( et[1] ) );
        }
    }

    // verify compact check
    bool is_compact = VertexRefCounts.IsDense();
    if ( is_compact )
    {
        for ( int vid = 0; vid < (int)Vertices.GetLength(); ++vid )
        {
            CheckOrFailF( VertexRefCounts.IsValid( vid ) );
        }
    }

    // vertex edges must exist and reference this vert
    for ( int vID : VertexIndicesItr() )
    {
        CheckOrFailF( IsVertex( vID ) );

        glm::dvec3 v = GetVertex( vID );
        CheckOrFailF( std::isnan( glm::length2( v ) ) == false );
        CheckOrFailF( std::isfinite( glm::length2( v ) ) );

        for ( int edgeid : VertexEdgeLists.Values( vID ) )
        {
            CheckOrFailF( IsEdge( edgeid ) );
            CheckOrFailF( EdgeHasVertex( edgeid, vID ) );

            int otherV = GetOtherEdgeVertex( edgeid, vID );
            int e2     = FindEdge( vID, otherV );
            CheckOrFailF( e2 != InvalidID );
            CheckOrFailF( e2 == edgeid );
            e2 = FindEdge( otherV, vID );
            CheckOrFailF( e2 != InvalidID );
            CheckOrFailF( e2 == edgeid );
        }

        for ( int nbr_vid : VtxVerticesItr( vID ) )
        {
            CheckOrFailF( IsVertex( nbr_vid ) );
            int edge = FindEdge( vID, nbr_vid );
            CheckOrFailF( IsEdge( edge ) );
        }

        DynamicMesh3::LocalIntArray vTris;
        GetVtxTriangles( vID, vTris );
        // System.Console.WriteLine(string.Format("{0} {1} {2}", vID, vTris.Count, GetVtxEdges(vID).Count));
        if ( Options.bAllowNonManifoldVertices )
        {
            CheckOrFailF( static_cast<int32_t>( vTris.size() ) <= GetVtxEdgeCount( vID ) );
        }
        else
        {
            CheckOrFailF( static_cast<int32_t>( vTris.size() ) == GetVtxEdgeCount( vID ) ||
                          static_cast<int32_t>( vTris.size() ) == GetVtxEdgeCount( vID ) - 1 );
        }
        int32_t const VertexRefCount = VertexRefCounts.GetRefCount( vID );
        CheckOrFailF( VertexRefCount == ( static_cast<int32_t>( vTris.size() ) + 1 ) );
        CheckOrFailF( triToVtxRefs[vID] == static_cast<int32_t>( vTris.size() ) );
        for ( int tID : vTris )
        {
            CheckOrFailF( TriangleHasVertex( tID, vID ) );
        }

        // check that edges around vert only references tris above, and reference all of them!
        std::vector<int> vRemoveTris( vTris );
        for ( int edgeid : VertexEdgeLists.Values( vID ) )
        {
            Index2i edget = GetEdgeT( edgeid );
            CheckOrFailF( ( std::find( vTris.begin(), vTris.end(), edget[0] ) != vTris.end() ) );
            if ( edget[1] != InvalidID )
            {
                CheckOrFailF( ( std::find( vTris.begin(), vTris.end(), edget[1] ) != vTris.end() ) );
            }
            std::erase( vRemoveTris, edget[0] );
            if ( edget[1] != InvalidID )
            {
                std::erase( vRemoveTris, edget[1] );
            }
        }
        CheckOrFailF( vRemoveTris.empty() );
    }

    if ( HasAttributes() )
    {
        CheckOrFailF( Attributes()->CheckValidity( true, FailMode ) );
    }

    return is_ok;
}
int DynamicMesh3::AddTriangleInternal( int a, int b, int c, int e0, int e1, int e2 )
{
    int tid = TriangleRefCounts.Allocate();
    Triangles.InsertAt( Index3i( a, b, c ), tid );
    TriangleEdges.InsertAt( Index3i( e0, e1, e2 ), tid );
    return tid;
}

int DynamicMesh3::ReplaceEdgeVertex( int eID, int vOld, int vNew )
{
    Index2i&  Verts = Edges[eID].Vert;
    int       a = Verts[0], b = Verts[1];
    if ( a == vOld )
    {
        Verts[0] = std::min( b, vNew );
        Verts[1] = std::max( b, vNew );
        return 0;
    }
    else if ( b == vOld )
    {
        Verts[0] = std::min( a, vNew );
        Verts[1] = std::max( a, vNew );
        return 1;
    }
    else
    {
        return -1;
    }
}

int DynamicMesh3::ReplaceEdgeTriangle( int eID, int tOld, int tNew )
{
    Index2i&  Tris = Edges[eID].Tri;
    int       a = Tris[0], b = Tris[1];
    if ( a == tOld )
    {
        if ( tNew == InvalidID )
        {
            Tris[0] = b;
            Tris[1] = InvalidID;
        }
        else
        {
            Tris[0] = tNew;
        }
        return 0;
    }
    else if ( b == tOld )
    {
        Tris[1] = tNew;
        return 1;
    }
    else
    {
        return -1;
    }
}

int DynamicMesh3::ReplaceTriangleEdge( int tID, int eOld, int eNew )
{
    Index3i& TriEdgeIDs = TriangleEdges[tID];
    for ( int j = 0; j < 3; ++j )
    {
        if ( TriEdgeIDs[j] == eOld )
        {
            TriEdgeIDs[j] = eNew;
            return j;
        }
    }
    return -1;
}

//! returns edge ID
int DynamicMesh3::FindTriangleEdge( int tID, int vA, int vB ) const
{
    const Index3i Triangle = Triangles[tID];
    if ( IndexUtil::SamePairUnordered( Triangle[0], Triangle[1], vA, vB ) )
        return TriangleEdges[tID][0];
    if ( IndexUtil::SamePairUnordered( Triangle[1], Triangle[2], vA, vB ) )
        return TriangleEdges[tID][1];
    if ( IndexUtil::SamePairUnordered( Triangle[2], Triangle[0], vA, vB ) )
        return TriangleEdges[tID][2];
    return InvalidID;
}

int32_t DynamicMesh3::FindEdgeInternal( int32_t vA, int32_t vB, bool& bIsBoundary ) const
{
    // edge vertices must be sorted (min,max), that means we only need one index-check in inner loop.
    int32_t vMax = vA;
    int32_t vMin = vB;
    if ( vB > vA )
    {
        vMax = vB;
        vMin = vA;
    }
    return VertexEdgeLists.Find(
         vMin,
         [&]( int32_t eid )
         {
             const Edge Edge = Edges[eid];
             if ( Edge.Vert[1] == vMax )
             {
                 bIsBoundary = ( Edge.Tri[1] == InvalidID );
                 return true;
             }
             return false;
         },
         InvalidID );
}

int DynamicMesh3::FindEdge( int vA, int vB ) const
{
    assert( IsVertex( vA ) );
    assert( IsVertex( vB ) );
    if ( vA == vB )
    {
        // self-edges are not allowed, and if we fall through to the search below on a self edge we will
        // incorrectly sometimes return an arbitrary edge if queried for a self-edge, due to the optimization of
        // only checking one side of the edge
        return InvalidID;
    }

    // edge vertices must be sorted (min,max),
    //   that means we only need one index-check in inner loop.
    int32_t vMax = vA;
    int32_t vMin = vB;
    if ( vB > vA )
    {
        vMax = vB;
        vMin = vA;
    }
    if ( IsVertex( vMin ) )
    {
        return VertexEdgeLists.Find(
             vMin, [&]( int32_t eid ) { return ( Edges[eid].Vert[1] == vMax ); }, InvalidID );
    }
    else
    {
        return InvalidID;
    }

    // this is slower, likely because it creates func<> every time. can we do w/o that?
    // return VertexEdgeLists.Find(vI, (eid) => { return Edges[4 * eid + 1] == vO; }, InvalidID);
}

int DynamicMesh3::FindEdgeFromTri( int vA, int vB, int tID ) const
{
    const Index3i& Triangle        = Triangles[tID];
    const Index3i& TriangleEdgeIDs = TriangleEdges[tID];
    if ( IndexUtil::SamePairUnordered( vA, vB, Triangle[0], Triangle[1] ) )
    {
        return TriangleEdgeIDs[0];
    }
    if ( IndexUtil::SamePairUnordered( vA, vB, Triangle[1], Triangle[2] ) )
    {
        return TriangleEdgeIDs[1];
    }
    if ( IndexUtil::SamePairUnordered( vA, vB, Triangle[2], Triangle[0] ) )
    {
        return TriangleEdgeIDs[2];
    }
    return InvalidID;
}

int DynamicMesh3::FindEdgeFromTriPair( int TriA, int TriB ) const
{
    if ( TriangleRefCounts.IsValid( TriA ) && TriangleRefCounts.IsValid( TriB ) )
    {
        for ( int j = 0; j < 3; ++j )
        {
            int         EdgeID = TriangleEdges[TriA][j];
            const Edge  Edge   = Edges[EdgeID];
            int         NbrT   = ( Edge.Tri[0] == TriA ) ? Edge.Tri[1] : Edge.Tri[0];
            if ( NbrT == TriB )
            {
                return EdgeID;
            }
        }
    }
    return InvalidID;
}
