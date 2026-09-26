// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/DynamicMesh/DynamicMeshOverlay.cpp:1-2196,
// adapted: UE Core via UECore.hpp, namespace Desert::Geometry, TUniquePtr is std::unique_ptr; FArchive
// serialization not ported; ParallelFor runs serially; TStaticArray is std::array; TInlineAllocator arrays are
// plain TArray.
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshOverlay.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"

#include <array>
#include <Common/Core/Core.hpp>

using namespace Desert::Geometry;

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::ClearElements()
{
    m_Elements.Clear();
    m_ElementsRefCounts = RefCountVector();
    m_ParentVertices.Clear();
    InitializeTriangles( m_ParentMesh->MaxTriangleID() );
}

template <typename RealType, int ElementSize>
int DynamicMeshOverlay<RealType, ElementSize>::AppendElement( RealType ConstantValue )
{
    int vid = m_ElementsRefCounts.Allocate();
    int i   = ElementSize * vid;
    for ( int k = ElementSize - 1; k >= 0; --k )
    {
        m_Elements.InsertAt( ConstantValue, i + k );
    }
    m_ParentVertices.InsertAt( DynamicMesh3::InvalidID, vid );

    // updateTimeStamp(true);
    return vid;
}

template <typename RealType, int ElementSize>
int DynamicMeshOverlay<RealType, ElementSize>::AppendElement( const RealType* Value )
{
    int vid = m_ElementsRefCounts.Allocate();
    int i   = ElementSize * vid;

    // insert in reverse order so that Resize() is only called once
    for ( int k = ElementSize - 1; k >= 0; --k )
    {
        m_Elements.InsertAt( Value[k], i + k );
    }
    m_ParentVertices.InsertAt( DynamicMesh3::InvalidID, vid );

    // updateTimeStamp(true);
    return vid;
}

template <typename RealType, int ElementSize>
MeshResult DynamicMeshOverlay<RealType, ElementSize>::InsertElement( int ElementID, const RealType* Value,
                                                                     bool bUnsafe )
{
    if ( m_ElementsRefCounts.IsValid( ElementID ) )
    {
        return MeshResult::Failed_VertexAlreadyExists;
    }

    bool bOK = ( bUnsafe ) ? m_ElementsRefCounts.AllocateAtUnsafe( ElementID )
                           : m_ElementsRefCounts.AllocateAt( ElementID );
    if ( bOK == false )
    {
        return MeshResult::Failed_CannotAllocateVertex;
    }

    int i = ElementSize * ElementID;
    // insert in reverse order so that Resize() is only called once
    for ( int k = ElementSize - 1; k >= 0; --k )
    {
        m_Elements.InsertAt( Value[k], i + k );
    }

    m_ParentVertices.InsertAt( DynamicMesh3::InvalidID, ElementID, DynamicMesh3::InvalidID );

    // UpdateTimeStamp(true, true);
    return MeshResult::Ok;
}

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::CreateFromPredicate(
     const std::function<bool( int ParentVertexIdx, int TriIDA, int TriIDB )>& TrisCanShareVertexPredicate,
     RealType                                                                  InitElementValue )
{
    ClearElements(); // deletes all elements and initializes triangles to be 1:1 w/ parentmesh IDs
    DynamicMesh3::LocalIntArray  TrisActiveSubGroup, AppendedElements;
    DynamicMesh3::LocalIntArray  TriangleIDs, TriangleContigGroupLens;
    DynamicMesh3::LocalBoolArray GroupIsLoop;
    for ( int VertexID : m_ParentMesh->VertexIndicesItr() )
    {

        bool bActiveSubGroupBroken = false;
        m_ParentMesh->GetVtxContiguousTriangles( VertexID, TriangleIDs, TriangleContigGroupLens, GroupIsLoop );
        int GroupStart = 0;
        for ( int GroupIdx = 0; GroupIdx < static_cast<int32_t>( TriangleContigGroupLens.size() ); GroupIdx++ )
        {
            bool bIsLoop  = GroupIsLoop[GroupIdx];
            int  GroupNum = TriangleContigGroupLens[GroupIdx];
            if ( !Common::EnsureOrWarn(
                      GroupNum > 0,
                      "GroupNum > 0" ) ) // sanity check; groups should always have at least one element
            {
                continue;
            }

            TrisActiveSubGroup.clear();
            AppendedElements.clear();
            TrisActiveSubGroup.resize( GroupNum );
            int CurrentGroupID        = 0;
            int CurrentGroupRefSubIdx = 0;
            for ( int TriSubIdx = 0; TriSubIdx + 1 < GroupNum; TriSubIdx++ )
            {
                int  TriIDA    = TriangleIDs[GroupStart + TriSubIdx];
                int  TriIDB    = TriangleIDs[GroupStart + TriSubIdx + 1];
                bool bCanShare = TrisCanShareVertexPredicate( VertexID, TriIDA, TriIDB );
                if ( !bCanShare )
                {
                    CurrentGroupID++;
                    CurrentGroupRefSubIdx = TriSubIdx + 1;
                }

                TrisActiveSubGroup[TriSubIdx + 1] = CurrentGroupID;
            }

            // for loops, merge first and last group if needed
            int NumGroupID = CurrentGroupID + 1;
            if ( bIsLoop && TrisActiveSubGroup[0] != TrisActiveSubGroup.back() )
            {
                if ( TrisCanShareVertexPredicate( VertexID, TriangleIDs[GroupStart],
                                                  TriangleIDs[GroupStart + GroupNum - 1] ) )
                {
                    int EndGroupID   = TrisActiveSubGroup[GroupNum - 1];
                    int StartGroupID = TrisActiveSubGroup[0];
                    int TriID0       = TriangleIDs[GroupStart];

                    for ( int Idx = GroupNum - 1; Idx >= 0 && TrisActiveSubGroup[Idx] == EndGroupID; Idx-- )
                    {
                        TrisActiveSubGroup[Idx] = StartGroupID;
                    }
                    NumGroupID--;
                }
            }

            for ( int Idx = 0; Idx < NumGroupID; Idx++ )
            {
                AppendedElements.push_back( AppendElement( InitElementValue ) );
            }
            for ( int TriSubIdx = 0; TriSubIdx < GroupNum; TriSubIdx++ )
            {
                int      TriID        = TriangleIDs[GroupStart + TriSubIdx];
                Index3i  TriVertIDs   = m_ParentMesh->GetTriangle( TriID );
                int      VertSubIdx   = IndexUtil::FindTriIndex( VertexID, TriVertIDs );
                int      i            = 3 * TriID;
                int      ElementIndex = AppendedElements[TrisActiveSubGroup[TriSubIdx]];
                m_ElementTriangles.InsertAt( ElementIndex, i + VertSubIdx, DynamicMesh3::InvalidID );
                m_ElementsRefCounts.Increment( ElementIndex );
                m_ParentVertices.InsertAt( VertexID, ElementIndex ); // elements were appended one-by-one above, so
                                                                     // default initialization not needed here
            }
            GroupStart += GroupNum;
        }
    }
}

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::CreatePerVertex( RealType InitElementValue, bool bExactMapIDs )
{
    ClearElements(); // deletes all elements and initializes triangles to be 1:1 w/ parentmesh IDs
    std::array<RealType, ElementSize> DefaultElement;
    for ( int32_t Idx = 0; Idx < ElementSize; ++Idx )
    {
        DefaultElement[Idx] = InitElementValue;
    }
    std::vector<int32_t> VIDtoEID;
    bool                 bNeedsRemap = !bExactMapIDs && !m_ParentMesh->IsCompactV();
    if ( bNeedsRemap )
    {
        VIDtoEID.resize( m_ParentMesh->MaxVertexID() );
    }
    for ( int32_t const VertexID : m_ParentMesh->VertexIndicesItr() )
    {
        int32_t ElementID = DynamicMesh3::InvalidID;
        if ( bExactMapIDs )
        {
            ElementID          = VertexID;
            MeshResult Result  = InsertElement( VertexID, DefaultElement.data() );
            assert( Result == MeshResult::Ok ); // because we allocate in increasing sequential order,
                                                // shouldn't be possible InsertElement to return failure
        }
        else
        {
            ElementID = AppendElement( InitElementValue );
        }
        m_ParentVertices.InsertAt( VertexID, ElementID, DynamicMesh3::InvalidID );
        if ( bNeedsRemap )
        {
            VIDtoEID[VertexID] = ElementID;
        }
        else
        {
            assert( VertexID == ElementID );
        }
    }

    for ( int32_t const TriangleID : m_ParentMesh->TriangleIndicesItr() )
    {
        Index3i       Tri   = m_ParentMesh->GetTriangle( TriangleID );
        int32_t const Start = 3 * TriangleID;
        for ( int32_t SubIdx = 0; SubIdx < 3; ++SubIdx )
        {
            int32_t const VID = Tri[SubIdx];
            int32_t const EID = bNeedsRemap ? VIDtoEID[VID] : VID;
            m_ElementTriangles.InsertAt( EID, Start + SubIdx, DynamicMesh3::InvalidID );
            m_ElementsRefCounts.Increment( EID );
        }
    }
}

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::SplitVerticesWithPredicate(
     const std::function<bool( int ElementID, int TriID )>&              ShouldSplitOutVertex,
     std::function<void( int ElementID, int TriID, RealType* FillVect )> GetNewElementValue )
{
    for ( int TriID : m_ParentMesh->TriangleIndicesItr() )
    {
        Index3i ElTri = GetTriangle( TriID );
        if ( ElTri.A < 0 )
        {
            // skip un-set triangles
            continue;
        }
        bool TriChanged = false;
        for ( int SubIdx = 0; SubIdx < 3; SubIdx++ )
        {
            int ElementID = ElTri[SubIdx];
            // by convention for overlays, a ref count of 2 means that only one triangle has the element -- can't
            // split it out further
            if ( m_ElementsRefCounts.GetRefCount( ElementID ) <= 2 )
            {
                // still set the new value though if the function wants to change it
                if ( ShouldSplitOutVertex( ElementID, TriID ) )
                {
                    RealType NewElementData[ElementSize];
                    GetNewElementValue( ElementID, TriID, NewElementData );
                    SetElement( ElementID, NewElementData );
                }
            }
            if ( ShouldSplitOutVertex( ElementID, TriID ) )
            {
                TriChanged = true;
                RealType NewElementData[ElementSize];
                GetNewElementValue( ElementID, TriID, NewElementData );
                ElTri[SubIdx] = AppendElement( NewElementData );
            }
        }
        if ( TriChanged )
        {
            InternalSetTriangle( TriID, ElTri, true );
        }
    }
}

