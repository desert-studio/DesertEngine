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
        m_VertexNormals = DynamicVector<glm::vec3>{};
    }
    if ( bWantColors )
    {
        m_VertexColors = DynamicVector<glm::vec3>{};
    }
    if ( bWantUVs )
    {
        m_VertexUVs = DynamicVector<glm::vec2>{};
    }
    if ( bWantTriGroups )
    {
        m_TriangleGroups = DynamicVector<int>{};
    }
}

DynamicMesh3::DynamicMesh3( MeshComponents flags )
     : DynamicMesh3( ( static_cast<int>( flags ) & static_cast<int>( MeshComponents::VertexNormals ) ) != 0,
                     ( static_cast<int>( flags ) & static_cast<int>( MeshComponents::VertexColors ) ) != 0,
                     ( static_cast<int>( flags ) & static_cast<int>( MeshComponents::VertexUVs ) ) != 0,
                     ( static_cast<int>( flags ) & static_cast<int>( MeshComponents::FaceGroups ) ) != 0 )
{
}

// normals/colors/uvs will only be copied if they exist
DynamicMesh3::DynamicMesh3( const DynamicMesh3& CopyMesh )
     : m_Vertices{ CopyMesh.m_Vertices }, m_VertexRefCounts{ CopyMesh.m_VertexRefCounts },
       m_VertexNormals{ CopyMesh.m_VertexNormals }, m_VertexColors{ CopyMesh.m_VertexColors },
       m_VertexUVs{ CopyMesh.m_VertexUVs }, m_VertexEdgeLists{ CopyMesh.m_VertexEdgeLists },

       m_Triangles{ CopyMesh.m_Triangles }, m_TriangleRefCounts{ CopyMesh.m_TriangleRefCounts },
       m_TriangleEdges{ CopyMesh.m_TriangleEdges }, m_TriangleGroups{ CopyMesh.m_TriangleGroups },
       m_GroupIDCounter{ CopyMesh.m_GroupIDCounter },

       m_Edges{ CopyMesh.m_Edges }, m_EdgeRefCounts{ CopyMesh.m_EdgeRefCounts }
{
    if ( CopyMesh.HasAttributes() )
    {
        EnableAttributes();
        m_AttributeSet->Copy( *CopyMesh.m_AttributeSet );
    }
    m_ChangeStampShape.Set( CopyMesh.m_ChangeStampShape.GetValue() );
    m_ChangeStampTopology.Set( CopyMesh.m_ChangeStampTopology.GetValue() );
}
// Not noexcept: DynamicVector's move re-seeds the moved-from vector with a fresh block (AddAllocatedBlock, can
// throw bad_alloc) and ChangeStamp::Set locks a std::mutex (can throw system_error).
// NOLINTNEXTLINE(bugprone-exception-escape,*-noexcept-move-*)
DynamicMesh3::DynamicMesh3( DynamicMesh3&& MoveMesh )
     : m_Vertices{ std::move( MoveMesh.m_Vertices ) },
       m_VertexRefCounts{ std::move( MoveMesh.m_VertexRefCounts ) },
       m_VertexNormals{ std::move( MoveMesh.m_VertexNormals ) },
       m_VertexColors{ std::move( MoveMesh.m_VertexColors ) }, m_VertexUVs{ std::move( MoveMesh.m_VertexUVs ) },
       m_VertexEdgeLists{ std::move( MoveMesh.m_VertexEdgeLists ) },

       m_Triangles{ std::move( MoveMesh.m_Triangles ) },
       m_TriangleRefCounts{ std::move( MoveMesh.m_TriangleRefCounts ) },
       m_TriangleEdges{ std::move( MoveMesh.m_TriangleEdges ) },
       m_TriangleGroups{ std::move( MoveMesh.m_TriangleGroups ) }, m_GroupIDCounter{ MoveMesh.m_GroupIDCounter },

       m_AttributeSet{ std::move( MoveMesh.m_AttributeSet ) },

       m_Edges{ std::move( MoveMesh.m_Edges ) }, m_EdgeRefCounts{ std::move( MoveMesh.m_EdgeRefCounts ) }
{
    if ( m_AttributeSet )
    {
        m_AttributeSet->Reparent( this );
    }
    m_ChangeStampShape.Set( MoveMesh.m_ChangeStampShape.GetValue() );
    m_ChangeStampTopology.Set( MoveMesh.m_ChangeStampTopology.GetValue() );
}
DynamicMesh3::~DynamicMesh3() = default;

