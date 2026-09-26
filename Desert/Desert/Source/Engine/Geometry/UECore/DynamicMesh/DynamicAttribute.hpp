// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/DynamicMesh/DynamicAttribute.h:1-421, adapted: UE
// Core via UECore.hpp, namespace Desert::Geometry, TUniquePtr is std::unique_ptr; std::string is std::string;
// undo/redo change objects and FArchive serialization not ported.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"

namespace Desert::Geometry
{

    template <typename ParentType>
    class TDynamicAttributeBase;

    /**
     * Base class for attributes that live on a dynamic mesh (or similar dynamic object)
     *
     * Subclasses can override the On* functions to ensure the attribute remains up to date through changes to the
     * dynamic object
     */
    template <typename ParentType>
    class TDynamicAttributeBase
    {

    public:
        virtual ~TDynamicAttributeBase() = default;

    public:
        /** Get optional identifier for this attribute set. */
        [[nodiscard]] std::string GetName() const
        {
            return Name;
        }

        /** Set optional identifier for this attribute set. */
        void SetName( std::string NameIn )
        {
            Name = NameIn;
        }

    protected:
        /** Optional std::string identifier for this attribute set. Not guaranteed to be unique. */
        std::string Name = std::string();

    public:
        /** Allocate a new copy of the attribute layer, optionally with a different parent */
        virtual TDynamicAttributeBase* MakeCopy( ParentType* ParentIn ) const = 0;
        /** Allocate a new empty instance of the same type of attribute layer */
        virtual TDynamicAttributeBase* MakeNew( ParentType* ParentIn ) const = 0;
        /**
         * Allocate a new compact copy of the attribute layer, optionally with a different parent.
         * Default implementation does a full copy and then compacts it, usually derived class will want to
         * override this with a more efficient direct compact copy implementation
         */
        virtual TDynamicAttributeBase* MakeCompactCopy( const FCompactMaps& CompactMaps,
                                                        ParentType*         ParentIn ) const
        {
            TDynamicAttributeBase* Copy = MakeCopy( ParentIn );
            Copy->CompactInPlace( CompactMaps );
            return Copy;
        }

        /** Compact the attribute in place */
        virtual void CompactInPlace( const FCompactMaps& CompactMaps ) = 0;

        /** Update any held pointer to the parent */
        virtual void Reparent( ParentType* NewParent ) = 0;

        /**
         * Add elements from the Source attribute, using the AppendInfo to determine the range to copy.
         * @return true if the append succeeded, false otherwise (e.g. false if the data from the source attribute
         * was not compatible and the CopyOut failed to copy across)
         */
        virtual bool Append( const TDynamicAttributeBase&      Source,
                             const FDynamicMesh3::FAppendInfo& AppendInfo ) = 0;

        /**
         * Add default-valued elements, using the AppendInfo to determine the range to add. Called when appending
         * from a mesh that does not have this attribute.
         */
        virtual void AppendDefaulted( const FDynamicMesh3::FAppendInfo& AppendInfo ) = 0;

        /** Generic function to copy data out of an attribute; it's up to the derived class to map RawID to chunks
         * of attribute data */
        virtual bool CopyOut( int RawID, void* Buffer, int BufferSize ) const = 0;

        /** Generic function to copy data in to an attribute; it's up to the derived class to map RawID to chunks
         * of attribute data */
        virtual bool CopyIn( int RawID, void* Buffer, int BufferSize ) = 0;

        virtual void OnNewVertex( int VertexID, bool bInserted )
        {
        }

        virtual void OnRemoveVertex( int VertexID )
        {
        }

        virtual void OnNewTriangle( int TriangleID, bool bInserted )
        {
        }

        virtual void OnRemoveTriangle( int TriangleID )
        {
        }

        virtual void OnReverseTriOrientation( int TriangleID )
        {
        }

        /**
         * Check validity of attribute
         *
         * @param bAllowNonmanifold Accept non-manifold topology as valid. Note that this should almost always be
         * true for attributes; non-manifold overlays are generally valid.
         * @param FailMode Desired behavior if mesh is found invalid
         */
        virtual bool CheckValidity( bool bAllowNonmanifold, EValidityCheckFailMode FailMode ) const
        {
            // default impl just doesn't check anything; override with any useful sanity checks
            return true;
        }