template <typename RealType, int ElementSize>
bool DynamicMeshOverlay<RealType, ElementSize>::MergeElement( int SourceElementID, int TargetElementID )
{
    if ( SourceElementID == TargetElementID )
    {
        return false;
    }

    int SourceParentID = m_ParentVertices[SourceElementID];
    int TargetParentID = m_ParentVertices[TargetElementID];

    auto MergeElementForTriangle = [this, SourceElementID, TargetElementID]( int32_t TriID )
    {
        int ElementTriStart = TriID * 3;
        for ( int SubIdx = 0; SubIdx < 3; SubIdx++ )
        {
            int CurElID = m_ElementTriangles[ElementTriStart + SubIdx];
            if ( CurElID == SourceElementID )
            {
                m_ElementsRefCounts.Decrement( SourceElementID );
                m_ElementsRefCounts.Increment( TargetElementID );
                m_ElementTriangles[ElementTriStart + SubIdx] = TargetElementID;
            }
        }
    };

    assert( SourceParentID == TargetParentID );
    if ( SourceParentID != TargetParentID )
    {
        return false;
    }

    m_ParentMesh->EnumerateVertexTriangles( SourceParentID, MergeElementForTriangle );

    assert( m_ElementsRefCounts.IsValid( SourceElementID ) );
    assert( m_ElementsRefCounts.GetRefCount( SourceElementID ) == 1 );
    if ( m_ElementsRefCounts.GetRefCount( SourceElementID ) == 1 )
    {
        m_ElementsRefCounts.Decrement( SourceElementID );
        m_ParentVertices[SourceElementID] = DynamicMesh3::InvalidID;
    }

    return true;
}

template <typename RealType, int ElementSize>
int DynamicMeshOverlay<RealType, ElementSize>::SplitElement( int                         ElementID,
                                                             const std::span<const int>& TrianglesToUpdate )
{
    int ParentID = m_ParentVertices[ElementID];
    return SplitElementWithNewParent( ElementID, ParentID, TrianglesToUpdate );
}

template <typename RealType, int ElementSize>
int DynamicMeshOverlay<RealType, ElementSize>::SplitElementWithNewParent(
     int ElementID, int NewParentID, const std::span<const int>& TrianglesToUpdate )
{
    RealType SourceData[ElementSize];
    GetElement( ElementID, SourceData );
    int NewElID = AppendElement( SourceData );
    for ( int TriID : TrianglesToUpdate )
    {
        int ElementTriStart = TriID * 3;
        for ( int SubIdx = 0; SubIdx < 3; SubIdx++ )
        {
            int CurElID = m_ElementTriangles[ElementTriStart + SubIdx];
            if ( CurElID == ElementID )
            {
                m_ElementsRefCounts.Decrement( ElementID );
                m_ElementsRefCounts.Increment( NewElID );
                m_ElementTriangles[ElementTriStart + SubIdx] = NewElID;
            }
        }
    }
    m_ParentVertices.InsertAt( NewParentID, NewElID, DynamicMesh3::InvalidID );

    assert( m_ElementsRefCounts.IsValid( ElementID ) );

    // An element may have become isolated after changing all of its incident triangles. Delete such an element.
    if ( m_ElementsRefCounts.GetRefCount( ElementID ) == 1 )
    {
        m_ElementsRefCounts.Decrement( ElementID );
        m_ParentVertices[ElementID] = DynamicMesh3::InvalidID;
    }

    return NewElID;
}

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::SplitBowties( bool bParallel )
{

    if ( bParallel )
    {
        // Bowties are typically rare, so we can test for bowties in parallel and only run the split on the
        // (hopefully) few needed cases
        std::vector<int32_t> BowtieVerts;
        for ( int32_t VertexID = 0; VertexID < m_ParentMesh->MaxVertexID(); ++VertexID )
        {
            if ( m_ParentMesh->IsVertex( VertexID ) && IsBowtieInOverlay( VertexID ) )
            {
                BowtieVerts.push_back( VertexID );
            }
        }
        for ( int32_t const VertexID : BowtieVerts )
        {
            SplitBowtiesAtVertex( VertexID );
        }
    }
    else
    {
        for ( int VertexID : m_ParentMesh->VertexIndicesItr() )
        {
            SplitBowtiesAtVertex( VertexID );
        }
    }
}

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::SplitBowtiesAtVertex( int32_t               VertexID,
                                                                      std::vector<int32_t>* NewElementIDs )
{
    // arrays for storing contiguous triangle groups from parentmesh
    DynamicMesh3::LocalIntArray  TrianglesOut, ContiguousGroupLengths;
    DynamicMesh3::LocalBoolArray GroupIsLoop;

    // per-vertex element group tracking data, reused in loop below
    DynamicMesh3::LocalIntArray ElementIDSeen;
    DynamicMesh3::LocalIntArray
         GroupElementIDs, // stores element IDs of this vertex, 1:1 w/ contiguous triangles in the parent mesh
         SubGroupID,      // mapping from GroupElementIDs indices into SubGroupElementIDs indices, giving subgroup
                          // membership per triangle
         SubGroupElementIDs; // 1:1 w/ 'subgroups' in the group (e.g. if all triangles in the group all had the
                             // same element ID, this would be an array of length 1, just containing that element
                             // ID)

    DESERT_VERIFY_WARN( MeshResult::Ok == m_ParentMesh->GetVtxContiguousTriangles(
                                               VertexID, TrianglesOut, ContiguousGroupLengths, GroupIsLoop ) );
    int32_t const NumTris = static_cast<int32_t>( TrianglesOut.size() );

    auto ElementIDFromTriangle = [VertexID, this]( int32_t TriID ) -> int32_t
    {
        Index3i  TriVIDs = m_ParentMesh->GetTriangle( TriID );
        Index3i  TriEIDs = GetTriangle( TriID );
        int      SubIdx  = TriVIDs.IndexOf( VertexID );
        return TriEIDs[SubIdx];
    };

    // Early out in the easy non-bowtie case of 1 element, 1 contiguous group
    if ( NumTris > 0 && static_cast<int32_t>( ContiguousGroupLengths.size() ) == 1 )
    {
        int32_t const FirstElSeen    = ElementIDFromTriangle( TrianglesOut[0] );
        bool  bSingleElement = true;
        for ( int32_t Idx = 1; Idx < static_cast<int32_t>( TrianglesOut.size() ); ++Idx )
        {
            if ( FirstElSeen != ElementIDFromTriangle( TrianglesOut[Idx] ) )
            {
                bSingleElement = false;
            }
        }
        if ( bSingleElement )
        {
            return;
        }
    }

    ElementIDSeen.clear();
    // per contiguous group of triangles around vertex in ParentMesh, find contiguous sub-groups in overlay
    for ( int32_t GroupIdx = 0, NumGroups = static_cast<int32_t>( ContiguousGroupLengths.size() ), TriSubStart = 0;
          GroupIdx < NumGroups; GroupIdx++ )
    {
        bool bIsLoop       = GroupIsLoop[GroupIdx];
        int  TriInGroupNum = ContiguousGroupLengths[GroupIdx];
        if ( Common::EnsureOrWarn( TriInGroupNum > 0, "TriInGroupNum > 0" ) == false )
        {
            continue;
        }
        int TriSubEnd = TriSubStart + TriInGroupNum;

        GroupElementIDs.clear();
        for ( int TriSubIdx = TriSubStart; TriSubIdx < TriSubEnd; TriSubIdx++ )
        {
            int TriID = TrianglesOut[TriSubIdx];
            GroupElementIDs.push_back( ElementIDFromTriangle( TriID ) );
        }

        auto IsConnected = [this, &GroupElementIDs, &TrianglesOut, &TriSubStart]( int TriOutIdxA, int TriOutIdxB )
        {
            if ( GroupElementIDs[TriOutIdxA - TriSubStart] != GroupElementIDs[TriOutIdxB - TriSubStart] )
            {
                return false;
            }
            int EdgeID = m_ParentMesh->FindEdgeFromTriPair( TrianglesOut[TriOutIdxA], TrianglesOut[TriOutIdxB] );
            return EdgeID >= 0 && !IsSeamEdge( EdgeID );
        };

        SubGroupID.clear();
        SubGroupID.resize( TriInGroupNum );
        SubGroupElementIDs.clear();
        int MaxSubID  = 0;
        SubGroupID[0] = 0;
        SubGroupElementIDs.push_back( GroupElementIDs[0] );

        // Iterate through tris in current group, except last one
        for ( int TriSubIdx = TriSubStart; TriSubIdx + 1 < TriSubEnd; TriSubIdx++ )
        {
            if ( !IsConnected( TriSubIdx, TriSubIdx + 1 ) )
            {
                SubGroupElementIDs.push_back( GroupElementIDs[TriSubIdx + 1 - TriSubStart] );
                MaxSubID++;
            }
            SubGroupID[TriSubIdx - TriSubStart + 1] = MaxSubID;
        }
        // if group was a loop, need to check if the last sub-group and first sub-group were actually the same
        // group
        if ( bIsLoop && MaxSubID > 0 && IsConnected( TriSubStart, TriSubStart + TriInGroupNum - 1 ) )
        {
            int LastGroupID = SubGroupID.back();
            for ( int32_t Idx = static_cast<int32_t>( SubGroupID.size() ) - 1;
                  Idx >= 0 && SubGroupID[Idx] == LastGroupID; Idx-- )
            {
                SubGroupID[Idx] = 0;
            }
            MaxSubID--;
            SubGroupElementIDs.pop_back();
        }

        for ( int SubID = 0; SubID < static_cast<int32_t>( SubGroupElementIDs.size() ); SubID++ )
        {
            int ElementID = SubGroupElementIDs[SubID];
            if ( ElementID < 0 )
            {
                continue; // skip if this is an invalid ElementID (eg from an invalid triangle)
            }
            // split needed the *second* time we see a sub-group using a given ElementID
            if ( ( std::find( ElementIDSeen.begin(), ElementIDSeen.end(), ElementID ) != ElementIDSeen.end() ) )
            {
                DynamicMesh3::LocalIntArray ConnectedTris;
                for ( int TriSubIdx = TriSubStart; TriSubIdx < TriSubEnd; TriSubIdx++ )
                {
                    if ( SubID == SubGroupID[TriSubIdx - TriSubStart] )
                    {
                        ConnectedTris.push_back( TrianglesOut[TriSubIdx] );
                    }
                }
                int32_t const NewElementID = SplitElement( ElementID, ConnectedTris );
                if ( NewElementIDs )
                {
                    NewElementIDs->push_back( NewElementID );
                }
            }
            else
            {
                ElementIDSeen.push_back( ElementID );
            }
        }

        TriSubStart = TriSubEnd;
    }
}

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::InitializeTriangles( int MaxTriangleID )
{
    m_ElementTriangles.SetNum( MaxTriangleID * 3 );
    m_ElementTriangles.Fill( DynamicMesh3::InvalidID );
}