DynamicMesh3& DynamicMesh3::operator=( const DynamicMesh3& CopyMesh )
{
    Copy( CopyMesh );
    return *this;
}

// Not noexcept: DynamicVector's move assignment empties the target and re-seeds the moved-from vector with a fresh
// block (AddAllocatedBlock, can throw bad_alloc), and ChangeStamp::Set locks a std::mutex (can throw
// system_error). NOLINTNEXTLINE(bugprone-exception-escape,*-noexcept-move-*)
DynamicMesh3& DynamicMesh3::operator=( DynamicMesh3&& MoveMesh )
{
    if ( this != &MoveMesh )
    {
        m_Vertices        = std::move( MoveMesh.m_Vertices );
        m_VertexRefCounts = std::move( MoveMesh.m_VertexRefCounts );
        m_VertexNormals   = std::move( MoveMesh.m_VertexNormals );
        m_VertexColors    = std::move( MoveMesh.m_VertexColors );
        m_VertexUVs       = std::move( MoveMesh.m_VertexUVs );
        m_VertexEdgeLists = std::move( MoveMesh.m_VertexEdgeLists );

        m_Triangles         = std::move( MoveMesh.m_Triangles );
        m_TriangleRefCounts = std::move( MoveMesh.m_TriangleRefCounts );
        m_TriangleEdges     = std::move( MoveMesh.m_TriangleEdges );
        m_TriangleGroups    = std::move( MoveMesh.m_TriangleGroups );
        m_GroupIDCounter    = MoveMesh.m_GroupIDCounter;

        m_Edges         = std::move( MoveMesh.m_Edges );
        m_EdgeRefCounts = std::move( MoveMesh.m_EdgeRefCounts );
        m_AttributeSet  = std::move( MoveMesh.m_AttributeSet );
        if ( m_AttributeSet )
        {
            m_AttributeSet->Reparent( this );
        }
        m_ChangeStampShape.Set( MoveMesh.m_ChangeStampShape.GetValue() );
        m_ChangeStampTopology.Set( MoveMesh.m_ChangeStampTopology.GetValue() );
    }

    return *this;
}

void DynamicMesh3::Copy( const DynamicMesh3& copy, bool bNormals, bool bColors, bool bUVs, bool bAttributes )
{
    if ( this != &copy )
    {
        m_Vertices        = copy.m_Vertices;
        m_VertexNormals   = bNormals ? copy.m_VertexNormals : std::optional<DynamicVector<glm::vec3>>{};
        m_VertexColors    = bColors ? copy.m_VertexColors : std::optional<DynamicVector<glm::vec3>>{};
        m_VertexUVs       = bUVs ? copy.m_VertexUVs : std::optional<DynamicVector<glm::vec2>>{};
        m_VertexRefCounts = copy.m_VertexRefCounts;
        m_VertexEdgeLists = copy.m_VertexEdgeLists;

        m_Triangles         = copy.m_Triangles;
        m_TriangleEdges     = copy.m_TriangleEdges;
        m_TriangleRefCounts = copy.m_TriangleRefCounts;
        m_TriangleGroups    = copy.m_TriangleGroups;
        m_GroupIDCounter    = copy.m_GroupIDCounter;

        m_Edges         = copy.m_Edges;
        m_EdgeRefCounts = copy.m_EdgeRefCounts;

        // Note that we populate our existing AttributeSet when possible rather than building a new one, so a
        // client may hold on to the AttributeSet pointer across a Copy.
        if ( bAttributes && copy.HasAttributes() )
        {
            EnableAttributes(); // does nothing if already enabled
            m_AttributeSet->Copy( *copy.m_AttributeSet );
        }
        else
        {
            DiscardAttributes();
        }
        m_ChangeStampShape.Set( copy.m_ChangeStampShape.GetValue() );
        m_ChangeStampTopology.Set( copy.m_ChangeStampTopology.GetValue() );
    }
}

