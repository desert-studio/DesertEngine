// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/DynamicMesh/DynamicMeshOverlay.h:1-883, adapted: UE
// Core via UECore.hpp, namespace Desert::Geometry, TUniquePtr is std::unique_ptr; FArchive serialization not
// ported.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/GeometryTypes.hpp"
#include "Engine/Geometry/UECore/IndexTypes.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/InfoTypes.hpp"

#include "Engine/Geometry/UECore/CompactMaps.hpp"
#include "Engine/Geometry/UECore/DynamicVector.hpp"
#include "Engine/Geometry/UECore/RefCountVector.hpp"
#include "Engine/Geometry/UECore/SmallListSet.hpp"
#include "Engine/Geometry/UECore/VectorTypes.hpp"
#include <Common/Core/Core.hpp>
#include <cstddef>

namespace Desert::Geometry
{

    class DynamicMesh3;

    /**
     * DynamicMeshOverlay is an add-on to a DynamicMesh3 that allows for per-triangle storage
     * of an "element" (eg like a per-triangle UV or normal). However the elements can be shared
     * between triangles at shared vertices because the elements are stored in a separate indexable
     * list.
     *
     * Each element has one vertex in the parent mesh as its parent, whereas each vertex may be the
     * parent of multiple elements in cases where neighboring triangles are not sharing a single
     * element for that vertex. This means that there may be "seam" boundary edges in the
     * overlay topology that are not mesh boundary edges in the associated/parent mesh, but
     * the overlay topology will not connect triangles that were not connected in the parent mesh
     * or create any topologically degenerate triangles, since the parent vids of the elements
     * of a triangle will have to match up to the vids of the triangle.
     *
     * A "seam" edge is one where at least one of the elements of the triangles on either
     * side of the edge is not shared between the two triangles.
     *
     * The DynamicMesh3 mesh topology operations (eg split/flip/collapse edge, poke face, etc)
     * can be mirrored to the overlay via OnSplitEdge(), etc.
     *
     * Note that although this is a template, many of the functions are defined in the .cpp file.
     * As a result you need to explicitly instantiate and export the instance of the template that
     * you wish to use in the block at the top of DynamicMeshOverlay.cpp
     */
    template <typename RealType, int ElementSize>
    class DynamicMeshOverlay
    {

    protected:
        /** The parent mesh this overlay belongs to */
        DynamicMesh3* m_ParentMesh = nullptr;

        /** Reference counts of element indices. Iterate over this to find out which elements are valid. */
        RefCountVector m_ElementsRefCounts;
        /** List of element values */
        DynamicVector<RealType> m_Elements;
        /** List of parent vertex indices, one per element */
        DynamicVector<int> m_ParentVertices;

        /** List of triangle element-index triplets [Elem0 Elem1 Elem2]*/
        DynamicVector<int> m_ElementTriangles;

        friend class DynamicMesh3;
        friend class DynamicMeshAttributeSet;

    public:
        /** Create an empty overlay */
        DynamicMeshOverlay() = default;

        /** Create an overlay for the given parent mesh */
        DynamicMeshOverlay( DynamicMesh3* ParentMeshIn ) : m_ParentMesh( ParentMeshIn )
        {
        }

    private:
        /** @set the parent mesh for this overlay.  Only safe for use during FDynamicMesh move */
        void Reparent( DynamicMesh3* ParentMeshIn )
        {
            m_ParentMesh = ParentMeshIn;
        }

    public:
        /** @return the parent mesh for this overlay */
        [[nodiscard]] const DynamicMesh3* GetParentMesh() const
        {
            return m_ParentMesh;
        }
        /** @return the parent mesh for this overlay */
        DynamicMesh3* GetParentMesh()
        {
            return m_ParentMesh;
        }

        /** Set this overlay to contain the same arrays as the copy overlay */
        void Copy( const DynamicMeshOverlay<RealType, ElementSize>& Copy )
        {
            m_ElementsRefCounts = RefCountVector( Copy.m_ElementsRefCounts );
            m_Elements          = Copy.m_Elements;
            m_ParentVertices    = Copy.m_ParentVertices;
            m_ElementTriangles  = Copy.m_ElementTriangles;
        }