template <typename RealType, int ElementSize>
MeshResult DynamicMeshOverlay<RealType, ElementSize>::SetTriangle( int tid, const Index3i& tv,
                                                                   bool bAllowElementFreeing )
{
    if ( IsElement( tv[0] ) == false || IsElement( tv[1] ) == false || IsElement( tv[2] ) == false )
    {
        assert( false );
        return MeshResult::Failed_NotAVertex;
    }
    if ( tv[0] == tv[1] || tv[0] == tv[2] || tv[1] == tv[2] )
    {
        assert( false );
        return MeshResult::Failed_InvalidNeighbourhood;
    }

    if ( m_ParentMesh->IsTriangle( tid ) == false )
    {
        assert( false );
        return MeshResult::Failed_NotATriangle;
    }

    InternalSetTriangle( tid, tv, true, bAllowElementFreeing );

    // updateTimeStamp(true);
    return MeshResult::Ok;
}

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::FreeUnusedElements(
     const std::unordered_set<int>* ElementsToCheck )
{
    auto FreeIfUnused = [this]( int ElementID )
    {
        if ( m_ElementsRefCounts.IsValid( ElementID ) && m_ElementsRefCounts.GetRefCount( ElementID ) == 1 )
        {
            m_ElementsRefCounts.Decrement( ElementID );
            m_ParentVertices[ElementID] = DynamicMesh3::InvalidID;
        }
    };

    if ( ElementsToCheck )
    {
        for ( int ElementID : *ElementsToCheck )
        {
            FreeIfUnused( ElementID );
        }
    }
    else
    {
        for ( int ElementID : m_ElementsRefCounts.Indices() )
        {
            FreeIfUnused( ElementID );
        }
    }
}

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::UnsetTriangle( int TriangleID, bool bAllowElementFreeing )
{
    int i = 3 * TriangleID;
    if ( m_ElementTriangles[i] == DynamicMesh3::InvalidID )
    {
        return;
    }
    for ( int SubIdx = 0; SubIdx < 3; SubIdx++ )
    {
        m_ElementsRefCounts.Decrement( m_ElementTriangles[i + SubIdx] );

        if ( bAllowElementFreeing && m_ElementsRefCounts.GetRefCount( m_ElementTriangles[i + SubIdx] ) == 1 )
        {
            m_ElementsRefCounts.Decrement( m_ElementTriangles[i + SubIdx] );
            m_ParentVertices[m_ElementTriangles[i + SubIdx]] = DynamicMesh3::InvalidID;
        }
        m_ElementTriangles[i + SubIdx] = DynamicMesh3::InvalidID;
    }
}

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::InternalSetTriangle( int tid, const Index3i& tv,
                                                                     bool bUpdateRefCounts,
                                                                     bool bAllowElementFreeing )
{
    if ( Common::EnsureOrWarn( m_ParentMesh, "ParentMesh" ) == false )
    {
        return;
    }

    // If we have to decrement refcounts, we will do it at the end, because Decrement() frees
    // elements as soon as they lose their last reference, so a Decrement followed by Increment
    // can leave things in an invalid state.
    bool     bNeedToDecrement = false;
    Index3i  OldTriElements; // only used if need to decrement.

    int i = 3 * tid;

    // See if triangle existed and make it exist if not
    if ( !m_ElementTriangles.SetMinimumSize( i + 3, DynamicMesh3::InvalidID ) && bUpdateRefCounts )
    {
        OldTriElements   = GetTriangle( tid );
        bNeedToDecrement = ( OldTriElements[0] != DynamicMesh3::InvalidID );
    }

    m_ElementTriangles[i + 2] = tv[2];
    m_ElementTriangles[i + 1] = tv[1];
    m_ElementTriangles[i]     = tv[0];

    if ( bUpdateRefCounts )
    {
        m_ElementsRefCounts.Increment( tv[0] );
        m_ElementsRefCounts.Increment( tv[1] );
        m_ElementsRefCounts.Increment( tv[2] );

        if ( bNeedToDecrement )
        {
            for ( int j = 0; j < 3; ++j )
            {
                m_ElementsRefCounts.Decrement( OldTriElements[j] );

                if ( bAllowElementFreeing && m_ElementsRefCounts.GetRefCount( OldTriElements[j] ) == 1 )
                {
                    m_ElementsRefCounts.Decrement( OldTriElements[j] );
                    m_ParentVertices[OldTriElements[j]] = DynamicMesh3::InvalidID;
                }
            };
        }
    }

    if ( tv != DynamicMesh3::InvalidTriangle )
    {
        // Set parent vertex IDs
        const Index3i ParentTriangle = m_ParentMesh->GetTriangle( tid );

        for ( int VInd = 0; VInd < 3; ++VInd )
        {
            // Checks that the parent vertices of the elements that we're referencing in the overlay
            // triangle are either not yet set or already point to the vertices of the corresponding
            // mesh triangle (and so will remain unchanged). Remember that the same element is not
            // allowed to be used for multiple vertices.
            assert( m_ParentVertices[tv[VInd]] == ParentTriangle[VInd] ||
                    m_ParentVertices[tv[VInd]] == DynamicMesh3::InvalidID );

            m_ParentVertices.InsertAt( ParentTriangle[VInd], tv[VInd], DynamicMesh3::InvalidID );
        }
    }
}

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::InitializeNewTriangle( int tid )
{
    int i = 3 * tid;
    m_ElementTriangles.SetMinimumSize( i + 3, DynamicMesh3::InvalidID );
    m_ElementTriangles[i + 2] = DynamicMesh3::InvalidID;
    m_ElementTriangles[i + 1] = DynamicMesh3::InvalidID;
    m_ElementTriangles[i]     = DynamicMesh3::InvalidID;

    // updateTimeStamp(true);
}

template <typename RealType, int ElementSize>
bool DynamicMeshOverlay<RealType, ElementSize>::IsSeamEdge( int eid, bool* bIsNonIntersecting ) const
{
    if ( bIsNonIntersecting != nullptr )
    {
        *bIsNonIntersecting = false;
    }
    if ( m_ParentMesh->IsEdge( eid ) == false )
    {
        return false;
    }

    Index2i et = m_ParentMesh->GetEdgeT( eid );
    if ( et.B == DynamicMesh3::InvalidID )
    {
        if ( bIsNonIntersecting != nullptr )
        {
            Index2i  ev     = m_ParentMesh->GetEdgeV( eid );
            int      CountA = CountVertexElements( ev.A );
            int      CountB = CountVertexElements( ev.B );

            // will be false if another seam intersects is adjacent to either end of the seam edge
            *bIsNonIntersecting = ( CountA == 1 ) && ( CountB == 1 );
        }

        return true;
    }

    Index2i  ev     = m_ParentMesh->GetEdgeV( eid );
    int      base_a = ev.A, base_b = ev.B;

    bool bASet = IsSetTriangle( et.A ), bBSet = IsSetTriangle( et.B );
    if ( !bASet || !bBSet ) // if either triangle is unset, need different logic for checking if this is a seam
    {
        return ( bASet || bBSet ); // consider it a seam if only one is unset
    }

    Index3i Triangle0 = GetTriangle( et.A );
    Index3i  BaseTriangle0( m_ParentVertices[Triangle0.A], m_ParentVertices[Triangle0.B],
                            m_ParentVertices[Triangle0.C] );
    int      idx_base_a0 = BaseTriangle0.IndexOf( base_a );
    int      idx_base_b0 = BaseTriangle0.IndexOf( base_b );

    Index3i Triangle1 = GetTriangle( et.B );
    Index3i  BaseTriangle1( m_ParentVertices[Triangle1.A], m_ParentVertices[Triangle1.B],
                            m_ParentVertices[Triangle1.C] );
    int      idx_base_a1 = BaseTriangle1.IndexOf( base_a );
    int      idx_base_b1 = BaseTriangle1.IndexOf( base_b );

    int el_a_tri0 = Triangle0[idx_base_a0];
    int el_b_tri0 = Triangle0[idx_base_b0];
    int el_a_tri1 = Triangle1[idx_base_a1];
    int el_b_tri1 = Triangle1[idx_base_b1];

    bool bIsSeam = !IndexUtil::SamePairUnordered( el_a_tri0, el_b_tri0, el_a_tri1, el_b_tri1 );

    if ( bIsNonIntersecting != nullptr )
    {
        if ( ( el_a_tri0 == el_a_tri1 || el_a_tri0 == el_b_tri1 ) ||
             ( el_b_tri0 == el_b_tri1 || el_b_tri0 == el_a_tri1 ) )
        {
            // seam edge "intersects" with end of the seam
            *bIsNonIntersecting = false;
        }
        else
        {
            // check that exactly two elements are associated with the vertices at each end of the edge
            int CountA = CountVertexElements( base_a );
            int CountB = CountVertexElements( base_b );

            // will be false if another seam intersects is adjacent to either end of the seam edge
            *bIsNonIntersecting = ( CountA == 2 ) && ( CountB == 2 );
        }
    }

    return bIsSeam;

    // TODO: this doesn't seem to work but it should, and would be more efficient:
    //   - add ParentMesh->FindTriEdgeIndex(tid,eid)
    //   - SamePairUnordered query could directly index into ElementTriangles[]
    // Index3i TriangleA = GetTriangle(et.A);
    // Index3i TriangleB = GetTriangle(et.B);

    // Index3i BaseTriEdgesA = ParentMesh->GetTriEdges(et.A);
    // int WhichA = (BaseTriEdgesA.A == eid) ? 0 :
    //	((BaseTriEdgesA.B == eid) ? 1 : 2);

    // Index3i BaseTriEdgesB = ParentMesh->GetTriEdges(et.B);
    // int WhichB = (BaseTriEdgesB.A == eid) ? 0 :
    //	((BaseTriEdgesB.B == eid) ? 1 : 2);

    // return SamePairUnordered(
    //	TriangleA[WhichA], TriangleA[(WhichA + 1) % 3],
    //	TriangleB[WhichB], TriangleB[(WhichB + 1) % 3]);
}

