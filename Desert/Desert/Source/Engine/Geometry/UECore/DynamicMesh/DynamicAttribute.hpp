// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/DynamicMesh/DynamicAttribute.h:1-421, adapted: UE
// Core via UECore.hpp, namespace Desert::Geometry, TUniquePtr is std::unique_ptr; std::string is std::string;
// undo/redo change objects and FArchive serialization not ported.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include <Common/Core/Core.hpp>
#include <memory>
#include <utility>

namespace Desert::Geometry
{

    template <typename ParentType>
    class DynamicAttributeBase;

    /**
     * Base class for attributes that live on a dynamic mesh (or similar dynamic object)
     *
     * Subclasses can override the On* functions to ensure the attribute remains up to date through changes to the
     * dynamic object
     */
    template <typename ParentType>
    class DynamicAttributeBase
    {

    public:
        virtual ~DynamicAttributeBase() = default;

        /** Get optional identifier for this attribute set. */
        [[nodiscard]] std::string GetName() const
        {
            return m_Name;
        }

        /** Set optional identifier for this attribute set. */
        void SetName( std::string NameIn )
        {
            m_Name = std::move( NameIn );
        }

    protected:
        /** Optional std::string identifier for this attribute set. Not guaranteed to be unique. */
        std::string m_Name = std::string();

    public:
        /** Allocate a new copy of the attribute layer, optionally with a different parent */
        [[nodiscard]] virtual std::unique_ptr<DynamicAttributeBase> MakeCopy( ParentType* ParentIn ) const = 0;
        /** Allocate a new empty instance of the same type of attribute layer */
        [[nodiscard]] virtual std::unique_ptr<DynamicAttributeBase> MakeNew( ParentType* ParentIn ) const = 0;
        /**
         * Allocate a new compact copy of the attribute layer, optionally with a different parent.
         * Default implementation does a full copy and then compacts it, usually derived class will want to
         * override this with a more efficient direct compact copy implementation
         */
        [[nodiscard]] virtual std::unique_ptr<DynamicAttributeBase>
        MakeCompactCopy( const DynamicMeshCompactMaps& CompactMaps, ParentType* ParentIn ) const
        {
            std::unique_ptr<DynamicAttributeBase> Copy = MakeCopy( ParentIn );
            Copy->CompactInPlace( CompactMaps );
            return Copy;
        }

        /** Compact the attribute in place */
        virtual void CompactInPlace( const DynamicMeshCompactMaps& CompactMaps ) = 0;

        /** Update any held pointer to the parent */
        virtual void Reparent( ParentType* NewParent ) = 0;

        /**
         * Add elements from the Source attribute, using the AppendInfo to determine the range to copy.
         * @return true if the append succeeded, false otherwise (e.g. false if the data from the source attribute
         * was not compatible and the CopyOut failed to copy across)
         */
        virtual bool Append( const DynamicAttributeBase& Source, const DynamicMesh3::AppendInfo& AppendInfo ) = 0;

        /**
         * Add default-valued elements, using the AppendInfo to determine the range to add. Called when appending
         * from a mesh that does not have this attribute.
         */
        virtual void AppendDefaulted( const DynamicMesh3::AppendInfo& AppendInfo ) = 0;

        /** Generic function to copy data out of an attribute; it's up to the derived class to map RawID to chunks
         * of attribute data */
        virtual bool CopyOut( int RawID, void* Buffer, int BufferSize ) const = 0;

        /** Generic function to copy data in to an attribute; it's up to the derived class to map RawID to chunks
         * of attribute data */
        virtual bool CopyIn( int RawID, void* Buffer, int BufferSize ) = 0;

        virtual void OnNewVertex( int /*VertexID*/, bool /*bInserted*/ )
        {
        }

        virtual void OnRemoveVertex( int /*VertexID*/ )
        {
        }

        virtual void OnNewTriangle( int /*TriangleID*/, bool /*bInserted*/ )
        {
        }

        virtual void OnRemoveTriangle( int /*TriangleID*/ )
        {
        }

        virtual void OnReverseTriOrientation( int /*TriangleID*/ )
        {
        }