        /** Copy the Copy overlay to a compact rep, also updating parent references based on the CompactMaps */
        void CompactCopy( const DynamicMeshCompactMaps&                    CompactMaps,
                          const DynamicMeshOverlay<RealType, ElementSize>& Copy )
        {
            ClearElements();

            // map of element IDs
            std::vector<int> MapE;
            MapE.resize( Copy.MaxElementID() );

            // copy elements across
            RealType Data[ElementSize];
            for ( int EID = 0; EID < Copy.MaxElementID(); EID++ )
            {
                if ( Copy.IsElement( EID ) )
                {
                    Copy.GetElement( EID, Data );
                    MapE[EID] = AppendElement( Data );
                }
                else
                {
                    MapE[EID] = -1;
                }
            }

            // copy triangles across
            assert( CompactMaps.NumTriangleMappings() ==
                    Copy.GetParentMesh()->MaxTriangleID() ); // must have valid triangle map
            for ( const int FromTID : Copy.GetParentMesh()->TriangleIndicesItr() )
            {
                if ( !Copy.IsSetTriangle( FromTID ) )
                {
                    continue;
                }
                const int ToTID           = CompactMaps.GetTriangleMapping( FromTID );
                const Index3i FromTriElements = Copy.GetTriangle( FromTID );
                SetTriangle(
                     ToTID, Index3i( MapE[FromTriElements.A], MapE[FromTriElements.B], MapE[FromTriElements.C] ) );
            }
        }

        /** Compact overlay and update links to parent based on CompactMaps */
        void CompactInPlace( const DynamicMeshCompactMaps& CompactMaps )
        {
            int iLastE = MaxElementID() - 1;
            int iCurE  = 0;
            while ( iLastE >= 0 && !m_ElementsRefCounts.IsValidUnsafe( iLastE ) )
            {
                iLastE--;
            }
            while ( iCurE < iLastE && m_ElementsRefCounts.IsValidUnsafe( iCurE ) )
            {
                iCurE++;
            }

            // make a map to track element index changes, to use to update element triangles later
            // Possible speed-up (as in UE): it may be faster to not construct this and to do the remapping per
            // element as we go (by iterating the one ring of each parent vertex for each element)
            std::vector<int> MapE;
            MapE.resize( MaxElementID() );
            for ( int ID = 0; ID < static_cast<int32_t>( MapE.size() ); ID++ )
            {
                // mapping is 1:1 by default; sparsely re-mapped below
                MapE[ID] = ID;
                // remap all parents
                if ( m_ParentVertices[ID] >= 0 )
                {
                    m_ParentVertices[ID] = CompactMaps.GetVertexMapping( m_ParentVertices[ID] );
                }
            }

            DynamicVector<unsigned short>&  ERef = m_ElementsRefCounts.GetRawRefCountsUnsafe();
            RealType                        Data[ElementSize];
            while ( iCurE < iLastE )
            {
                // remap the element data
                GetElement( iLastE, Data );
                SetElement( iCurE, Data );
                m_ParentVertices[iCurE] = m_ParentVertices[iLastE];
                ERef[iCurE]           = ERef[iLastE];
                ERef[iLastE]            = RefCountVector::INVALID_REF_COUNT;
                MapE[iLastE]          = iCurE;

                // move cur forward one, last back one, and  then search for next valid
                iLastE--;
                iCurE++;
                while ( iLastE >= 0 && !m_ElementsRefCounts.IsValidUnsafe( iLastE ) )
                {
                    iLastE--;
                }
                while ( iCurE < iLastE && m_ElementsRefCounts.IsValidUnsafe( iCurE ) )
                {
                    iCurE++;
                }
            }
            m_ElementsRefCounts.Trim( ElementCount() );
            m_Elements.Resize( ElementCount() * ElementSize );
            m_ParentVertices.Resize( ElementCount() );

            // Remap and compact triangle element indices.
            int32_t MaxNewTID = -1;
            for ( int TID = 0, OldMaxTID = m_ElementTriangles.Num() / 3; TID < OldMaxTID; TID++ )
            {
                const int32_t OldStart = TID * 3;
                const int32_t NewTID   = CompactMaps.GetTriangleMapping( TID );
                if ( NewTID == IndexConstants::InvalidID )
                {
                    // skip if there's no mapping
                    continue;
                }

                MaxNewTID = std::max( NewTID, MaxNewTID );

                const int32_t NewStart = NewTID * 3;
                if ( m_ElementTriangles[OldStart] == IndexConstants::InvalidID )
                {
                    // triangle was not set; copy back InvalidID
                    for ( int SubIdx = 0; SubIdx < 3; SubIdx++ )
                    {
                        m_ElementTriangles[NewStart + SubIdx] = IndexConstants::InvalidID;
                    }
                }
                else
                {
                    for ( int SubIdx = 0; SubIdx < 3; SubIdx++ )
                    {
                        m_ElementTriangles[NewStart + SubIdx] = MapE[m_ElementTriangles[OldStart + SubIdx]];
                    }
                }
            }
            // ElementTriangles should never grow during a compaction, so just resizing is ok
            // (i.e., we shouldn't need to set InvalidID on any added triangles)
            assert( m_ElementTriangles.Num() >= static_cast<size_t>( MaxNewTID + 1 ) * 3 );
            m_ElementTriangles.Resize( static_cast<size_t>( MaxNewTID + 1 ) * 3 );

            assert( IsCompact() );
        }