void DynamicMesh3::AppendWithOffsets( const DynamicMesh3& ToAppend, AppendInfo* OutAppendInfo )
{
    AppendInfo  LocalAppendInfo;
    AppendInfo* UseAppendInfo = ( OutAppendInfo != nullptr ) ? OutAppendInfo : &LocalAppendInfo;

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
    m_Vertices.Add( ToAppend.m_Vertices );
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
    MatchOptional( m_VertexNormals, ToAppend.m_VertexNormals, static_cast<int32_t>( m_Vertices.Num() ),
                   glm::vec3( 0, 1, 0 ) );
    MatchOptional( m_VertexColors, ToAppend.m_VertexColors, static_cast<int32_t>( m_Vertices.Num() ),
                   glm::vec3( 1 ) );
    MatchOptional( m_VertexUVs, ToAppend.m_VertexUVs, static_cast<int32_t>( m_Vertices.Num() ), glm::vec2( 0 ) );

    m_VertexRefCounts.Append( ToAppend.m_VertexRefCounts );
    m_VertexEdgeLists.AppendWithElementOffset( ToAppend.m_VertexEdgeLists, UseAppendInfo->EdgeOffset );

    m_Triangles.Add( ToAppend.m_Triangles );
    for ( int32_t Idx = UseAppendInfo->TriangleOffset, N = static_cast<int32_t>( m_Triangles.Num() ); Idx < N;
          ++Idx )
    {
        m_Triangles[Idx].A += UseAppendInfo->VertexOffset;
        m_Triangles[Idx].B += UseAppendInfo->VertexOffset;
        m_Triangles[Idx].C += UseAppendInfo->VertexOffset;
    }
    // TriangleEdges should be 1:1 with Triangles, so this ensure should not fail ...
    // however due to a now-fixed bug, it is possible that a serialized mesh will
    // have too many TriangleEdges; we can recover by resizing to match before appending
    if ( !Common::EnsureOrWarn( static_cast<int32_t>( m_TriangleEdges.Num() ) == UseAppendInfo->TriangleOffset,
                                "TriangleEdges.Num() == UseAppendInfo->TriangleOffset" ) )
    {
        // Resize to recover from a too-large TriangleEdges array
        m_TriangleEdges.Resize( UseAppendInfo->TriangleOffset );
    }
    m_TriangleEdges.Add( ToAppend.m_TriangleEdges );
    for ( int32_t Idx = UseAppendInfo->TriangleOffset, N = static_cast<int32_t>( m_Triangles.Num() ); Idx < N;
          ++Idx )
    {
        m_TriangleEdges[Idx].A += UseAppendInfo->EdgeOffset;
        m_TriangleEdges[Idx].B += UseAppendInfo->EdgeOffset;
        m_TriangleEdges[Idx].C += UseAppendInfo->EdgeOffset;
    }
    m_TriangleRefCounts.Append( ToAppend.m_TriangleRefCounts );

    if ( m_TriangleGroups.has_value() )
    {
        m_GroupIDCounter += ToAppend.MaxGroupID();
        if ( ToAppend.m_TriangleGroups.has_value() )
        {
            m_TriangleGroups->Add( *ToAppend.m_TriangleGroups );
            for ( int32_t Idx = UseAppendInfo->TriangleOffset, N = static_cast<int32_t>( m_Triangles.Num() );
                  Idx < N; ++Idx )
            {
                ( *m_TriangleGroups )[Idx] += UseAppendInfo->GroupOffset;
            }
        }
        else
        {
            m_TriangleGroups->Resize( MaxTriangleID(), 0 );
        }
    }

    m_Edges.Add( ToAppend.m_Edges );
    for ( int32_t Idx = UseAppendInfo->EdgeOffset, N = static_cast<int32_t>( m_Edges.Num() ); Idx < N; ++Idx )
    {
        m_Edges[Idx].Tri.A += UseAppendInfo->TriangleOffset;
        if ( m_Edges[Idx].Tri.B != INDEX_NONE )
        {
            m_Edges[Idx].Tri.B += UseAppendInfo->TriangleOffset;
        }
        m_Edges[Idx].Vert.A += UseAppendInfo->VertexOffset;
        m_Edges[Idx].Vert.B += UseAppendInfo->VertexOffset;
    }
    m_EdgeRefCounts.Append( ToAppend.m_EdgeRefCounts );

    if ( HasAttributes() )
    {
        if ( ToAppend.HasAttributes() )
        {
            m_AttributeSet->Append( *ToAppend.m_AttributeSet, *UseAppendInfo );
        }
        else
        {
            m_AttributeSet->AppendDefaulted( *UseAppendInfo );
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
    if ( CompactInfo == nullptr )
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
    for ( int const FromTID : copy.TriangleIndicesItr() )
    {
        const Index3i  t      = CompactInfo->GetVertexMapping( copy.GetTriangle( FromTID ) );
        const int      g      = ( copy.HasTriangleGroups() ) ? copy.GetTriangleGroup( FromTID ) : InvalidID;
        const int      NewTID = AppendTriangle( t, g );
        m_GroupIDCounter      = std::max( m_GroupIDCounter, g + 1 );
        if ( bUseTriangleMap )
        {
            CompactInfo->SetTriangleMapping( FromTID, NewTID );
        }
    }

    // copy attributes
    if ( bAttributes && copy.HasAttributes() )
    {
        EnableAttributes();
        m_AttributeSet->EnableMatchingAttributes( *copy.Attributes() );
        m_AttributeSet->CompactCopy( *CompactInfo, *copy.Attributes() );
    }

    m_ChangeStampShape.Set( copy.m_ChangeStampShape.GetValue() );
    m_ChangeStampTopology.Set( copy.m_ChangeStampTopology.GetValue() );
}

void DynamicMesh3::Clear()
{
    m_Vertices.Clear();
    m_VertexRefCounts.Clear();
    m_VertexNormals.reset();
    m_VertexColors.reset();
    m_VertexUVs.reset();
    m_VertexEdgeLists.Reset();

    m_Triangles.Clear();
    m_TriangleRefCounts.Clear();
    m_TriangleEdges.Clear();
    m_TriangleGroups.reset();
    m_GroupIDCounter = 0;

    m_Edges.Clear();
    m_EdgeRefCounts.Clear();
    m_AttributeSet.reset();
    m_ChangeStampShape.Set( 1 );
    m_ChangeStampTopology.Set( 1 );
}

void DynamicMesh3::EnableMatchingAttributes( const DynamicMesh3& ToMatch, bool bClearExisting,
                                             bool bDiscardExtraAttributes )
{
    bool const bWantVertexNormals = ( bClearExisting || bDiscardExtraAttributes )
                                         ? ToMatch.HasVertexNormals()
                                         : ( ToMatch.HasVertexNormals() || this->HasVertexNormals() );
    if ( bClearExisting || !bWantVertexNormals )
    {
        DiscardVertexNormals();
    }
    if ( bWantVertexNormals )
    {
        EnableVertexNormals( glm::vec3( 0, 0, 1 ) );
    }

    bool const bWantVertexColors = ( bClearExisting || bDiscardExtraAttributes )
                                        ? ToMatch.HasVertexColors()
                                        : ( ToMatch.HasVertexColors() || this->HasVertexColors() );
    if ( bClearExisting || !bWantVertexColors )
    {
        DiscardVertexColors();
    }
    if ( bWantVertexColors )
    {
        EnableVertexColors( glm::vec3( 0 ) );
    }

    bool const bWantVertexUVs = ( bClearExisting || bDiscardExtraAttributes )
                                     ? ToMatch.HasVertexUVs()
                                     : ( ToMatch.HasVertexUVs() || this->HasVertexUVs() );
    if ( bClearExisting || !bWantVertexUVs )
    {
        DiscardVertexUVs();
    }
    if ( bWantVertexUVs )
    {
        EnableVertexUVs( glm::vec2( 0 ) );
    }

    bool const bWantTriangleGroups = ( bClearExisting || bDiscardExtraAttributes )
                                          ? ToMatch.HasTriangleGroups()
                                          : ( ToMatch.HasTriangleGroups() || this->HasTriangleGroups() );
    if ( bClearExisting || !bWantTriangleGroups )
    {
        DiscardTriangleGroups();
    }
    if ( bWantTriangleGroups )
    {
        EnableTriangleGroups();
    }

    bool const bWantAttributes = ( bClearExisting || bDiscardExtraAttributes )
                                      ? ToMatch.HasAttributes()
                                      : ( ToMatch.HasAttributes() || this->HasAttributes() );
    if ( bClearExisting || !bWantAttributes )
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
    m_AttributeSet = std::make_unique<DynamicMeshAttributeSet>( this );
    m_AttributeSet->Initialize( MaxVertexID(), MaxTriangleID() );
}

void DynamicMesh3::DiscardAttributes()
{
    m_AttributeSet = nullptr;
}

int DynamicMesh3::GetComponentsFlags() const
{
    int c = 0;
    if ( HasVertexNormals() )
    {
        c |= static_cast<int>( MeshComponents::VertexNormals );
    }
    if ( HasVertexColors() )
    {
        c |= static_cast<int>( MeshComponents::VertexColors );
    }
    if ( HasVertexUVs() )
    {
        c |= static_cast<int>( MeshComponents::VertexUVs );
    }
    if ( HasTriangleGroups() )
    {
        c |= static_cast<int>( MeshComponents::FaceGroups );
    }
    return c;
}

void DynamicMesh3::EnableMeshComponents( int MeshComponentsFlags )
{
    if ( ( static_cast<int>( MeshComponents::FaceGroups ) & MeshComponentsFlags ) != 0 )
    {
        EnableTriangleGroups( 0 );
    }
    else
    {
        DiscardTriangleGroups();
    }
    if ( ( static_cast<int>( MeshComponents::VertexColors ) & MeshComponentsFlags ) != 0 )
    {
        EnableVertexColors( glm::vec3( 1, 1, 1 ) );
    }
    else
    {
        DiscardVertexColors();
    }
    if ( ( static_cast<int>( MeshComponents::VertexNormals ) & MeshComponentsFlags ) != 0 )
    {
        EnableVertexNormals( glm::vec3( 0, 1, 0 ) );
    }
    else
    {
        DiscardVertexNormals();
    }
    if ( ( static_cast<int>( MeshComponents::VertexUVs ) & MeshComponentsFlags ) != 0 )
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
    int const                 NV = MaxVertexID();
    NewNormals.Resize( NV );
    for ( int i = 0; i < NV; ++i )
    {
        NewNormals[i] = InitialNormal;
    }
    m_VertexNormals = std::move( NewNormals );
}

void DynamicMesh3::DiscardVertexNormals()
{
    m_VertexNormals.reset();
}

void DynamicMesh3::EnableVertexColors( const glm::vec3& InitialColor )
{
    if ( HasVertexColors() )
    {
        return;
    }
    m_VertexColors = DynamicVector<glm::vec3>();
    int const NV   = MaxVertexID();
    m_VertexColors->Resize( NV );
    for ( int i = 0; i < NV; ++i )
    {
        m_VertexColors.value()[i] = InitialColor;
    }
}

void DynamicMesh3::DiscardVertexColors()
{
    m_VertexColors.reset();
}

void DynamicMesh3::EnableVertexUVs( const glm::vec2& InitialUV )
{
    if ( HasVertexUVs() )
    {
        return;
    }
    m_VertexUVs = DynamicVector<glm::vec2>();
    int const NV = MaxVertexID();
    m_VertexUVs->Resize( NV );
    for ( int i = 0; i < NV; ++i )
    {
        m_VertexUVs.value()[i] = InitialUV;
    }
}

void DynamicMesh3::DiscardVertexUVs()
{
    m_VertexUVs.reset();
}

void DynamicMesh3::EnableTriangleGroups( int InitialGroup )
{
    if ( HasTriangleGroups() )
    {
        return;
    }
    assert( InitialGroup >= 0 );
    m_TriangleGroups = DynamicVector<int>();
    int const NT     = MaxTriangleID();
    m_TriangleGroups->Resize( NT );
    for ( int i = 0; i < NT; ++i )
    {
        m_TriangleGroups.value()[i] = InitialGroup;
    }
    m_GroupIDCounter = InitialGroup + 1;
}

void DynamicMesh3::DiscardTriangleGroups()
{
    m_TriangleGroups.reset();
    m_GroupIDCounter = 0;
}

bool DynamicMesh3::GetVertex( int VertexID, VertexInfo& VertInfo, bool bWantNormals, bool bWantColors,
                              bool bWantUVs ) const
{
    if ( !m_VertexRefCounts.IsValid( VertexID ) )
    {
        return false;
    }
    VertInfo.Position = m_Vertices[VertexID];
    VertInfo.bHaveN = VertInfo.bHaveUV = VertInfo.bHaveC = false;
    if ( m_VertexNormals.has_value() && bWantNormals )
    {
        VertInfo.bHaveN                            = true;
        const DynamicVector<glm::vec3>& NormalVec  = m_VertexNormals.value();
        VertInfo.Normal                            = NormalVec[VertexID];
    }
    if ( m_VertexColors.has_value() && bWantColors )
    {
        VertInfo.bHaveC                           = true;
        const DynamicVector<glm::vec3>& ColorVec  = m_VertexColors.value();
        VertInfo.Color                            = ColorVec[VertexID];
    }
    if ( m_VertexUVs.has_value() && bWantUVs )
    {
        VertInfo.bHaveUV                       = true;
        const DynamicVector<glm::vec2>& UVVec  = m_VertexUVs.value();
        VertInfo.UV                            = UVVec[VertexID];
    }
    return true;
}

int DynamicMesh3::GetMaxVtxEdgeCount() const
{
    int max = 0;
    for ( int const vid : VertexIndicesItr() )
    {
        max = std::max( max, m_VertexEdgeLists.GetCount( vid ) );
    }
    return max;
}

VertexInfo DynamicMesh3::GetVertexInfo( int VertexID ) const
{
    VertexInfo vi  = VertexInfo();
    vi.Position    = GetVertex( VertexID );
    vi.bHaveN = vi.bHaveC = vi.bHaveUV = false;
    if ( HasVertexNormals() )
    {
        vi.bHaveN = true;
        vi.Normal = GetVertexNormal( VertexID );
    }
    if ( HasVertexColors() )
    {
        vi.bHaveC = true;
        vi.Color  = GetVertexColor( VertexID );
    }
    if ( HasVertexUVs() )
    {
        vi.bHaveUV = true;
        vi.UV      = GetVertexUV( VertexID );
    }
    return vi;
}

Index3i DynamicMesh3::GetTriNeighbourTris( int TriangleID ) const
{
    if ( m_TriangleRefCounts.IsValid( TriangleID ) )
    {
        Index3i nbr_t = Index3i::Zero();
        for ( int j = 0; j < 3; ++j )
        {
            Edge Edge = m_Edges[m_TriangleEdges[TriangleID][j]];
            nbr_t[j]  = ( Edge.Tri[0] == TriangleID ) ? Edge.Tri[1] : Edge.Tri[0];
        }
        return nbr_t;
    }

    return InvalidTriangle;
}

void DynamicMesh3::EnumerateVertexTriangles( int32_t VertexID, std::function<void( int32_t )> ApplyFunc ) const
{
    assert( m_VertexRefCounts.IsValid( VertexID ) );
    if ( !IsVertex( VertexID ) )
    {
        return;
    }

    m_VertexEdgeLists.Enumerate( VertexID,
                                 [&]( int32_t eid )
                                 {
                                     const Edge Edge   = m_Edges[eid];
                                     const int  vOther = Edge.Vert.A == VertexID ? Edge.Vert.B : Edge.Vert.A;
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
    assert( m_VertexRefCounts.IsValid( VID ) );
    if ( !IsVertex( VID ) || m_VertexEdgeLists.GetCount( VID ) == 0 )
    {
        return IndexConstants::InvalidID;
    }
    return m_VertexEdgeLists.First( VID );
}

void DynamicMesh3::EnumerateEdgeTriangles( int32_t EdgeID, const std::function<void( int32_t )>& ApplyFunc ) const
{
    assert( m_EdgeRefCounts.IsValid( EdgeID ) );
    if ( IsEdge( EdgeID ) )
    {
        const Edge Edge = m_Edges[EdgeID];
        ApplyFunc( Edge.Tri.A );
        if ( Edge.Tri.B != IndexConstants::InvalidID )
        {
            ApplyFunc( Edge.Tri.B );
        }
    }
}

size_t DynamicMesh3::GetByteCount() const
{
    size_t const ByteCount = m_VertexRefCounts.GetByteCount() + m_VertexEdgeLists.GetByteCount() +
                             m_TriangleRefCounts.GetByteCount() + m_EdgeRefCounts.GetByteCount();
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
    CheckOrFailF( !m_VertexRefCounts.IsDense() ||
                  ( m_VertexRefCounts.IsDense() && m_VertexRefCounts.GetCount() == m_Vertices.Num() ) );
    CheckOrFailF( !m_TriangleRefCounts.IsDense() ||
                  ( m_TriangleRefCounts.IsDense() && m_TriangleRefCounts.GetCount() == m_Triangles.Num() ) );

    // TriangleEdges should be 1:1 with Triangles
    CheckOrFailF( m_TriangleEdges.Num() == m_Triangles.Num() );

    for ( int const tID : TriangleIndicesItr() )
    {
        CheckOrFailF( IsTriangle( tID ) );
        CheckOrFailF( m_TriangleRefCounts.GetRefCount( tID ) == 1 );

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
            int const a = tv[j];
            int const b = tv[( j + 1 ) % 3];
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
            int const eid = te[j];
            CheckOrFailF( IsEdge( eid ) );
            int const tOther = GetOtherEdgeTriangle( eid, tID );
            if ( tOther == InvalidID )
            {
                CheckOrFailF( IsBoundaryTriangle( tID ) );
                continue;
            }

            CheckOrFailF( TriHasNeighbourTri( tOther, tID ) );

            // edge must have same two verts as tri for same index
            int const a  = tv[j];
            int const b  = tv[( j + 1 ) % 3];
            Index2i  ev = GetEdgeV( te[j] );
            CheckOrFailF( IndexUtil::SamePairUnordered( a, b, ev[0], ev[1] ) );

            // also check that nbr edge has opposite orientation
            if ( !Options.bAllowAdjacentFacesReverseOrientation )
            {
                Index3i const othertv = GetTriangle( tOther );
                int const     found   = IndexUtil::FindTriOrderedEdge( b, a, othertv );
                CheckOrFailF( found != InvalidID );
            }
        }
    }

    if ( HasTriangleGroups() )
    {
        const DynamicVector<int>& Groups = m_TriangleGroups.value();
        // must have a group per triangle ID
        CheckOrFailF( static_cast<int>( Groups.Num() ) == MaxTriangleID() );
        // group IDs must be in range [0, GroupIDCounter)
        for ( int const TID : TriangleIndicesItr() )
        {
            CheckOrFailF( Groups[TID] >= 0 );
            CheckOrFailF( Groups[TID] < m_GroupIDCounter );
        }
    }

    // edge verts/tris must exist
    for ( int const eID : EdgeIndicesItr() )
    {
        CheckOrFailF( IsEdge( eID ) );
        CheckOrFailF( m_EdgeRefCounts.GetRefCount( eID ) == 1 );
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
    bool const is_compact = m_VertexRefCounts.IsDense();
    if ( is_compact )
    {
        for ( int vid = 0; vid < static_cast<int>( m_Vertices.GetLength() ); ++vid )
        {
            CheckOrFailF( m_VertexRefCounts.IsValid( vid ) );
        }
    }

    // vertex edges must exist and reference this vert
    for ( int const vID : VertexIndicesItr() )
    {
        CheckOrFailF( IsVertex( vID ) );

        glm::dvec3 const v = GetVertex( vID );
        CheckOrFailF( !std::isnan( glm::length2( v ) ) );
        CheckOrFailF( std::isfinite( glm::length2( v ) ) );

        for ( int const edgeid : m_VertexEdgeLists.Values( vID ) )
        {
            CheckOrFailF( IsEdge( edgeid ) );
            CheckOrFailF( EdgeHasVertex( edgeid, vID ) );

            int const otherV = GetOtherEdgeVertex( edgeid, vID );
            int e2     = FindEdge( vID, otherV );
            CheckOrFailF( e2 != InvalidID );
            CheckOrFailF( e2 == edgeid );
            e2 = FindEdge( otherV, vID );
            CheckOrFailF( e2 != InvalidID );
            CheckOrFailF( e2 == edgeid );
        }

        for ( int const nbr_vid : VtxVerticesItr( vID ) )
        {
            CheckOrFailF( IsVertex( nbr_vid ) );
            int const edge = FindEdge( vID, nbr_vid );
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
        int32_t const VertexRefCount = m_VertexRefCounts.GetRefCount( vID );
        CheckOrFailF( VertexRefCount == ( static_cast<int32_t>( vTris.size() ) + 1 ) );
        CheckOrFailF( triToVtxRefs[vID] == static_cast<int32_t>( vTris.size() ) );
        for ( int const tID : vTris )
        {
            CheckOrFailF( TriangleHasVertex( tID, vID ) );
        }

        // check that edges around vert only references tris above, and reference all of them!
        std::vector<int> vRemoveTris( vTris );
        for ( int const edgeid : m_VertexEdgeLists.Values( vID ) )
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
    int const tid = m_TriangleRefCounts.Allocate();
    m_Triangles.InsertAt( Index3i( a, b, c ), tid );
    m_TriangleEdges.InsertAt( Index3i( e0, e1, e2 ), tid );
    return tid;
}

int DynamicMesh3::ReplaceEdgeVertex( int eID, int vOld, int vNew )
{
    Index2i&  Verts = m_Edges[eID].Vert;
    int const a     = Verts[0];
    int const b     = Verts[1];
    if ( a == vOld )
    {
        Verts[0] = std::min( b, vNew );
        Verts[1] = std::max( b, vNew );
        return 0;
    }
    if ( b == vOld )
    {
        Verts[0] = std::min( a, vNew );
        Verts[1] = std::max( a, vNew );
        return 1;
    }
    return -1;
}

int DynamicMesh3::ReplaceEdgeTriangle( int eID, int tOld, int tNew )
{
    Index2i&  Tris = m_Edges[eID].Tri;
    int const a    = Tris[0];
    int const b    = Tris[1];
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
    if ( b == tOld )
    {
        Tris[1] = tNew;
        return 1;
    }
    return -1;
}

int DynamicMesh3::ReplaceTriangleEdge( int TriangleID, int eOld, int eNew )
{
    Index3i& TriEdgeIDs = m_TriangleEdges[TriangleID];
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
int DynamicMesh3::FindTriangleEdge( int TriangleID, int vA, int vB ) const
{
    const Index3i Triangle = m_Triangles[TriangleID];
    if ( IndexUtil::SamePairUnordered( Triangle[0], Triangle[1], vA, vB ) )
        return m_TriangleEdges[TriangleID][0];
    if ( IndexUtil::SamePairUnordered( Triangle[1], Triangle[2], vA, vB ) )
        return m_TriangleEdges[TriangleID][1];
    if ( IndexUtil::SamePairUnordered( Triangle[2], Triangle[0], vA, vB ) )
        return m_TriangleEdges[TriangleID][2];
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
    return m_VertexEdgeLists.Find(
         vMin,
         [&]( int32_t eid )
         {
             const Edge Edge = m_Edges[eid];
             if ( Edge.Vert[1] == vMax )
             {
                 bIsBoundary = ( Edge.Tri[1] == InvalidID );
                 return true;
             }
             return false;
         },
         InvalidID );
}

int DynamicMesh3::FindEdge( int VertexA, int VertexB ) const
{
    assert( IsVertex( VertexA ) );
    assert( IsVertex( VertexB ) );
    if ( VertexA == VertexB )
    {
        // self-edges are not allowed, and if we fall through to the search below on a self edge we will
        // incorrectly sometimes return an arbitrary edge if queried for a self-edge, due to the optimization of
        // only checking one side of the edge
        return InvalidID;
    }

    // edge vertices must be sorted (min,max),
    //   that means we only need one index-check in inner loop.
    int32_t vMax = VertexA;
    int32_t vMin = VertexB;
    if ( VertexB > VertexA )
    {
        vMax = VertexB;
        vMin = VertexA;
    }
    if ( IsVertex( vMin ) )
    {
        return m_VertexEdgeLists.Find(
             vMin, [&]( int32_t eid ) { return ( m_Edges[eid].Vert[1] == vMax ); }, InvalidID );
    }

    return InvalidID;

    // this is slower, likely because it creates func<> every time. can we do w/o that?
    // return VertexEdgeLists.Find(vI, (eid) => { return Edges[4 * eid + 1] == vO; }, InvalidID);
}

int DynamicMesh3::FindEdgeFromTri( int VertexA, int VertexB, int TriangleID ) const
{
    const Index3i& Triangle        = m_Triangles[TriangleID];
    const Index3i& TriangleEdgeIDs = m_TriangleEdges[TriangleID];
    if ( IndexUtil::SamePairUnordered( VertexA, VertexB, Triangle[0], Triangle[1] ) )
    {
        return TriangleEdgeIDs[0];
    }
    if ( IndexUtil::SamePairUnordered( VertexA, VertexB, Triangle[1], Triangle[2] ) )
    {
        return TriangleEdgeIDs[1];
    }
    if ( IndexUtil::SamePairUnordered( VertexA, VertexB, Triangle[2], Triangle[0] ) )
    {
        return TriangleEdgeIDs[2];
    }
    return InvalidID;
}

int DynamicMesh3::FindEdgeFromTriPair( int TriA, int TriangleB ) const
{
    if ( m_TriangleRefCounts.IsValid( TriA ) && m_TriangleRefCounts.IsValid( TriangleB ) )
    {
        for ( int j = 0; j < 3; ++j )
        {
            int const   EdgeID = m_TriangleEdges[TriA][j];
            const Edge  Edge   = m_Edges[EdgeID];
            int const   NbrT   = ( Edge.Tri[0] == TriA ) ? Edge.Tri[1] : Edge.Tri[0];
            if ( NbrT == TriangleB )
            {
                return EdgeID;
            }
        }
    }
    return InvalidID;
}