        /**
         * Check validity of attribute
         *
         * @param bAllowNonmanifold Accept non-manifold topology as valid. Note that this should almost always be
         * true for attributes; non-manifold overlays are generally valid.
         * @param FailMode Desired behavior if mesh is found invalid
         */
        [[nodiscard]] virtual bool CheckValidity( bool /*bAllowNonmanifold*/,
                                                  ValidityCheckFailMode /*FailMode*/ ) const
        {
            // default impl just doesn't check anything; override with any useful sanity checks
            return true;
        }

        /** Update to reflect an edge split in the parent mesh */
        virtual void OnSplitEdge( const DynamicMeshInfo::EdgeSplitInfo& /*SplitInfo*/ )
        {
        }

        /** Update to reflect an edge flip in the parent mesh */
        virtual void OnFlipEdge( const DynamicMeshInfo::EdgeFlipInfo& /*FlipInfo*/ )
        {
        }

        /** Update to reflect an edge collapse in the parent mesh */
        virtual void OnCollapseEdge( const DynamicMeshInfo::EdgeCollapseInfo& /*CollapseInfo*/ )
        {
        }

        /** Update to reflect a face poke in the parent mesh */
        virtual void OnPokeTriangle( const DynamicMeshInfo::PokeTriangleInfo& /*PokeInfo*/ )
        {
        }

        /** Update to reflect an edge merge in the parent mesh */
        virtual void OnMergeEdges( const DynamicMeshInfo::MergeEdgesInfo& /*MergeInfo*/ )
        {
        }

        /**
         * Update to reflect a vertex merge that does not resolve as a collapse or edge merge (i.e. a
         *  vertex merge that resolves as bowtie creation).
         */
        virtual void OnMergeVertices( const DynamicMeshInfo::MergeVerticesInfo& /*MergeInfo*/ )
        {
        }

        /** Update to reflect an edge merge in the parent mesh */
        virtual void OnSplitVertex( const DynamicMeshInfo::VertexSplitInfo& /*SplitInfo*/,
                                    const std::span<const int>& /*TrianglesToUpdate*/ )
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
        virtual void CopyParentClassData( const DynamicAttributeBase<ParentType>& Other )
        {
            m_Name = Other.m_Name;
        }
    };

    using DynamicMeshAttributeBase = DynamicAttributeBase<DynamicMesh3>;

    /**
     * Generic base class for managing a set of registered attributes that must all be kept up to date
     */
    template <typename ParentType>
    class DynamicAttributeSetBase
    {
    protected:
        // not managed by the base class; we should be able to register any attributes here that we want to be
        // automatically updated
        std::vector<DynamicAttributeBase<ParentType>*> m_RegisteredAttributes;

        /**
         * Stores the given attribute pointer in the attribute register, so that it will be updated with mesh
         * changes, but does not take ownership of the attribute memory.
         */
        void RegisterExternalAttribute( DynamicAttributeBase<ParentType>* Attribute )
        {
            m_RegisteredAttributes.push_back( Attribute );
        }

        void UnregisterExternalAttribute( DynamicAttributeBase<ParentType>* Attribute )
        {
            std::erase( m_RegisteredAttributes, Attribute );
        }

        void ResetRegisteredAttributes()
        {
            m_RegisteredAttributes.clear();
        }

    public:
        virtual ~DynamicAttributeSetBase() = default;

        [[nodiscard]] int NumRegisteredAttributes() const
        {
            return m_RegisteredAttributes.Num();
        }

        [[nodiscard]] DynamicAttributeBase<ParentType>* GetRegisteredAttribute( int Idx ) const
        {
            return m_RegisteredAttributes[Idx];
        }