template <typename RealType, int ElementSize>
bool DynamicMeshOverlay<RealType, ElementSize>::IsSeamEndEdge( int eid ) const
{
    if ( m_ParentMesh->IsEdge( eid ) == false )
    {
        return false;
    }

    Index2i et = m_ParentMesh->GetEdgeT( eid );
    if ( et.B == DynamicMesh3::InvalidID )
    {
        return false;
    }

    Index2i  ev     = m_ParentMesh->GetEdgeV( eid );
    int      base_a = ev.A, base_b = ev.B;

    bool bASet = IsSetTriangle( et.A ), bBSet = IsSetTriangle( et.B );
    if ( !bASet || !bBSet )
    {
        return false;
    }

    Index3i Triangle0 = GetTriangle( et.A );
    Index3i  BaseTriangle0( m_ParentVertices[Triangle0.A], m_ParentVertices[Triangle0.B],
                            m_ParentVertices[Triangle0.C] );
    int      idx_base_a0 = BaseTriangle0.IndexOf( base_a );
    int      idx_base_b0 = BaseTriangle0.IndexOf( base_b );

    Index3i Triangle1 = GetTriangle( et.B );
    Index3i  BaseTriangle1( m_ParentVertices[Triangle1.A], m_ParentVertices[Triangle1.B],
                            m_ParentVertices[Triangle1.C] );
    int      idx_base_a1 = BaseTriangle1.IndexOf( base_a );
    int      idx_base_b1 = BaseTriangle1.IndexOf( base_b );

    int el_a_tri0 = Triangle0[idx_base_a0];
    int el_b_tri0 = Triangle0[idx_base_b0];
    int el_a_tri1 = Triangle1[idx_base_a1];
    int el_b_tri1 = Triangle1[idx_base_b1];

    bool bIsSeam = !IndexUtil::SamePairUnordered( el_a_tri0, el_b_tri0, el_a_tri1, el_b_tri1 );

    bool bIsSeamEnd = false;
    if ( bIsSeam )
    {
        // is only one of elements split?
        if ( ( el_a_tri0 == el_a_tri1 || el_a_tri0 == el_b_tri1 ) ||
             ( el_b_tri0 == el_b_tri1 || el_b_tri0 == el_a_tri1 ) )
        {
            bIsSeamEnd = true;
        }
    }

    return bIsSeamEnd;
}

template <typename RealType, int ElementSize>
bool DynamicMeshOverlay<RealType, ElementSize>::HasInteriorSeamEdges() const
{
    for ( int eid : m_ParentMesh->EdgeIndicesItr() )
    {
        Index2i et = m_ParentMesh->GetEdgeT( eid );
        if ( et.B != DynamicMesh3::InvalidID )
        {
            bool bASet = IsSetTriangle( et.A ), bBSet = IsSetTriangle( et.B );
            if ( bASet != bBSet )
            {
                // seam between triangles with elements and triangles without
                return true;
            }
            else if ( !bASet )
            {
                // neither triangle has set elements
                continue;
            }
            Index2i  ev     = m_ParentMesh->GetEdgeV( eid );
            int      base_a = ev.A, base_b = ev.B;

            Index3i  Triangle0 = GetTriangle( et.A );
            Index3i  BaseTriangle0( m_ParentVertices[Triangle0.A], m_ParentVertices[Triangle0.B],
                                    m_ParentVertices[Triangle0.C] );
            int      idx_base_a1 = BaseTriangle0.IndexOf( base_a );
            int      idx_base_b1 = BaseTriangle0.IndexOf( base_b );

            Index3i  Triangle1 = GetTriangle( et.B );
            Index3i  BaseTriangle1( m_ParentVertices[Triangle1.A], m_ParentVertices[Triangle1.B],
                                    m_ParentVertices[Triangle1.C] );
            int      idx_base_a2 = BaseTriangle1.IndexOf( base_a );
            int      idx_base_b2 = BaseTriangle1.IndexOf( base_b );

            if ( !IndexUtil::SamePairUnordered( Triangle0[idx_base_a1], Triangle0[idx_base_b1],
                                                Triangle1[idx_base_a2], Triangle1[idx_base_b2] ) )
            {
                return true;
            }
        }
    }
    return false;
}

template <typename RealType, int ElementSize>
bool DynamicMeshOverlay<RealType, ElementSize>::IsSeamVertex( int vid, bool bBoundaryIsSeam ) const
{
    // @todo can we do this more efficiently? At minimum we are looking up each triangle twice...
    for ( int edgeid : m_ParentMesh->VtxEdgesItr( vid ) )
    {
        if ( !bBoundaryIsSeam && m_ParentMesh->IsBoundaryEdge( edgeid ) )
        {
            continue;
        }
        if ( IsSeamEdge( edgeid ) )
        {
            return true;
        }
    }
    return false;
}

template <typename RealType, int ElementSize>
bool DynamicMeshOverlay<RealType, ElementSize>::IsSeamIntersectionVertex( int32_t VertexID ) const
{
    int32_t SeamCount = 0;
    for ( int32_t const EdgeID : m_ParentMesh->VtxEdgesItr( VertexID ) )
    {
        SeamCount += int32_t( IsSeamEdge( EdgeID ) );
    }
    return SeamCount == 1 || SeamCount > 2;
}

template <typename RealType, int ElementSize>
bool DynamicMeshOverlay<RealType, ElementSize>::IsBowtieInOverlay( int32_t VertexID ) const
{
    // arrays for storing contiguous triangle groups from parentmesh
    DynamicMesh3::LocalIntArray  TrianglesOut, ContiguousGroupLengths;
    DynamicMesh3::LocalBoolArray GroupIsLoop;

    // per-vertex element group tracking data, reused in loop below
    DynamicMesh3::LocalIntArray ElementIDSeen;
    DynamicMesh3::LocalIntArray
         GroupElementIDs, // stores element IDs of this vertex, 1:1 w/ contiguous triangles in the parent mesh
         SubGroupID,      // mapping from GroupElementIDs indices into SubGroupElementIDs indices, giving subgroup
                          // membership per triangle
         SubGroupElementIDs; // 1:1 w/ 'subgroups' in the group (e.g. if all triangles in the group all had the
                             // same element ID, this would be an array of length 1, just containing that element
                             // ID)

    DESERT_VERIFY_WARN( MeshResult::Ok == m_ParentMesh->GetVtxContiguousTriangles(
                                               VertexID, TrianglesOut, ContiguousGroupLengths, GroupIsLoop ) );
    int32_t const NumTris = static_cast<int32_t>( TrianglesOut.size() );

    auto ElementIDFromTriangle = [VertexID, this]( int32_t TriID ) -> int32_t
    {
        Index3i  TriVIDs = m_ParentMesh->GetTriangle( TriID );
        Index3i  TriEIDs = GetTriangle( TriID );
        int      SubIdx  = TriVIDs.IndexOf( VertexID );
        return TriEIDs[SubIdx];
    };

    // Handle the easy case of 1 element, 1 contiguous group
    if ( NumTris > 0 && static_cast<int32_t>( ContiguousGroupLengths.size() ) == 1 )
    {
        int32_t const FirstElSeen    = ElementIDFromTriangle( TrianglesOut[0] );
        bool  bSingleElement = true;
        for ( int32_t Idx = 1; Idx < static_cast<int32_t>( TrianglesOut.size() ); ++Idx )
        {
            if ( FirstElSeen != ElementIDFromTriangle( TrianglesOut[Idx] ) )
            {
                bSingleElement = false;
            }
        }
        if ( bSingleElement )
        {
            return false;
        }
    }

    // More complex case: Iterate through each contiguous group looking for a re-used element across groups
    ElementIDSeen.clear();
    // per contiguous group of triangles around vertex in ParentMesh, find contiguous sub-groups in overlay
    for ( int32_t GroupIdx = 0, NumGroups = static_cast<int32_t>( ContiguousGroupLengths.size() ), TriSubStart = 0;
          GroupIdx < NumGroups; GroupIdx++ )
    {
        bool bIsLoop       = GroupIsLoop[GroupIdx];
        int  TriInGroupNum = ContiguousGroupLengths[GroupIdx];
        if ( Common::EnsureOrWarn( TriInGroupNum > 0, "TriInGroupNum > 0" ) == false )
        {
            continue;
        }
        int TriSubEnd = TriSubStart + TriInGroupNum;

        GroupElementIDs.clear();
        for ( int TriSubIdx = TriSubStart; TriSubIdx < TriSubEnd; TriSubIdx++ )
        {
            int TriID = TrianglesOut[TriSubIdx];
            GroupElementIDs.push_back( ElementIDFromTriangle( TriID ) );
        }

        auto IsConnected = [this, &GroupElementIDs, &TrianglesOut, &TriSubStart]( int TriOutIdxA, int TriOutIdxB )
        {
            if ( GroupElementIDs[TriOutIdxA - TriSubStart] != GroupElementIDs[TriOutIdxB - TriSubStart] )
            {
                return false;
            }
            int EdgeID = m_ParentMesh->FindEdgeFromTriPair( TrianglesOut[TriOutIdxA], TrianglesOut[TriOutIdxB] );
            return EdgeID >= 0 && !IsSeamEdge( EdgeID );
        };

        SubGroupID.clear();
        SubGroupID.resize( TriInGroupNum );
        SubGroupElementIDs.clear();
        int MaxSubID  = 0;
        SubGroupID[0] = 0;
        SubGroupElementIDs.push_back( GroupElementIDs[0] );

        // Iterate through tris in current group, except last one
        for ( int TriSubIdx = TriSubStart; TriSubIdx + 1 < TriSubEnd; TriSubIdx++ )
        {
            if ( !IsConnected( TriSubIdx, TriSubIdx + 1 ) )
            {
                SubGroupElementIDs.push_back( GroupElementIDs[TriSubIdx + 1 - TriSubStart] );
                MaxSubID++;
            }
            SubGroupID[TriSubIdx - TriSubStart + 1] = MaxSubID;
        }
        // if group was a loop, need to check if the last sub-group and first sub-group were actually the same
        // group
        if ( bIsLoop && MaxSubID > 0 && IsConnected( TriSubStart, TriSubStart + TriInGroupNum - 1 ) )
        {
            int LastGroupID = SubGroupID.back();
            for ( int32_t Idx = static_cast<int32_t>( SubGroupID.size() ) - 1;
                  Idx >= 0 && SubGroupID[Idx] == LastGroupID; Idx-- )
            {
                SubGroupID[Idx] = 0;
            }
            MaxSubID--;
            SubGroupElementIDs.pop_back();
        }

        for ( int SubID = 0; SubID < static_cast<int32_t>( SubGroupElementIDs.size() ); SubID++ )
        {
            int ElementID = SubGroupElementIDs[SubID];
            if ( ElementID < 0 )
            {
                continue; // skip if this is an invalid ElementID (eg from an invalid triangle)
            }
            // split needed the *second* time we see a sub-group using a given ElementID
            if ( ( std::find( ElementIDSeen.begin(), ElementIDSeen.end(), ElementID ) != ElementIDSeen.end() ) )
            {
                return true;
            }
            else
            {
                ElementIDSeen.push_back( ElementID );
            }
        }

        TriSubStart = TriSubEnd;
    }

    return false;
}