        void Append( const DynamicMeshOverlay& ToAppend, const DynamicMesh3::AppendInfo& AppendInfo )
        {
            // We expect to be appending s.t. the intial ElementTriangles map to the pre-append triangles
            if ( !Common::EnsureOrWarn( m_ElementTriangles.Num() ==
                                             static_cast<size_t>( AppendInfo.TriangleOffset ) * 3,
                                        "ElementTriangles.Num() == AppendInfo.TriangleOffset * 3" ) )
            {
                m_ElementTriangles.SetNum( AppendInfo.TriangleOffset * 3 );
            }
            assert( ToAppend.m_ElementTriangles.Num() == static_cast<size_t>( AppendInfo.NumTriangle ) * 3 );

            const int32_t ElementIDOffset        = m_ElementsRefCounts.GetMaxIndex();
            const int32_t ElementTrianglesOffset = m_ElementTriangles.Num();
            m_ElementTriangles.Add( ToAppend.m_ElementTriangles );
            for ( int32_t Idx = ElementTrianglesOffset, N = m_ElementTriangles.Num(); Idx < N; ++Idx )
            {
                int32_t& ElID = m_ElementTriangles[Idx];
                if ( ElID != IndexConstants::InvalidID )
                {
                    ElID += ElementIDOffset;
                }
            }

            assert( ElementIDOffset == m_ParentVertices.Num() ); // ParentVertices must be 1:1 with Element IDs
            m_ParentVertices.Add( ToAppend.m_ParentVertices );
            for ( int32_t Idx = 0; Idx < ToAppend.m_ParentVertices.Num(); ++Idx )
            {
                int32_t& Parent = m_ParentVertices[Idx + ElementIDOffset];
                if ( Parent != IndexConstants::InvalidID )
                {
                    Parent += AppendInfo.VertexOffset;
                }
            }
            m_ElementsRefCounts.Append( ToAppend.m_ElementsRefCounts );
            m_Elements.Add( ToAppend.m_Elements );
        }
        void AppendDefaulted( const DynamicMesh3::AppendInfo& AppendInfo )
        {
            // Note: ElementRefCounts, Elements and ParentVertices remain unchanged, since the new triangles are
            // unset
            assert( m_ElementTriangles.Num() == static_cast<size_t>( AppendInfo.TriangleOffset ) * 3 );
            m_ElementTriangles.Resize(
                 3 * static_cast<size_t>( AppendInfo.TriangleOffset + AppendInfo.NumTriangle ),
                 IndexConstants::InvalidID );
        }

        /** Discard all elements. */
        void ClearElements();

        /** Discard elements for given triangles. */
        template <typename EnumerableIntType>
        void ClearElements( const EnumerableIntType& Triangles )
        {
            for ( const int32_t TriID : Triangles )
            {
                UnsetTriangle( TriID );
            }
        }

        /** @return the number of in-use Elements in the overlay */
        [[nodiscard]] int ElementCount() const
        {
            return (int)m_ElementsRefCounts.GetCount();
        }
        /** @return the maximum element index in the overlay. This may be larger than the count if Elements have
         * been deleted. */
        [[nodiscard]] int MaxElementID() const
        {
            return (int)m_ElementsRefCounts.GetMaxIndex();
        }
        /** @return true if this element index is in use */
        [[nodiscard]] bool IsElement( int vID ) const
        {
            return m_ElementsRefCounts.IsValid( vID );
        }

        /** @return true if the elements are compact */
        [[nodiscard]] bool IsCompact() const
        {
            return m_ElementsRefCounts.IsDense();
        }

        using element_iterator = typename RefCountVector::IndexEnumerable;

        /** @return enumerator for valid element indices suitable for use with range-based for */
        [[nodiscard]] element_iterator ElementIndicesItr() const
        {
            return m_ElementsRefCounts.Indices();
        }

        /** Allocate a new element with the given constant value */
        int AppendElement( RealType ConstantValue );
        /** Allocate a new element with the given value */
        int AppendElement( const RealType* Value );

        void SetParentVertex( int ElementIndex, int ParentVertexIndex )
        {
            m_ParentVertices[ElementIndex] = ParentVertexIndex;
        }

        /** Initialize the triangle list to the given size, and set all triangles to InvalidID */
        void InitializeTriangles( int MaxTriangleID );