        /** Update to reflect an edge split in the parent mesh */
        virtual void OnSplitEdge( const DynamicMeshInfo::FEdgeSplitInfo& SplitInfo )
        {
        }

        /** Update to reflect an edge flip in the parent mesh */
        virtual void OnFlipEdge( const DynamicMeshInfo::FEdgeFlipInfo& FlipInfo )
        {
        }

        /** Update to reflect an edge collapse in the parent mesh */
        virtual void OnCollapseEdge( const DynamicMeshInfo::FEdgeCollapseInfo& CollapseInfo )
        {
        }

        /** Update to reflect a face poke in the parent mesh */
        virtual void OnPokeTriangle( const DynamicMeshInfo::FPokeTriangleInfo& PokeInfo )
        {
        }

        /** Update to reflect an edge merge in the parent mesh */
        virtual void OnMergeEdges( const DynamicMeshInfo::FMergeEdgesInfo& MergeInfo )
        {
        }

        /**
         * Update to reflect a vertex merge that does not resolve as a collapse or edge merge (i.e. a
         *  vertex merge that resolves as bowtie creation).
         */
        virtual void OnMergeVertices( const DynamicMeshInfo::FMergeVerticesInfo& MergeInfo )
        {
        }

        /** Update to reflect an edge merge in the parent mesh */
        virtual void OnSplitVertex( const DynamicMeshInfo::FVertexSplitInfo& SplitInfo,
                                    const TArrayView<const int>&             TrianglesToUpdate )
        {
        }

        [[nodiscard]] virtual size_t GetByteCount() const
        {
            return 0;
        }

    protected:
        /**
         * Implementation of parent-class copy. MakeCopy() and MakeCompactCopy() implementations should call
         * this to transfer any custom data added by parent attribute set class.
         */
        virtual void CopyParentClassData( const TDynamicAttributeBase<ParentType>& Other )
        {
            Name = Other.Name;
        }
    };

    typedef TDynamicAttributeBase<FDynamicMesh3> FDynamicMeshAttributeBase;

    /**
     * Generic base class for managing a set of registered attributes that must all be kept up to date
     */
    template <typename ParentType>
    class TDynamicAttributeSetBase
    {
    protected:
        // not managed by the base class; we should be able to register any attributes here that we want to be
        // automatically updated
        TArray<TDynamicAttributeBase<ParentType>*> RegisteredAttributes;

        /**
         * Stores the given attribute pointer in the attribute register, so that it will be updated with mesh
         * changes, but does not take ownership of the attribute memory.
         */
        void RegisterExternalAttribute( TDynamicAttributeBase<ParentType>* Attribute )
        {
            RegisteredAttributes.Add( Attribute );
        }

        void UnregisterExternalAttribute( TDynamicAttributeBase<ParentType>* Attribute )
        {
            RegisteredAttributes.Remove( Attribute );
        }

        void ResetRegisteredAttributes()
        {
            RegisteredAttributes.Reset();
        }

    public:
        virtual ~TDynamicAttributeSetBase() = default;

        int NumRegisteredAttributes() const
        {
            return RegisteredAttributes.Num();
        }

        TDynamicAttributeBase<ParentType>* GetRegisteredAttribute( int Idx ) const
        {
            return RegisteredAttributes[Idx];
        }

        // These functions are called by the FDynamicMesh3 to update the various
        // attributes when the parent mesh topology has been modified.
        virtual void OnNewTriangle( int TriangleID, bool bInserted )
        {
            for ( TDynamicAttributeBase<ParentType>* A : RegisteredAttributes )
            {
                A->OnNewTriangle( TriangleID, bInserted );
            }
        }
        virtual void OnNewVertex( int VertexID, bool bInserted )
        {
            for ( TDynamicAttributeBase<ParentType>* A : RegisteredAttributes )
            {
                A->OnNewVertex( VertexID, bInserted );
            }
        }
        virtual void OnRemoveTriangle( int TriangleID )
        {
            for ( TDynamicAttributeBase<ParentType>* A : RegisteredAttributes )
            {
                A->OnRemoveTriangle( TriangleID );
            }
        }
        virtual void OnRemoveVertex( int VertexID )
        {
            for ( TDynamicAttributeBase<ParentType>* A : RegisteredAttributes )
            {
                A->OnRemoveVertex( VertexID );
            }
        }
        virtual void OnReverseTriOrientation( int TriangleID )
        {
            for ( TDynamicAttributeBase<ParentType>* A : RegisteredAttributes )
            {
                A->OnReverseTriOrientation( TriangleID );
            }
        }