template <typename RealType, int ElementSize>
bool DynamicMeshOverlay<RealType, ElementSize>::AreTrianglesConnected( int TriangleID0, int TriangleID1 ) const
{
    Index3i  NbrTris  = m_ParentMesh->GetTriNeighbourTris( TriangleID0 );
    int      NbrIndex = IndexUtil::FindTriIndex( TriangleID1, NbrTris );
    if ( NbrIndex != IndexConstants::InvalidID )
    {
        Index3i TriEdges = m_ParentMesh->GetTriEdges( TriangleID0 );
        return IsSeamEdge( TriEdges[NbrIndex] ) == false;
    }
    return false;
}

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::GetVertexElements( int vid, std::vector<int>& OutElements ) const
{
    OutElements.clear();
    for ( int tid : m_ParentMesh->VtxTrianglesItr( vid ) )
    {
        if ( !IsSetTriangle( tid ) )
        {
            continue;
        }
        Index3i Triangle = GetTriangle( tid );
        for ( int j = 0; j < 3; ++j )
        {
            if ( m_ParentVertices[Triangle[j]] == vid )
            {
                if ( std::find( OutElements.begin(), OutElements.end(), Triangle[j] ) == OutElements.end() )
                {
                    OutElements.push_back( Triangle[j] );
                }
            }
        }
    }
}

template <typename RealType, int ElementSize>
bool DynamicMeshOverlay<RealType, ElementSize>::FindAnyElementIDAtVertex( int32_t  VertexID,
                                                                          int32_t& OutElementID ) const
{
    OutElementID = IndexConstants::InvalidID;
    for ( int TID : m_ParentMesh->VtxTrianglesItr( VertexID ) )
    {
        if ( !IsSetTriangle( TID ) )
        {
            continue;
        }
        Index3i Triangle = GetTriangle( TID );
        for ( int SubIdx = 0; SubIdx < 3; ++SubIdx )
        {
            if ( m_ParentVertices[Triangle[SubIdx]] == VertexID )
            {
                OutElementID = Triangle[SubIdx];
                return true;
            }
        }
    }
    return false;
}

template <typename RealType, int ElementSize>
int DynamicMeshOverlay<RealType, ElementSize>::CountVertexElements( int vid, bool bBruteForce ) const
{
    DynamicMesh3::LocalIntArray VertexElements;
    Index3i                     Triangle;
    if ( bBruteForce )
    {
        for ( int tid : m_ParentMesh->TriangleIndicesItr() )
        {
            if ( GetTriangleIfValid( tid, Triangle ) )
            {
                for ( int j = 0; j < 3; ++j )
                {
                    if ( m_ParentVertices[Triangle[j]] == vid )
                    {
                        if ( std::find( VertexElements.begin(), VertexElements.end(), Triangle[j] ) ==
                             VertexElements.end() )
                        {
                            VertexElements.push_back( Triangle[j] );
                        }
                    }
                }
            }
        }
    }
    else
    {
        for ( int tid : m_ParentMesh->VtxTrianglesItr( vid ) )
        {
            if ( GetTriangleIfValid( tid, Triangle ) )
            {
                for ( int j = 0; j < 3; ++j )
                {
                    if ( m_ParentVertices[Triangle[j]] == vid )
                    {
                        if ( std::find( VertexElements.begin(), VertexElements.end(), Triangle[j] ) ==
                             VertexElements.end() )
                        {
                            VertexElements.push_back( Triangle[j] );
                        }
                    }
                }
            }
        }
    }

    return static_cast<int32_t>( VertexElements.size() );
}

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::GetElementTriangles( int               ElementID,
                                                                     std::vector<int>& OutTriangles ) const
{
    assert( m_ElementsRefCounts.IsValid( ElementID ) );
    if ( m_ElementsRefCounts.IsValid( ElementID ) )
    {
        int VertexID = m_ParentVertices[ElementID];

        for ( int TriangleID : m_ParentMesh->VtxTrianglesItr( VertexID ) )
        {
            int i = 3 * TriangleID;
            if ( m_ElementTriangles[i] == ElementID || m_ElementTriangles[i + 1] == ElementID ||
                 m_ElementTriangles[i + 2] == ElementID )
            {
                OutTriangles.push_back( TriangleID );
            }
        }
    }
}