        /**
         * Set the triangle to the given Element index tuple, and increment element reference counts
         *
         * @param bAllowElementFreeing If true, then any elements that were only referenced by this triangle
         *  become immediately unallocated if the triangle no longer references them. This can be set to false
         *  when remeshing across existing elements to avoid them being freed while temporarily unreferenced,
         *  but then it should eventually be followed by a call to FreeUnusedElements().
         */
        MeshResult SetTriangle( int tid, const Index3i& tv, bool bAllowElementFreeing = true );

        /**
         * Goes through elements and frees any whose reference counts indicate that they are not being used. This
         * is usually not necessary since most operations that remove references will go ahead and do this, but
         * it may be used, for instance, after SetTriangle is called with bAllowElementFreeing set to false.
         *
         * @param ElementsToCheck If provided, only these element ID's will be checked.
         */
        void FreeUnusedElements( const std::unordered_set<int>* ElementsToCheck = nullptr );

        /**
         * Set the triangle to have InvalidID element IDs, decrementing element reference counts if needed.
         *
         * @param bAllowElementFreeing If true, then any elements that were only referenced by this triangle
         *  become immediately unallocated. This can be set to false as part of a remeshing, but then it
         *  should eventually be followed by a call to FreeUnusedElements().
         */
        void UnsetTriangle( int TriangleID, bool bAllowElementFreeing = true );

        /** @return true if this triangle was set */
        [[nodiscard]] bool IsSetTriangle( int TID ) const
        {
            const bool bIsSet = m_ElementTriangles[3 * TID] >= 0;
            // we require that triangle elements either be all set or all unset
            assert( ( m_ElementTriangles[3 * TID + 1] >= 0 ) == bIsSet );
            assert( ( m_ElementTriangles[3 * TID + 2] >= 0 ) == bIsSet );
            return bIsSet;
        }

        /**
         * @return true if this overlay's per-triangle storage is large enough to cover every triangle
         *  in the parent mesh's ID space. When false, the overlay was not grown alongside the parent
         *  (e.g., triangles were added to the parent through a path that did not fire OnNewTriangle,
         *  or the on-disk data was deserialized with mismatched element-triangle counts) and any
         *  IsSetTriangle / GetTri* call for a triangle ID near MaxTriangleID() will read past the end
         *  of ElementTriangles.
         */
        [[nodiscard]] bool IsTriangleStorageValid() const
        {
            return m_ParentMesh != nullptr &&
                   static_cast<int32_t>( m_ElementTriangles.Num() ) >= 3 * m_ParentMesh->MaxTriangleID();
        }

        /**
         * Build overlay topology from a predicate function, e.g. to build topology for sharp normals
         *
         * @param TrisCanShareVertexPredicate Indicator function returns true if the given vertex can be shared for
         *the given pair of triangles Note if a vertex can be shared between tris A and B, and B and C, it will be
         *shared between all three
         * @param InitElementValue Initial element value, copied into all created elements
         */
        void CreateFromPredicate(
             const std::function<bool( int ParentVertexIdx, int TriIDA, int TriIDB )>& TrisCanShareVertexPredicate,
             RealType                                                                  InitElementValue );

        /**
         * Build overlay topology with one element per vertex.
         * Note: Faster-but-equivalent-to calling CreateFromPredicate() with a CanShareVertex predicate that always
         * returns true
         * @param bExactMapIDs If true, ElementIDs will be constructed to exactly match the corresponding vertex
         * IDs, even if the mesh is non-compact.
         */
        void CreatePerVertex( RealType InitElementValue, bool bExactMapIDs = false );

        /**
         * Refine an existing overlay topology.  For any element on a given triangle, if the predicate returns
         * true, it gets topologically split out so it isn't shared by any other triangle. Used for creating sharp
         * vertices in the normals overlay.
         *
         * @param ShouldSplitOutVertex predicate returns true of the element should be split out and not shared w/
         * any other triangle
         * @param GetNewElementValue function to assign a new value to any element that is split out
         */
        void SplitVerticesWithPredicate(
             const std::function<bool( int ElementIdx, int TriID )>&              ShouldSplitOutVertex,
             std::function<void( int ElementIdx, int TriID, RealType* FillVect )> GetNewElementValue );

        /**
         * Collapse SourceElementID into TargetElementID, resulting in connecting any containing triangles and
         * reducing the total elements in the overlay.
         *
         * @param SourceElementID the element to merge away
         * @param TargetElementID the element to merge into
         * @return If the operation completed successfully, returns true
         */
        bool MergeElement( int SourceElementID, int TargetElementID );