        // These functions are called by the DynamicMesh3 to update the various
        // attributes when the parent mesh topology has been modified.
        virtual void OnNewTriangle( int TriangleID, bool bInserted )
        {
            for ( DynamicAttributeBase<ParentType>* A : m_RegisteredAttributes )
            {
                A->OnNewTriangle( TriangleID, bInserted );
            }
        }
        virtual void OnNewVertex( int VertexID, bool bInserted )
        {
            for ( DynamicAttributeBase<ParentType>* A : m_RegisteredAttributes )
            {
                A->OnNewVertex( VertexID, bInserted );
            }
        }
        virtual void OnRemoveTriangle( int TriangleID )
        {
            for ( DynamicAttributeBase<ParentType>* A : m_RegisteredAttributes )
            {
                A->OnRemoveTriangle( TriangleID );
            }
        }
        virtual void OnRemoveVertex( int VertexID )
        {
            for ( DynamicAttributeBase<ParentType>* A : m_RegisteredAttributes )
            {
                A->OnRemoveVertex( VertexID );
            }
        }
        virtual void OnReverseTriOrientation( int TriangleID )
        {
            for ( DynamicAttributeBase<ParentType>* A : m_RegisteredAttributes )
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
        [[nodiscard]] virtual bool CheckValidity( bool bAllowNonmanifold, ValidityCheckFailMode FailMode ) const
        {
            bool bValid = true;
            for ( DynamicAttributeBase<ParentType>* A : m_RegisteredAttributes )
            {
                bValid = A->CheckValidity( bAllowNonmanifold, FailMode ) && bValid;
            }
            return bValid;
        }

        // mesh-specific on* functions; may be split out

        virtual void OnSplitEdge( const DynamicMeshInfo::EdgeSplitInfo& SplitInfo )
        {
            for ( DynamicAttributeBase<ParentType>* A : m_RegisteredAttributes )
            {
                A->OnSplitEdge( SplitInfo );
            }
        }
        virtual void OnFlipEdge( const DynamicMeshInfo::EdgeFlipInfo& FlipInfo )
        {
            for ( DynamicAttributeBase<ParentType>* A : m_RegisteredAttributes )
            {
                A->OnFlipEdge( FlipInfo );
            }
        }
        virtual void OnCollapseEdge( const DynamicMeshInfo::EdgeCollapseInfo& CollapseInfo )
        {
            for ( DynamicAttributeBase<ParentType>* A : m_RegisteredAttributes )
            {
                A->OnCollapseEdge( CollapseInfo );
            }
        }
        virtual void OnPokeTriangle( const DynamicMeshInfo::PokeTriangleInfo& PokeInfo )
        {
            for ( DynamicAttributeBase<ParentType>* A : m_RegisteredAttributes )
            {
                A->OnPokeTriangle( PokeInfo );
            }
        }
        virtual void OnMergeEdges( const DynamicMeshInfo::MergeEdgesInfo& MergeInfo )
        {
            for ( DynamicAttributeBase<ParentType>* A : m_RegisteredAttributes )
            {
                A->OnMergeEdges( MergeInfo );
            }
        }
        virtual void OnMergeVertices( const DynamicMeshInfo::MergeVerticesInfo& MergeInfo )
        {
            if ( !Common::EnsureOrWarn( !MergeInfo.EdgeCollapseInfo.has_value(),
                                        "Vertex merge that resolves as edge collapse is expected to have called "
                                        "OnCollapseEdge, not OnMergeVertices." ) )
            {
                OnCollapseEdge( MergeInfo.EdgeCollapseInfo.value() );
                return;
            }
            if ( !Common::EnsureOrWarn( !MergeInfo.MergeEdgesInfo.has_value(),
                                        "Vertex merge that resolves as edge merge is expected to have called "
                                        "OnMergeEdges, not OnMergeVertices." ) )
            {
                OnMergeEdges( MergeInfo.MergeEdgesInfo.value() );
                return;
            }

            for ( DynamicAttributeBase<ParentType>* A : m_RegisteredAttributes )
            {
                A->OnMergeVertices( MergeInfo );
            }
        }
        virtual void OnSplitVertex( const DynamicMeshInfo::VertexSplitInfo& SplitInfo,
                                    const std::span<const int>&             TrianglesToUpdate )
        {
            for ( DynamicAttributeBase<ParentType>* A : m_RegisteredAttributes )
            {
                A->OnSplitVertex( SplitInfo, TrianglesToUpdate );
            }
        }
    };

    using DynamicMeshAttributeSetBase = DynamicAttributeSetBase<DynamicMesh3>;

} // namespace Desert::Geometry