template <typename RealType, int ElementSize>
int DynamicMeshOverlay<RealType, ElementSize>::GetElementIDAtVertex( int TriangleID, int VertexID ) const
{
    Index3i Triangle = GetTriangle( TriangleID );
    // Check first element ID to see if triangle is unset (Note: triangles must be fully set or fully unset, so
    // checking first element should be enough)
    if ( Triangle.A == DynamicMesh3::InvalidID )
    {
        return DynamicMesh3::InvalidID;
    }
    for ( int IDX = 0; IDX < 3; ++IDX )
    {
        int ElementID = Triangle[IDX];
        if ( m_ParentVertices[ElementID] == VertexID )
        {
            return ElementID;
        }
    }

    assert( false );
    return DynamicMesh3::InvalidID;
}

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::OnRemoveTriangle( int TriangleID )
{
    Index3i Triangle = GetTriangle( TriangleID );
    if ( Triangle.A < 0 && Triangle.B < 0 && Triangle.C < 0 )
    {
        // if whole triangle has no overlay vertices set, that's OK, just remove nothing
        // (if only *some* of the triangle vertices were < 0, that would be a bug / invalid overlay triangle)
        return;
    }
    InitializeNewTriangle( TriangleID );

    // decrement element refcounts, and free element if it is now unreferenced
    for ( int j = 0; j < 3; ++j )
    {
        int elemid = Triangle[j];
        m_ElementsRefCounts.Decrement( elemid );
        if ( m_ElementsRefCounts.GetRefCount( elemid ) == 1 )
        {
            m_ElementsRefCounts.Decrement( elemid );
            m_ParentVertices[elemid] = DynamicMesh3::InvalidID;
            DESERT_VERIFY_WARN( m_ElementsRefCounts.IsValid( elemid ) == false );
        }
    }
}

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::OnReverseTriOrientation( int TriangleID )
{
    Index3i  Triangle       = GetTriangle( TriangleID );
    int      i              = 3 * TriangleID;
    m_ElementTriangles[i]     = Triangle[1]; // mirrors order in DynamicMesh3::ReverseTriOrientationInternal
    m_ElementTriangles[i + 1] = Triangle[0];
    m_ElementTriangles[i + 2] = Triangle[2];
}

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::OnSplitEdge( const DynamicMesh3::EdgeSplitInfo& splitInfo )
{
    int orig_t0 = splitInfo.OriginalTriangles.A;
    int orig_t1 = splitInfo.OriginalTriangles.B;
    int base_a  = splitInfo.OriginalVertices.A;
    int base_b  = splitInfo.OriginalVertices.B;

    // special handling if either triangle is unset
    bool bT0Set = IsSetTriangle( orig_t0 ), bT1Set = orig_t1 >= 0 && IsSetTriangle( orig_t1 );
    // insert invalid triangle as needed
    if ( !bT0Set )
    {
        InitializeNewTriangle( splitInfo.NewTriangles.A );
    }
    if ( !bT1Set && splitInfo.NewTriangles.B >= 0 )
    {
        // new triangle is invalid
        InitializeNewTriangle( splitInfo.NewTriangles.B );
    }
    // if neither tri was set, nothing else to do
    if ( !bT0Set && !bT1Set )
    {
        return;
    }

    // look up current triangle 0, and infer base triangle 0
    Index3i  Triangle0( -1, -1, -1 );
    int      idx_base_a1 = -1, idx_base_b1 = -1;
    int      NewElemID = -1;
    if ( bT0Set )
    {
        Triangle0 = GetTriangle( orig_t0 );
        Index3i BaseTriangle0( m_ParentVertices[Triangle0.A], m_ParentVertices[Triangle0.B],
                               m_ParentVertices[Triangle0.C] );
        idx_base_a1    = BaseTriangle0.IndexOf( base_a );
        idx_base_b1    = BaseTriangle0.IndexOf( base_b );
        int idx_base_c = IndexUtil::GetOtherTriIndex( idx_base_a1, idx_base_b1 );

        // create new element at lerp position
        NewElemID = AppendElement( (RealType)0 );
        SetElementFromLerp( NewElemID, Triangle0[idx_base_a1], Triangle0[idx_base_b1], (double)splitInfo.SplitT );

        // rewrite triangle 0
        m_ElementTriangles[3 * orig_t0 + idx_base_b1] = NewElemID;

        // create new triangle 2 w/ correct winding order
        Index3i NewTriangle2( NewElemID, Triangle0[idx_base_b1],
                              Triangle0[idx_base_c] ); // mirrors DMesh3::SplitEdge [f,b,c]
        InternalSetTriangle( splitInfo.NewTriangles.A, NewTriangle2, false );

        // update ref counts
        m_ElementsRefCounts.Increment( NewElemID, 2 ); // for the two tris on the T0 side
        m_ElementsRefCounts.Increment( Triangle0[idx_base_c] );
    }

    if ( orig_t1 == DynamicMesh3::InvalidID )
    {
        return; // we are done if this is a boundary triangle
    }

    // look up current triangle1 and infer base triangle 1
    if ( bT1Set )
    {
        Index3i  Triangle1 = GetTriangle( orig_t1 );
        Index3i  BaseTriangle1( m_ParentVertices[Triangle1.A], m_ParentVertices[Triangle1.B],
                                m_ParentVertices[Triangle1.C] );
        int      idx_base_a2 = BaseTriangle1.IndexOf( base_a );
        int      idx_base_b2 = BaseTriangle1.IndexOf( base_b );
        int      idx_base_d  = IndexUtil::GetOtherTriIndex( idx_base_a2, idx_base_b2 );

        int OtherNewElemID = NewElemID;

        // if we don't have a shared edge, we need to create another new UV for the other side
        bool bHasSharedUVEdge =
             bT0Set && IndexUtil::SamePairUnordered( Triangle0[idx_base_a1], Triangle0[idx_base_b1],
                                                     Triangle1[idx_base_a2], Triangle1[idx_base_b2] );
        if ( bHasSharedUVEdge == false )
        {
            // create new element at lerp position
            OtherNewElemID = AppendElement( (RealType)0 );
            SetElementFromLerp( OtherNewElemID, Triangle1[idx_base_a2], Triangle1[idx_base_b2],
                                (double)splitInfo.SplitT );
        }

        // rewrite triangle 1
        m_ElementTriangles[3 * orig_t1 + idx_base_b2] = OtherNewElemID;

        // create new triangle 3 w/ correct winding order
        Index3i NewTriangle3( OtherNewElemID, Triangle1[idx_base_d],
                              Triangle1[idx_base_b2] ); // mirrors DMesh3::SplitEdge [f,d,b]
        InternalSetTriangle( splitInfo.NewTriangles.B, NewTriangle3, false );

        // update ref counts
        m_ElementsRefCounts.Increment( OtherNewElemID, 2 );
        m_ElementsRefCounts.Increment( Triangle1[idx_base_d] );
    }
}

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::OnFlipEdge( const DynamicMesh3::EdgeFlipInfo& FlipInfo )
{
    int   orig_t0 = FlipInfo.Triangles.A;
    int   orig_t1 = FlipInfo.Triangles.B;
    bool  bT0Set = IsSetTriangle( orig_t0 ), bT1Set = IsSetTriangle( orig_t1 );
    int32_t const NumSet = static_cast<int32_t>( bT0Set ) + static_cast<int32_t>( bT1Set );
    if ( NumSet == 0 )
    {
        return; // nothing to do on the overlay if both triangles are unset
    }
    else if ( NumSet == 1 )
    {
        DESERT_VERIFY_WARN( false ); // flipping across a set/unset boundary is not allowed?
        // recover by just unsetting both triangles, since it's too late to prevent the flip
        UnsetTriangle( orig_t0 );
        UnsetTriangle( orig_t1 );
        return;
    }

    int base_a = FlipInfo.OriginalVerts.A;
    int base_b = FlipInfo.OriginalVerts.B;
    int base_c = FlipInfo.OpposingVerts.A;
    int base_d = FlipInfo.OpposingVerts.B;

    // look up triangle 0
    Index3i Triangle0 = GetTriangle( orig_t0 );
    Index3i  BaseTriangle0( m_ParentVertices[Triangle0.A], m_ParentVertices[Triangle0.B],
                            m_ParentVertices[Triangle0.C] );
    int      idx_base_a1 = BaseTriangle0.IndexOf( base_a );
    int      idx_base_b1 = BaseTriangle0.IndexOf( base_b );
    int      idx_base_c  = IndexUtil::GetOtherTriIndex( idx_base_a1, idx_base_b1 );

    // look up triangle 1 (must exist because base mesh would never flip a boundary edge)
    Index3i Triangle1 = GetTriangle( orig_t1 );
    Index3i  BaseTriangle1( m_ParentVertices[Triangle1.A], m_ParentVertices[Triangle1.B],
                            m_ParentVertices[Triangle1.C] );
    int      idx_base_a2 = BaseTriangle1.IndexOf( base_a );
    int      idx_base_b2 = BaseTriangle1.IndexOf( base_b );
    int      idx_base_d  = IndexUtil::GetOtherTriIndex( idx_base_a2, idx_base_b2 );

    // sanity checks
    assert( idx_base_c == BaseTriangle0.IndexOf( base_c ) );
    assert( idx_base_d == BaseTriangle1.IndexOf( base_d ) );

    // we should not have been called on a non-shared edge!!
    bool bHasSharedUVEdge = ( Triangle0[idx_base_a1] == Triangle1[idx_base_a2] ) &&
                            ( Triangle0[idx_base_b1] == Triangle1[idx_base_b2] );
    assert( bHasSharedUVEdge );

    int A = Triangle0[idx_base_a1];
    int B = Triangle0[idx_base_b1];
    int C = Triangle0[idx_base_c];
    int D = Triangle1[idx_base_d];

    // set triangles to same index order as in FDynamicMesh::FlipEdge
    int i0                   = 3 * orig_t0;
    m_ElementTriangles[i0]     = C;
    m_ElementTriangles[i0 + 1] = D;
    m_ElementTriangles[i0 + 2] = B;
    int i1                   = 3 * orig_t1;
    m_ElementTriangles[i1]     = D;
    m_ElementTriangles[i1 + 1] = C;
    m_ElementTriangles[i1 + 2] = A;

    // update reference counts
    m_ElementsRefCounts.Increment( C );
    m_ElementsRefCounts.Increment( D );
    if ( bHasSharedUVEdge )
    {
        m_ElementsRefCounts.Decrement( A );
        m_ElementsRefCounts.Decrement( B );
    }
    else // flipped a seam edge case
    {
        // ideally we wouldn't flip a seam edge, but if we have done so, may need to clean up a now-unreferenced
        // element (note it's too late to prevent the flip here)
        int A2 = Triangle1[idx_base_a2];
        int B2 = Triangle1[idx_base_b2];

        m_ElementsRefCounts.Decrement( A2 );
        // free A2 if it's no longer referenced (can happen in the A != A2 case)
        if ( m_ElementsRefCounts.GetRefCount( A2 ) == 1 )
        {
            m_ElementsRefCounts.Decrement( A2 );
            m_ParentVertices[A2] = DynamicMesh3::InvalidID;
        }
        m_ElementsRefCounts.Decrement( B2 );
        // free B2 if it's no longer referenced (can happen in the B != B2 case)
        if ( m_ElementsRefCounts.GetRefCount( B2 ) == 1 )
        {
            m_ElementsRefCounts.Decrement( B2 );
            m_ParentVertices[B2] = DynamicMesh3::InvalidID;
        }
    }
}

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::OnCollapseEdge(
     const DynamicMesh3::EdgeCollapseInfo& collapseInfo )
{

    int  tid_removed0 = collapseInfo.RemovedTris.A;
    int  tid_removed1 = collapseInfo.RemovedTris.B;
    bool bT0Set = IsSetTriangle( tid_removed0 ), bT1Set = tid_removed1 >= 0 && IsSetTriangle( tid_removed1 );

    int vid_base_kept    = collapseInfo.KeptVertex;
    int vid_base_removed = collapseInfo.RemovedVertex;

    bool bIsSeam    = false;
    bool bIsSeamEnd = false;

    // look up triangle 0
    Index3i  Triangle0( -1, -1, -1 ), BaseTriangle0( -1, -1, -1 );
    int      idx_removed_tri0 = -1, idx_kept_tri0 = -1;
    if ( bT0Set )
    {
        Triangle0 = GetTriangle( tid_removed0 );
        BaseTriangle0    = Index3i( m_ParentVertices[Triangle0.A], m_ParentVertices[Triangle0.B],
                                    m_ParentVertices[Triangle0.C] );
        idx_kept_tri0    = BaseTriangle0.IndexOf( vid_base_kept );
        idx_removed_tri0 = BaseTriangle0.IndexOf( vid_base_removed );
    }

    // look up triangle 1 if this is not a boundary edge
    Index3i  Triangle1( -1, -1, -1 ), BaseTriangle1( -1, -1, -1 );
    int      idx_removed_tri1 = -1, idx_kept_tri1 = -1;
    if ( collapseInfo.bIsBoundary == false && bT1Set )
    {
        Triangle1 = GetTriangle( tid_removed1 );
        BaseTriangle1 = Index3i( m_ParentVertices[Triangle1.A], m_ParentVertices[Triangle1.B],
                                 m_ParentVertices[Triangle1.C] );

        idx_kept_tri1    = BaseTriangle1.IndexOf( vid_base_kept );
        idx_removed_tri1 = BaseTriangle1.IndexOf( vid_base_removed );

        if ( bT0Set )
        {
            int el_kept_tri0    = Triangle0[idx_kept_tri0];
            int el_removed_tri0 = Triangle0[idx_removed_tri0];
            int el_kept_tri1    = Triangle1[idx_kept_tri1];
            int el_removed_tri1 = Triangle1[idx_removed_tri1];

            // is this a seam?
            bIsSeam =
                 !IndexUtil::SamePairUnordered( el_kept_tri0, el_removed_tri0, el_kept_tri1, el_removed_tri1 );

            if ( bIsSeam )
            {
                // is only one of elements split?
                if ( ( el_kept_tri0 == el_kept_tri1 || el_kept_tri0 == el_removed_tri1 ) ||
                     ( el_removed_tri0 == el_kept_tri1 || el_removed_tri0 == el_removed_tri1 ) )
                {
                    bIsSeamEnd = true;
                }
            }
        }
    }

    // this should be protected against by calling code.
    // UE_CHECK_SLOW(!bIsSeamEnd);

    // need to find the elementid for the "kept" and "removed" vertices that are connected by the edges of T0 and
    // T1. If this edge is :
    //       not a seam - just one kept and one removed element.
    //       a seam end - one (two) kept and two (one) removed elements.
    //       a seam     - two kept and two removed elements.
    // The collapse of a seam end must be protected against by the higher-level code.
    //     There is no sensible way to handle the collapse of a seam end.  Retaining two elements would require
    //     some arbitrary split of the removed element and conversely if one element is retained there is no reason
    //     to believe a single element value would be a good approximation to collapsing the edges on both sides of
    //     the seam end.
    int  kept_elemid[2]          = { DynamicMesh3::InvalidID, DynamicMesh3::InvalidID };
    int  removed_elemid[2]       = { DynamicMesh3::InvalidID, DynamicMesh3::InvalidID };
    bool bFoundRemovedElement[2] = { false, false };
    bool bFoundKeptElement[2]    = { false, false };
    if ( bT0Set )
    {
        kept_elemid[0]       = Triangle0[idx_kept_tri0];
        removed_elemid[0]    = Triangle0[idx_removed_tri0];
        bFoundKeptElement[0] = bFoundRemovedElement[0] = true;

        assert( kept_elemid[0] != DynamicMesh3::InvalidID );
        assert( removed_elemid[0] != DynamicMesh3::InvalidID );
    }
    if ( ( bIsSeam || !bT0Set ) && bT1Set )
    {
        kept_elemid[1]       = Triangle1[idx_kept_tri1];
        removed_elemid[1]    = Triangle1[idx_removed_tri1];
        bFoundKeptElement[1] = bFoundRemovedElement[1] = true;

        assert( kept_elemid[1] != DynamicMesh3::InvalidID );
        assert( removed_elemid[1] != DynamicMesh3::InvalidID );
    }

    // update value of kept elements
    for ( int i = 0; i < 2; ++i )
    {
        if ( kept_elemid[i] == DynamicMesh3::InvalidID || removed_elemid[i] == DynamicMesh3::InvalidID )
        {
            continue;
        }

        // Guard against seam-end edge collapse case or otherwise corrupted mesh data
        if ( !Common::EnsureOrWarn( IsElement( kept_elemid[i] ) && IsElement( removed_elemid[i] ),
                                    "IsElement( kept_elemid[i] ) && IsElement( removed_elemid[i] )" ) )
        {
            continue;
        }

        SetElementFromLerp( kept_elemid[i], kept_elemid[i], removed_elemid[i], (double)collapseInfo.CollapseT );
    }

    // Helper for detaching from elements further below. Technically, the freeing gets done for us if the
    // triangle unset call is the last detachment (which it should be as long as only elements on a removed
    // overlay edge are ones that are removed), but it is saner to have it.
    auto DecrementAndFreeIfLast = [this]( int32_t elem_id )
    {
        m_ElementsRefCounts.Decrement( elem_id );
        if ( m_ElementsRefCounts.GetRefCount( elem_id ) == 1 )
        {
            m_ElementsRefCounts.Decrement( elem_id );
            m_ParentVertices[elem_id] = DynamicMesh3::InvalidID;
        }
    };

    // Look for still-existing triangles that have elements linked to the removed vertex and update them.
    // Note that this has to happen even if both triangles were unset, as the removed vertex may have had
    // other elements associated with it, so we need to look at its triangles (which are now attached to
    // vid_base_kept).
    for ( int onering_tid : m_ParentMesh->VtxTrianglesItr( vid_base_kept ) )
    {
        if ( !IsSetTriangle( onering_tid ) )
        {
            continue;
        }
        Index3i elem_tri = GetTriangle( onering_tid );
        for ( int j = 0; j < 3; ++j )
        {
            int elem_id = elem_tri[j];
            if ( m_ParentVertices[elem_id] == vid_base_removed )
            {
                if ( elem_id == removed_elemid[0] )
                {
                    m_ElementTriangles[3 * onering_tid + j] = kept_elemid[0];
                    if ( bFoundKeptElement[0] )
                    {
                        m_ElementsRefCounts.Increment( kept_elemid[0] );
                    }
                    DecrementAndFreeIfLast( elem_id );
                }
                else if ( elem_id == removed_elemid[1] )
                {
                    m_ElementTriangles[3 * onering_tid + j] = kept_elemid[1];
                    if ( bFoundKeptElement[1] )
                    {
                        m_ElementsRefCounts.Increment( kept_elemid[1] );
                    }
                    DecrementAndFreeIfLast( elem_id );
                }
                else
                {
                    // this could happen if a split edge is adjacent to the edge we collapse
                    m_ParentVertices[elem_id] = vid_base_kept;
                }
            }
        }
    }

    // clear the two triangles we removed
    if ( bT0Set )
    {
        UnsetTriangle( tid_removed0, true );
    }
    if ( collapseInfo.bIsBoundary == false && bT1Set )
    {
        UnsetTriangle( tid_removed1, true );
    }

    // if the edge was split, but still shared one element, this should be protected against in the calling code
    if ( removed_elemid[1] == removed_elemid[0] )
    {
        removed_elemid[1] = DynamicMesh3::InvalidID;
    }

    // Note: the elements associated with the removed vertex should have been removed in the iteration or triangle
    // unsetting above
#if UE_BUILD_DEBUG
    for ( int k = 0; k < 2; ++k )
    {
        if ( removed_elemid[k] != DynamicMesh3::InvalidID )
        {
            int rc = ElementsRefCounts.GetRefCount( removed_elemid[k] );
            assert( rc == 0 );
        }
    }
#endif
}

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::OnPokeTriangle( const DynamicMesh3::PokeTriangleInfo& PokeInfo )
{
    if ( !IsSetTriangle( PokeInfo.OriginalTriangle ) )
    {
        InitializeNewTriangle( PokeInfo.NewTriangles.A );
        InitializeNewTriangle( PokeInfo.NewTriangles.B );
        return;
    }

    Index3i Triangle = GetTriangle( PokeInfo.OriginalTriangle );

    // create new element at barycentric position
    int       CenterElemID = AppendElement( (RealType)0 );
    glm::dvec3 BaryCoords( (double)PokeInfo.BaryCoords.x, (double)PokeInfo.BaryCoords.y,
                           (double)PokeInfo.BaryCoords.z );
    SetElementFromBary( CenterElemID, Triangle[0], Triangle[1], Triangle[2], BaryCoords );

    // update orig triangle and two new ones. Winding orders here mirror DynamicMesh3::PokeTriangle
    InternalSetTriangle( PokeInfo.OriginalTriangle, Index3i( Triangle[0], Triangle[1], CenterElemID ), false );
    InternalSetTriangle( PokeInfo.NewTriangles.A, Index3i( Triangle[1], Triangle[2], CenterElemID ), false );
    InternalSetTriangle( PokeInfo.NewTriangles.B, Index3i( Triangle[2], Triangle[0], CenterElemID ), false );

    m_ElementsRefCounts.Increment( Triangle[0] );
    m_ElementsRefCounts.Increment( Triangle[1] );
    m_ElementsRefCounts.Increment( Triangle[2] );
    m_ElementsRefCounts.Increment( CenterElemID, 3 );
}

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::OnMergeEdges( const DynamicMesh3::MergeEdgesInfo& MergeInfo )
{
    // MergeEdges just merges vertices. For now we will not also merge UVs. So all we need to
    // do is rewrite the UV parent vertices

    for ( int i = 0; i < 2; i++ )
    {
        int KeptVID    = MergeInfo.KeptVerts[i];
        int RemovedVID = MergeInfo.RemovedVerts[i];
        if ( RemovedVID == DynamicMesh3::InvalidID )
        {
            continue;
        }
        // this for loop is very similar to GetVertexElements() but accounts for the base mesh already being
        // updated
        for ( int TID : m_ParentMesh->VtxTrianglesItr(
                   KeptVID ) ) // only care about triangles connected to the *new* vertex; these are updated
        {
            if ( !IsSetTriangle( TID ) )
            {
                continue;
            }
            Index3i Triangle = GetTriangle( TID );
            for ( int j = 0; j < 3; ++j )
            {
                // though the ParentMesh vertex is NewVertex in the source mesh, it is still OriginalVertex in the
                // ParentVertices array (since that hasn't been updated yet)
                if ( Triangle[j] != DynamicMesh3::InvalidID && m_ParentVertices[Triangle[j]] == RemovedVID )
                {
                    m_ParentVertices[Triangle[j]] = KeptVID;
                }
            }
        }
    }
}

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::OnMergeVertices( const DynamicMesh3::MergeVerticesInfo& MergeInfo )
{
    // Attributes don't change, but one of the vertices got removed, so any elements on that vertex
    //  should be reparented to the kept vertex. We have to iterate around the kept vertex since
    //  ParentMesh is already updated.
    for ( int Tid : m_ParentMesh->VtxTrianglesItr( MergeInfo.KeptVertex ) )
    {
        if ( !IsSetTriangle( Tid ) )
        {
            continue;
        }
        Index3i Triangle = GetTriangle( Tid );
        for ( int j = 0; j < 3; ++j )
        {
            if ( Triangle[j] != DynamicMesh3::InvalidID &&
                 m_ParentVertices[Triangle[j]] == MergeInfo.RemovedVertex )
            {
                m_ParentVertices[Triangle[j]] = MergeInfo.KeptVertex;
            }
        }
    }
}

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::OnSplitVertex( const DynamicMeshInfo::VertexSplitInfo& SplitInfo,
                                                               const std::span<const int>& TrianglesToUpdate )
{
    std::vector<int> OutElements;

    // this for loop is very similar to GetVertexElements() but accounts for the base mesh already being updated
    for ( int tid :
          m_ParentMesh->VtxTrianglesItr( SplitInfo.NewVertex ) ) // only care about triangles connected to
                                                                 // the *new* vertex; these are updated
    {
        if ( !IsSetTriangle( tid ) )
        {
            continue;
        }
        Index3i Triangle = GetTriangle( tid );
        for ( int j = 0; j < 3; ++j )
        {
            // though the ParentMesh vertex is NewVertex in the source mesh, it is still OriginalVertex in the
            // ParentVertices array (since that hasn't been updated yet)
            if ( Triangle[j] != DynamicMesh3::InvalidID &&
                 m_ParentVertices[Triangle[j]] == SplitInfo.OriginalVertex )
            {
                if ( std::find( OutElements.begin(), OutElements.end(), Triangle[j] ) == OutElements.end() )
                {
                    OutElements.push_back( Triangle[j] );
                }
            }
        }
    }

    for ( int ElementID : OutElements )
    {
        // Note: TrianglesToUpdate will include triangles that don't include the element, but that's ok; it just
        // won't find any elements to update for those
        //			(and this should be cheaper than constructing a new array for every element)
        SplitElementWithNewParent( ElementID, SplitInfo.NewVertex, TrianglesToUpdate );
    }
}

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::SetElementFromLerp( int SetElement, int ElementA, int ElementB,
                                                                    double Alpha )
{
    int    IndexSet = ElementSize * SetElement;
    int    IndexA   = ElementSize * ElementA;
    int    IndexB   = ElementSize * ElementB;
    double Beta     = ( (double)1 - Alpha );
    for ( int i = 0; i < ElementSize; ++i )
    {
        double LerpValue         = Beta * (double)m_Elements[IndexA + i] + Alpha * (double)m_Elements[IndexB + i];
        m_Elements[IndexSet + i] = (RealType)LerpValue;
    }
}