        /**
         * Create a new copy of ElementID, and update connected triangles in the TrianglesToUpdate array to
         * reference the copy of ElementID where they used to reference ElementID (Note: This just calls
         * "SplitElementWithNewParent" with the existing element's parent id.)
         *
         * @param ElementID the element to copy
         * @param TrianglesToUpdate the triangles that should now reference the new element
         * @return the ID of the newly created element
         */
        int SplitElement( int ElementID, const std::span<const int>& TrianglesToUpdate );

        /**
         * Create a new copy of ElementID, and update connected triangles in the TrianglesToUpdate array to
         * reference the copy of ElementID where they used to reference ElementID.  The new element will have the
         * given parent vertex ID. Deletes any elements that are no longer used after the triangles are changed.
         *
         * @param ElementID the element to copy
         * @param SplitParentVertexID the new parent vertex for copied elements
         * @param TrianglesToUpdate the triangles that should now reference the new element.  Note: this is allowed
         * to include triangles that do not have the element at all; sometimes you may want to do so to avoid
         * creating a new array for each call.
         * @return the ID of the newly created element
         */
        int SplitElementWithNewParent( int ElementID, int NewParentID,
                                       const std::span<const int>& TrianglesToUpdate );

        /**
         * Split any bowties at given vertex.
         *
         * @param NewElementIDs If not null, newly created element IDs are placed here. Note that this array is
         *   intentionally not cleared before appending to it.
         */
        void SplitBowtiesAtVertex( int32_t VertexID, std::vector<int32_t>* NewElementIDs = nullptr );

        /**
         * Refine an existing overlay topology by splitting any bowties
         * @param bParallel Whether to run bowtie detection in parallel
         */
        void SplitBowties( bool bParallel = true );

        //
        // Support for inserting element at specific ID. This is a bit tricky
        // because we likely will need to update the free list in the RefCountVector, which
        // can be expensive. If you are going to do many inserts (eg inside a loop), wrap in
        // BeginUnsafe / EndUnsafe calls, and pass bUnsafe = true to the InsertElement() calls,
        // to the defer free list rebuild until you are done.
        //

        /** Call this before a set of unsafe InsertVertex() calls */
        void BeginUnsafeElementsInsert()
        {
            // do nothing...
        }

        /** Call after a set of unsafe InsertVertex() calls to rebuild free list */
        void EndUnsafeElementsInsert()
        {
            m_ElementsRefCounts.RebuildFreeList();
        }

        /**
         * Insert element at given index, assuming it is unused.
         * If bUnsafe, we use fast id allocation that does not update free list.
         * You should only be using this between BeginUnsafeElementsInsert() / EndUnsafeElementsInsert() calls
         */
        MeshResult InsertElement( int ElementID, const RealType* Value, bool bUnsafe = false );

        //
        // Accessors/Queries
        //

        /** Get the element at a given index */
        void GetElement( int ElementID, RealType* Data ) const
        {
            const int k = ElementID * ElementSize;
            for ( int i = 0; i < ElementSize; ++i )
            {
                Data[i] = m_Elements[k + i];
            }
        }

        /** Get the element at a given index */
        template <typename AsType>
        void GetElement( int ElementID, AsType& Data ) const
        {
            const int k = ElementID * ElementSize;
            for ( int i = 0; i < ElementSize; ++i )
            {
                Data[i] = m_Elements[k + i];
            }
        }

        /**
         * Get the Element value associated with a vertex of a triangle.
         *
         * @param TriangleID ID of a triangle containing the Element
         * @param VertexID ID of the Element's parent vertex
         * @param Data Value contained at the Element
         */
        template <typename AsType>
        void GetElementAtVertex( int TriangleID, int VertexID, AsType& Data ) const
        {
            const int ElementID = GetElementIDAtVertex( TriangleID, VertexID );

            assert( ElementID != IndexConstants::InvalidID );
            if ( ElementID != IndexConstants::InvalidID )
            {
                GetElement( ElementID, Data );
            }
        }

        /** Get the parent vertex id for the element at a given index */
        [[nodiscard]] int GetParentVertex( int ElementID ) const
        {
            return m_ParentVertices[ElementID];
        }

        /** Get the element index tuple for a triangle */
        [[nodiscard]] Index3i GetTriangle( int TriangleID ) const
        {
            const int i = 3 * TriangleID;
            return { m_ElementTriangles[i], m_ElementTriangles[i + 1], m_ElementTriangles[i + 2] };
        }