        /**
         * Check validity of attributes
         *
         * @param bAllowNonmanifold Accept non-manifold topology as valid. Note that this should almost always be
         * true for attributes; non-manifold overlays are generally valid.
         * @param FailMode Desired behavior if mesh is found invalid
         */
        virtual bool CheckValidity( bool bAllowNonmanifold, EValidityCheckFailMode FailMode ) const
        {
            bool bValid = true;
            for ( TDynamicAttributeBase<ParentType>* A : RegisteredAttributes )
            {
                bValid = A->CheckValidity( bAllowNonmanifold, FailMode ) && bValid;
            }
            return bValid;
        }

        // mesh-specific on* functions; may be split out
    public:
        virtual void OnSplitEdge( const DynamicMeshInfo::FEdgeSplitInfo& SplitInfo )
        {
            for ( TDynamicAttributeBase<ParentType>* A : RegisteredAttributes )
            {
                A->OnSplitEdge( SplitInfo );
            }
        }
        virtual void OnFlipEdge( const DynamicMeshInfo::FEdgeFlipInfo& FlipInfo )
        {
            for ( TDynamicAttributeBase<ParentType>* A : RegisteredAttributes )
            {
                A->OnFlipEdge( FlipInfo );
            }
        }
        virtual void OnCollapseEdge( const DynamicMeshInfo::FEdgeCollapseInfo& CollapseInfo )
        {
            for ( TDynamicAttributeBase<ParentType>* A : RegisteredAttributes )
            {
                A->OnCollapseEdge( CollapseInfo );
            }
        }
        virtual void OnPokeTriangle( const DynamicMeshInfo::FPokeTriangleInfo& PokeInfo )
        {
            for ( TDynamicAttributeBase<ParentType>* A : RegisteredAttributes )
            {
                A->OnPokeTriangle( PokeInfo );
            }
        }
        virtual void OnMergeEdges( const DynamicMeshInfo::FMergeEdgesInfo& MergeInfo )
        {
            for ( TDynamicAttributeBase<ParentType>* A : RegisteredAttributes )
            {
                A->OnMergeEdges( MergeInfo );
            }
        }
        virtual void OnMergeVertices( const DynamicMeshInfo::FMergeVerticesInfo& MergeInfo )
        {
            if ( !UE_ENSURE_MSGF( !MergeInfo.EdgeCollapseInfo.IsSet(),
                                  TEXT( "Vertex merge that resolves as edge collapse "
                                        "is expected to have called OnCollapseEdge, not OnMergeVertices." ) ) )
            {
                OnCollapseEdge( MergeInfo.EdgeCollapseInfo.GetValue() );
                return;
            }
            if ( !UE_ENSURE_MSGF( !MergeInfo.MergeEdgesInfo.IsSet(),
                                  TEXT( "Vertex merge that resolves as edge merge "
                                        "is expected to have called OnMergeEdges, not OnMergeVertices." ) ) )
            {
                OnMergeEdges( MergeInfo.MergeEdgesInfo.GetValue() );
                return;
            }

            for ( TDynamicAttributeBase<ParentType>* A : RegisteredAttributes )
            {
                A->OnMergeVertices( MergeInfo );
            }
        }
        virtual void OnSplitVertex( const DynamicMeshInfo::FVertexSplitInfo& SplitInfo,
                                    const TArrayView<const int>&             TrianglesToUpdate )
        {
            for ( TDynamicAttributeBase<ParentType>* A : RegisteredAttributes )
            {
                A->OnSplitVertex( SplitInfo, TrianglesToUpdate );
            }
        }
    };

    typedef TDynamicAttributeSetBase<FDynamicMesh3> FDynamicMeshAttributeSetBase;

} // namespace Desert::Geometry