template <typename RealType, int ElementSize>
void DynamicMeshOverlay<RealType, ElementSize>::SetElementFromBary( int SetElement, int ElementA, int ElementB,
                                                                    int ElementC, const glm::dvec3& BaryCoords )
{
    int IndexSet = ElementSize * SetElement;
    int IndexA   = ElementSize * ElementA;
    int IndexB   = ElementSize * ElementB;
    int IndexC   = ElementSize * ElementC;
    for ( int i = 0; i < ElementSize; ++i )
    {
        double BaryValue = BaryCoords.x * (double)m_Elements[IndexA + i] +
                           BaryCoords.y * (double)m_Elements[IndexB + i] +
                           BaryCoords.z * (double)m_Elements[IndexC + i];
        m_Elements[IndexSet + i] = (RealType)BaryValue;
    }
}

template <typename RealType, int ElementSize, typename VectorType>
bool DynamicMeshVectorOverlay<RealType, ElementSize, VectorType>::EnumerateVertexElements(
     int VertexID, std::function<bool( int TriangleID, int ElementID, const VectorType& Value )> ProcessFunc,
     bool bFindUniqueElements ) const
{
    if ( this->m_ParentMesh->IsVertex( VertexID ) == false )
        return false;

    std::vector<int32_t> UniqueElements;

    int32_t Count = 0;
    for ( int tid : this->m_ParentMesh->VtxTrianglesItr( VertexID ) )
    {
        int32_t const BaseElemIdx    = 3 * tid;
        bool          bIsSetTriangle = ( this->m_ElementTriangles[BaseElemIdx] >= 0 );
        if ( bIsSetTriangle )
        {
            Count++;

            for ( int j = 0; j < 3; ++j )
            {
                int32_t const ElementIdx = this->m_ElementTriangles[BaseElemIdx + j];
                if ( this->m_ParentVertices[ElementIdx] == VertexID )
                {
                    bool bIsNew = true;
                    if ( bFindUniqueElements )
                    {
                        bIsNew = std::find( UniqueElements.begin(), UniqueElements.end(), ElementIdx ) ==
                                 UniqueElements.end();
                        if ( bIsNew )
                        {
                            UniqueElements.push_back( ElementIdx );
                        }
                    }
                    if ( bIsNew )
                    {
                        bool bContinue = ProcessFunc( tid, ElementIdx, this->GetElement( ElementIdx ) );
                        if ( !bContinue )
                        {
                            return true;
                        }
                    }
                }
            }
        }
    }
    return ( Count > 0 );
}