        /** If the triangle is set to valid element indices, return the indices in TriangleOut and return true,
         * otherwise return false */
        bool GetTriangleIfValid( int TriangleID, Index3i& TriangleOut ) const
        {
            const int i = 3 * TriangleID;
            const int a = m_ElementTriangles[i];
            if ( a >= 0 )
            {
                TriangleOut = Index3i( a, m_ElementTriangles[i + 1], m_ElementTriangles[i + 2] );
                assert( TriangleOut.B >= 0 && TriangleOut.C >= 0 );
                return true;
            }
            return false;
        }

        /** Set the element at a given index */
        void SetElement( int ElementID, const RealType* Data )
        {
            const int k = ElementID * ElementSize;
            for ( int i = 0; i < ElementSize; ++i )
            {
                m_Elements[k + i] = Data[i];
            }
        }

        /** Set the element at a given index */
        template <typename AsType>
        void SetElement( int ElementID, const AsType& Data )
        {
            const int k = ElementID * ElementSize;
            for ( int i = 0; i < ElementSize; ++i )
            {
                m_Elements[k + i] = Data[i];
            }
        }

        /** @return true if triangle contains element */
        [[nodiscard]] bool TriangleHasElement( int TriangleID, int ElementID ) const
        {
            const int i = 3 * TriangleID;
            return ( m_ElementTriangles[i] == ElementID || m_ElementTriangles[i + 1] == ElementID ||
                     m_ElementTriangles[i + 2] == ElementID );
        }

        /** Returns true if the parent-mesh edge is a "Seam" in this overlay.
         *   If present, bIsNonIntersectingOut will be true only if this is a seam edge
         *   that does not intersect with another seam or the end of the seam.
         */
        bool IsSeamEdge( int EdgeID, bool* bIsNonIntersectingOut = nullptr ) const;
        /** Returns true if the parent-mesh edge is a "Seam End" in this overlay, meaning the adjacent element
         * triangles share one element, not two */
        [[nodiscard]] bool IsSeamEndEdge( int EdgeID ) const;
        /** Returns true if the parent-mesh vertex is connected to any seam edges */
        [[nodiscard]] bool IsSeamVertex( int vid, bool bBoundaryIsSeam = true ) const;
        /** Returns true if the parent-mesh vertex is at a seam 'intersection' -- i.e., the end of a seam, or the
         * intersection w/ another seam. */
        [[nodiscard]] bool IsSeamIntersectionVertex( int32_t VertexID ) const;

        /**
         * Determines whether the base-mesh vertex has "bowtie" topology in the Overlay.
         * Bowtie topology means that one or more elements at the vertex are shared across disconnected
         * UV-components.
         * @return true if the base-mesh vertex has "bowtie" topology in the overlay
         */
        [[nodiscard]] bool IsBowtieInOverlay( int32_t VertexID ) const;

        /** @return true if the two triangles are connected, ie shared edge exists and is not a seam edge */
        [[nodiscard]] bool AreTrianglesConnected( int TriangleID0, int TriangleID1 ) const;

        /** find the elements associated with a given parent-mesh vertex */
        void GetVertexElements( int vid, std::vector<int>& OutElements ) const;
        /** Count the number of unique elements for a given parent-mesh vertex */
        [[nodiscard]] int CountVertexElements( int vid, bool bBruteForce = false ) const;

        /** find the triangles connected to an element */
        void GetElementTriangles( int ElementID, std::vector<int>& OutTriangles ) const;

        /**
         * Find the element ID at a vertex of a triangle.
         *
         * @return Returns the element ID or DynamicMesh3::InvalidID if the vertex is not a parent of any element
         * contained in the triangle.
         */
        [[nodiscard]] int GetElementIDAtVertex( int TriangleID, int VertexID ) const;

        /**
         * Find an element ID associated with the given VertexID
         *
         * @param VertexID The vertex ID to search
         * @param OutElementID Will be set to the found element ID, or DynamicMesh3::InvalidID if none found
         * @return true if an element ID was found, false otherwise
         */
        bool FindAnyElementIDAtVertex( int32_t VertexID, int32_t& OutElementID ) const;

        /** @return true if overlay has any interior seam edges. This requires an O(N) search unless it early-outs.
         */
        [[nodiscard]] bool HasInteriorSeamEdges() const;

        /**
         * Compute interpolated parameter value inside triangle using barycentric coordinates
         * @param TriangleID index of triangle
         * @param BaryCoords 3 barycentric coordinates inside triangle
         * @param DataOut resulting interpolated overlay parameter value (of size ElementSize)
         */
        template <typename AsType>
        void GetTriBaryInterpolate( int32_t TriangleID, const AsType* BaryCoords, AsType* DataOut ) const
        {
            int32_t const TriIndex   = 3 * TriangleID;
            int32_t       ElemIndex0 = m_ElementTriangles[TriIndex] * ElementSize;
            int32_t       ElemIndex1 = m_ElementTriangles[TriIndex + 1] * ElementSize;
            int32_t       ElemIndex2 = m_ElementTriangles[TriIndex + 2] * ElementSize;
            const auto    Bary0      = (AsType)BaryCoords[0];
            const auto    Bary1      = (AsType)BaryCoords[1];
            const auto    Bary2      = (AsType)BaryCoords[2];
            for ( int32_t i = 0; i < ElementSize; ++i )
            {
                DataOut[i] = Bary0 * (AsType)m_Elements[ElemIndex0 + i] +
                             Bary1 * (AsType)m_Elements[ElemIndex1 + i] +
                             Bary2 * (AsType)m_Elements[ElemIndex2 + i];
            }
        }

        /**
         * Checks that the overlay mesh is well-formed, ie all internal data structures are consistent
         */
        [[nodiscard]] bool CheckValidity( bool                  bAllowNonManifoldVertices = true,
                                          ValidityCheckFailMode FailMode = ValidityCheckFailMode::Check ) const;

        /**
         * Returns true if this overlay is the same as Other.
         */
        [[nodiscard]] bool IsSameAs( const DynamicMeshOverlay<RealType, ElementSize>& Other,
                                     bool                                             bIgnoreDataLayout ) const;

        [[nodiscard]] size_t GetByteCount() const
        {
            return m_ElementsRefCounts.GetByteCount() + m_Elements.GetByteCount() +
                   m_ParentVertices.GetByteCount() + m_ElementTriangles.GetByteCount();
        }

        /** Set a triangle's element indices to InvalidID */
        void InitializeNewTriangle( int tid );
        /** Remove a triangle from the overlay */
        void OnRemoveTriangle( int TriangleID );
        /** Reverse the orientation of a triangle's elements */
        void OnReverseTriOrientation( int TriangleID );
        /** Update the overlay to reflect an edge split in the parent mesh */
        void OnSplitEdge( const DynamicMeshInfo::EdgeSplitInfo& SplitInfo );
        /** Update the overlay to reflect an edge flip in the parent mesh */
        void OnFlipEdge( const DynamicMeshInfo::EdgeFlipInfo& FlipInfo );
        /** Update the overlay to reflect an edge collapse in the parent mesh */
        void OnCollapseEdge( const DynamicMeshInfo::EdgeCollapseInfo& CollapseInfo );
        /** Update the overlay to reflect a face poke in the parent mesh */
        void OnPokeTriangle( const DynamicMeshInfo::PokeTriangleInfo& PokeInfo );
        /** Update the overlay to reflect an edge merge in the parent mesh */
        void OnMergeEdges( const DynamicMeshInfo::MergeEdgesInfo& MergeInfo );
        /** Update the overlay to reflect a vertex merge in the parent mesh */
        void OnMergeVertices( const DynamicMeshInfo::MergeVerticesInfo& MergeInfo );
        /** Update the overlay to reflect a vertex split in the parent mesh */
        void OnSplitVertex( const DynamicMeshInfo::VertexSplitInfo& SplitInfo,
                            const std::span<const int>&             TrianglesToUpdate );

    protected:
        /** Set the value at an Element to be a linear interpolation of two other Elements */
        void SetElementFromLerp( int SetElement, int ElementA, int ElementB, double Alpha );
        /** Set the value at an Element to be a barycentric interpolation of three other Elements */
        void SetElementFromBary( int SetElement, int ElementA, int ElementB, int ElementC,
                                 const glm::dvec3& BaryCoords );

        /** updates the triangles array and optionally the element reference counts */
        void InternalSetTriangle( int tid, const Index3i& tv, bool bUpdateRefCounts,
                                  bool bAllowElementFreeing = true );
    };

    /**
     * DynamicMeshVectorOverlay is an convenient extension of DynamicMeshOverlay that adds
     * a specific N-element Vector type to the template, and adds accessor functions
     * that convert between that N-element vector type and the N-element arrays used by DynamicMeshOverlay.
     */
    template <typename RealType, int ElementSize, typename InVectorType>
    class DynamicMeshVectorOverlay : public DynamicMeshOverlay<RealType, ElementSize>
    {
    public:
        using BaseType   = DynamicMeshOverlay<RealType, ElementSize>;
        using VectorType = InVectorType;

        DynamicMeshVectorOverlay() : DynamicMeshOverlay<RealType, ElementSize>()
        {
        }

        DynamicMeshVectorOverlay( DynamicMesh3* parentMesh )
             : DynamicMeshOverlay<RealType, ElementSize>( parentMesh )
        {
        }