template <typename RealType, int ElementSize>
bool DynamicMeshOverlay<RealType, ElementSize>::CheckValidity( bool                  bAllowNonManifoldVertices,
                                                               ValidityCheckFailMode FailMode ) const
{
    bool                    is_ok        = true;
    std::function<void( bool )> CheckOrFailF = [&]( bool b ) { is_ok = is_ok && b; };
    if ( FailMode == ValidityCheckFailMode::Check )
    {
        CheckOrFailF = [&]( bool b )
        {
            assert( ( b ) && "DynamicMeshOverlay::CheckValidity failed!" );
            is_ok = is_ok && b;
        };
    }
    else if ( FailMode == ValidityCheckFailMode::Ensure )
    {
        CheckOrFailF = [&]( bool b )
        {
            DESERT_VERIFY_WARN( b, "DynamicMeshOverlay::CheckValidity failed!" );
            is_ok = is_ok && b;
        };
    }

    // @todo: check that all connected element-pairs are also edges in parentmesh

    // Check that the number of parent vertices is consistent with number of elements.
    CheckOrFailF( m_ParentVertices.Num() * ElementSize == m_Elements.Num() );

    // Check that the per-triangle data matches the number of triangles in the parent mesh.
    CheckOrFailF( !m_ParentMesh || m_ElementTriangles.Num() == m_ParentMesh->MaxTriangleID() * 3 );

    // check that parent vtx of a non-isolated element is actually a vertex
    for ( int elemid : ElementIndicesItr() )
    {
        int  ParentVID          = GetParentVertex( elemid );
        bool bParentIsVertex    = m_ParentMesh->IsVertex( ParentVID );
        bool bElementIsIsolated = ( m_ElementsRefCounts.GetRefCount( elemid ) == 1 );

        // bParentIsVertex XOR bElementIsIsolated
        if ( bParentIsVertex )
        {
            CheckOrFailF( !bElementIsIsolated );
        }
        else
        {
            CheckOrFailF( bElementIsIsolated );
        }
    }

    // check that parent vertices of each element triangle are the same as base triangle
    for ( int tid : m_ParentMesh->TriangleIndicesItr() )
    {
        Index3i ElemTri = GetTriangle( tid );
        Index3i BaseTri = m_ParentMesh->GetTriangle( tid );
        for ( int j = 0; j < 3; ++j )
        {
            if ( ElemTri[j] != DynamicMesh3::InvalidID )
            {
                CheckOrFailF( GetParentVertex( ElemTri[j] ) == BaseTri[j] );
            }
        }
    }

    // count references to each element
    std::vector<int> RealRefCounts;
    RealRefCounts.assign( MaxElementID(), 0 );
    for ( int tid : m_ParentMesh->TriangleIndicesItr() )
    {
        Index3i  Tri        = GetTriangle( tid );
        int      ValidCount = 0;
        for ( int j = 0; j < 3; ++j )
        {
            if ( Tri[j] != DynamicMesh3::InvalidID )
            {
                ++ValidCount;
                RealRefCounts[Tri[j]] += 1;
            }
        }
        CheckOrFailF( ValidCount == 3 || ValidCount == 0 ); // element tri should be fully set or fully unset
    }
    // verify that refcount list counts are same as actual reference counts
    for ( int32_t ElementID = 0; ElementID < static_cast<int32_t>( RealRefCounts.size() ); ++ElementID )
    {
        int32_t const CurRefCount = m_ElementsRefCounts.GetRefCount( ElementID );
        CheckOrFailF( ( RealRefCounts[ElementID] == 0 && CurRefCount == 0 ) ||
                      ( CurRefCount == RealRefCounts[ElementID] + 1 ) );
    }

    return is_ok;
}

template <typename RealType, int ElementSize>
bool DynamicMeshOverlay<RealType, ElementSize>::IsSameAs( const DynamicMeshOverlay<RealType, ElementSize>& Other,
                                                          bool bIgnoreDataLayout ) const
{
    if ( m_ElementsRefCounts.GetCount() != Other.m_ElementsRefCounts.GetCount() )
    {
        return false;
    }

    if ( !bIgnoreDataLayout || ( m_ElementsRefCounts.IsDense() && Other.m_ElementsRefCounts.IsDense() &&
                                 m_ParentMesh->MaxTriangleID() == m_ParentMesh->TriangleCount() &&
                                 Other.m_ParentMesh->MaxTriangleID() == Other.m_ParentMesh->TriangleCount() ) )
    {
        if ( m_ElementsRefCounts.GetMaxIndex() != Other.m_ElementsRefCounts.GetMaxIndex() ||
             m_Elements.Num() != Other.m_Elements.Num() ||
             m_ParentVertices.Num() != Other.m_ParentVertices.Num() ||
             m_ElementTriangles.Num() != Other.m_ElementTriangles.Num() )
        {
            return false;
        }

        if ( m_Elements != Other.m_Elements )
        {
            return false;
        }

        for ( int Idx = 0; Idx < m_ElementsRefCounts.GetMaxIndex(); Idx++ )
        {
            if ( m_ElementsRefCounts.GetRefCount( Idx ) != Other.m_ElementsRefCounts.GetRefCount( Idx ) )
            {
                return false;
            }
        }

        if ( m_ParentVertices != Other.m_ParentVertices )
        {
            return false;
        }

        if ( m_ElementTriangles != Other.m_ElementTriangles )
        {
            return false;
        }
    }
    else
    {
        RefCountVector::IndexIterator       ItEid         = m_ElementsRefCounts.BeginIndices();
        const RefCountVector::IndexIterator ItEidEnd      = m_ElementsRefCounts.EndIndices();
        RefCountVector::IndexIterator       ItEidOther    = Other.m_ElementsRefCounts.BeginIndices();
        const RefCountVector::IndexIterator ItEidEndOther = Other.m_ElementsRefCounts.EndIndices();

        DynamicVector<int> EidMapping;
        EidMapping.Resize( m_ElementsRefCounts.GetMaxIndex(), DynamicMesh3::InvalidID );

        while ( ItEid != ItEidEnd && ItEidOther != ItEidEndOther )
        {
            for ( int32_t i = 0; i < ElementSize; ++i )
            {
                if ( m_Elements[*ItEid * ElementSize + i] != Other.m_Elements[*ItEidOther * ElementSize + i] )
                {
                    // Element values are not the same.
                    return false;
                }
            }

            if ( m_ElementsRefCounts.GetRawRefCount( *ItEid ) !=
                 Other.m_ElementsRefCounts.GetRefCount( *ItEidOther ) )
            {
                // Element ref counts are not the same.
                return false;
            }

            EidMapping[*ItEid] = *ItEidOther;

            ++ItEid;
            ++ItEidOther;
        }

        if ( ItEid != ItEidEnd || ItEidOther != ItEidEndOther )
        {
            // Number of elements is not the same.
            return false;
        }

        if ( m_ParentMesh->TriangleCount() != Other.m_ParentMesh->TriangleCount() )
        {
            return false;
        }

        RefCountVector::IndexIterator       ItTid    = m_ParentMesh->GetTrianglesRefCounts().BeginIndices();
        const RefCountVector::IndexIterator ItTidEnd = m_ParentMesh->GetTrianglesRefCounts().EndIndices();
        RefCountVector::IndexIterator ItTidOther     = Other.m_ParentMesh->GetTrianglesRefCounts().BeginIndices();
        const RefCountVector::IndexIterator ItTidEndOther =
             Other.m_ParentMesh->GetTrianglesRefCounts().EndIndices();

        while ( ItTid != ItTidEnd && ItTidOther != ItTidEndOther )
        {
            const int ElementTrianglesIdx      = *ItTid * 3;
            const int ElementTrianglesIdxOther = *ItTidOther * 3;

            for ( int i = 0; i < 3; ++i )
            {
                const int Eid      = m_ElementTriangles[ElementTrianglesIdx + i];
                const int EidOther = Other.m_ElementTriangles[ElementTrianglesIdxOther + i];
                if ( EidOther != ( Eid != DynamicMesh3::InvalidID ? EidMapping[Eid] : DynamicMesh3::InvalidID ) )
                {
                    // Triangle elements do not index the same element value.
                    return false;
                }
            }

            ++ItTid;
            ++ItTidOther;
        }

        if ( ItTid != ItTidEnd || ItTidOther != ItTidEndOther )
        {
            // Number of element triangles is not the same.
            return false;
        }
    }

    return true;
}

namespace Desert::Geometry
{

    // These are explicit instantiations of the templates that are exported from the shared lib.
    // Only these instantiations of the template can be used.
    // This is necessary because we have placed most of the templated functions in this .cpp file, instead of the
    // header.
    template class DynamicMeshOverlay<float, 1>;
    template class DynamicMeshOverlay<double, 1>;
    template class DynamicMeshOverlay<int, 1>;
    template class DynamicMeshOverlay<float, 2>;
    template class DynamicMeshOverlay<double, 2>;
    template class DynamicMeshOverlay<int, 2>;
    template class DynamicMeshOverlay<float, 3>;
    template class DynamicMeshOverlay<double, 3>;
    template class DynamicMeshOverlay<int, 3>;
    template class DynamicMeshOverlay<float, 4>;
    template class DynamicMeshOverlay<double, 4>;

    template class DynamicMeshVectorOverlay<float, 2, glm::vec2>;
    template class DynamicMeshVectorOverlay<double, 2, glm::dvec2>;
    template class DynamicMeshVectorOverlay<float, 3, glm::vec3>;
    template class DynamicMeshVectorOverlay<double, 3, glm::dvec3>;
    template class DynamicMeshVectorOverlay<float, 4, glm::vec4>;

} // namespace Desert::Geometry