        /**
         * Append a new Element to the overlay
         */
        int AppendElement( const VectorType& Value )
        {
            // Cannot use cast operator here because Core Vector types do not define it.
            // However assuming that vector has .X member is also not good...
            // return BaseType::AppendElement((const RealType*)Value);
            return BaseType::AppendElement( &Value.x );
        }

        /**
         * Append a new Element to the overlay
         */
        int AppendElement( const RealType* Value )
        {
            return BaseType::AppendElement( Value );
        }

        /**
         * Get Element at a specific ID
         */
        [[nodiscard]] VectorType GetElement( int ElementID ) const
        {
            VectorType V;
            BaseType::GetElement( ElementID, V );
            return V;
        }

        /**
         * Get Element at a specific ID
         */
        void GetElement( int ElementID, VectorType& V ) const
        {
            BaseType::GetElement( ElementID, V );
        }

        /**
         * Get the Element value associated with a vertex of a triangle.
         */
        [[nodiscard]] VectorType GetElementAtVertex( int TriangleID, int VertexID ) const
        {
            VectorType V;
            BaseType::GetElementAtVertex( TriangleID, VertexID, V );
            return V;
        }

        /**
         * Get the Element value associated with a vertex of a triangle.
         */
        void GetElementAtVertex( int TriangleID, int VertexID, VectorType& V ) const
        {
            BaseType::GetElementAtVertex( TriangleID, VertexID, V );
        }

        /**
         * Get the Element associated with a vertex of a triangle
         * @param TriVertexIndex index of vertex in triangle, valid values are 0,1,2
         */
        void GetTriElement( int TriangleID, int32_t TriVertexIndex, VectorType& Value ) const
        {
            assert( TriVertexIndex >= 0 && TriVertexIndex <= 2 );
            GetElement( BaseType::m_ElementTriangles[( 3 * TriangleID ) + TriVertexIndex], Value );
        }

        /**
         * Get the three Elements associated with a triangle
         */
        void GetTriElements( int TriangleID, VectorType& A, VectorType& B, VectorType& C ) const
        {
            const int i = 3 * TriangleID;
            GetElement( BaseType::m_ElementTriangles[i], A );
            GetElement( BaseType::m_ElementTriangles[i + 1], B );
            GetElement( BaseType::m_ElementTriangles[i + 2], C );
        }

        /**
         * Set Element at a specific ID
         */
        void SetElement( int ElementID, const VectorType& Value )
        {
            BaseType::SetElement( ElementID, Value );
        }

        /**
         * Iterate through triangles connected to VertexID and call ProcessFunc for each per-triangle-vertex
         * Element with its Value. ProcessFunc must return true to continue the enumeration, or false to
         * early-terminate it
         *
         * @param bFindUniqueElements if true, ProcessFunc is only called once for each ElementID, otherwise it is
         * called once for each Triangle
         * @return true if at least one valid Element was found, ie if ProcessFunc was called at least one time
         */
        bool EnumerateVertexElements(
             int                                                                           VertexID,
             std::function<bool( int TriangleID, int ElementID, const VectorType& Value )> ProcessFunc,
             bool bFindUniqueElements = true ) const;
    };

#if PLATFORM_COMPILER_CLANG
#define GEOMETRYCORE_API
#else
#define UE_EXTERN_TEMPLATE_API
#endif

#if !UE_MERGED_MODULES
    extern template class DynamicMeshOverlay<float, 1>;
    extern template class DynamicMeshOverlay<double, 1>;
    extern template class DynamicMeshOverlay<int, 1>;
    extern template class DynamicMeshOverlay<float, 2>;
    extern template class DynamicMeshOverlay<double, 2>;
    extern template class DynamicMeshOverlay<int, 2>;
    extern template class DynamicMeshOverlay<float, 3>;
    extern template class DynamicMeshOverlay<double, 3>;
    extern template class DynamicMeshOverlay<int, 3>;
    extern template class DynamicMeshOverlay<float, 4>;
    extern template class DynamicMeshOverlay<double, 4>;

    extern template class DynamicMeshVectorOverlay<float, 2, glm::vec2>;
    extern template class DynamicMeshVectorOverlay<double, 2, glm::dvec2>;
    extern template class DynamicMeshVectorOverlay<float, 3, glm::vec3>;
    extern template class DynamicMeshVectorOverlay<double, 3, glm::dvec3>;
    extern template class DynamicMeshVectorOverlay<float, 4, glm::vec4>;
#endif

#undef UE_EXTERN_TEMPLATE_API

} // namespace Desert::Geometry