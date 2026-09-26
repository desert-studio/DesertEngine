// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Polygroups/PolygroupSet.h:1-161, adapted: UE
// Core via UECore.hpp, namespace Desert::Geometry, TUniquePtr is std::unique_ptr; std::string layer lookup by
// std::string.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"

namespace Desert::Geometry
{

    /**
     * PolygroupLayer represents a polygroup set on a DynamicMesh3, which supports a "default"
     * group set stored on the mesh, and then N extended group layers stored in the mesh AttributeSet.
     * This struct can represent either.
     */
    struct PolygroupLayer
    {
        /** If true, layer is the default DynamicMesh3 triangle groups layer */
        bool bIsDefaultLayer = true;
        /** If bIsDefaultLayer is false, this is the index of the AttributeSet Polygroup Layer */
        int32_t LayerIndex = -1;

        /** Construct a PolygroupLayer for the default layer */
        static PolygroupLayer Default()
        {
            return PolygroupLayer{ true, -1 };
        }
        /** Construct a PolygroupLayer for an extended layer */
        static PolygroupLayer Layer( int32_t Index )
        {
            return PolygroupLayer{ false, Index };
        }

        bool operator==( const PolygroupLayer& OtherLayer ) const
        {
            if ( bIsDefaultLayer || OtherLayer.bIsDefaultLayer )
            {
                return bIsDefaultLayer && OtherLayer.bIsDefaultLayer;
            }

            return LayerIndex == OtherLayer.LayerIndex;
        }

        /** @return true if the specified layer (default or extended) exist and is initialized on the given Mesh */
        bool CheckExists( const DynamicMesh3* Mesh ) const;

        /** Enable the specified layer on the given Mesh, if it's not already there */
        void EnableOnMesh( DynamicMesh3& Mesh ) const;
    };

    /**
     * Polygroup sets can be stored in multiple places. The default location is in the per-triangle group integer
     * stored directly on a DynamicMesh3. Additional layers may be stored in the DynamicMeshAttributeSet. Future
     * iterations could store packed polygroups in other places, store them in separate arrays, and so on.
     * PolygroupSet can be used to abstract these different cases, by providing a standard Polygroup Get/Set API.
     *
     * To support unique Polygroup ID allocation, PolygroupSet calculates the maximum GroupID on creation, and
     * updates this maximum across SetGroup() calls. AllocateNewGroupID() can be used to provide new unused
     * GroupIDs. For consistency with DynamicMesh3, MaxGroupID is set such that all GroupIDs are less than
     * MaxGroupID
     *
     */
    struct PolygroupSet
    {
        const DynamicMesh3*                   Mesh            = nullptr;
        const DynamicMeshPolygroupAttribute*  PolygroupAttrib = nullptr;
        int32_t                               GroupLayerIndex = -1;
        int32_t                               MaxGroupID      = 0; // Note: all group IDs are less than MaxGroupID

        /** Initialize a PolygroupSet for the given Mesh, and standard triangle group layer */
        explicit PolygroupSet( const DynamicMesh3* MeshIn );

        /** Initialize a PolygroupSet for the given Mesh, and standard triangle group layer */
        explicit PolygroupSet( const DynamicMesh3* MeshIn, PolygroupLayer GroupLayer );

        /** Initialize a PolygroupSet for given Mesh and specific Polygroup attribute layer */
        explicit PolygroupSet( const DynamicMesh3*                  MeshIn,
                               const DynamicMeshPolygroupAttribute* PolygroupAttribIn );

        /** Initialize a PolygroupSet for given Mesh and specific Polygroup attribute layer, found by index. If not
         * valid, fall back to standard triangle group layer. */
        explicit PolygroupSet( const DynamicMesh3* MeshIn, int32_t PolygroupLayerIndex );

        /** Initialize a PolygroupSet for given Mesh and specific Polygroup attribute layer, found by name. If not
         * valid, fall back to standard triangle group layer. */
        explicit PolygroupSet( const DynamicMesh3* MeshIn, const std::string& AttribName );

        /** Initialize a PolygroupSet by copying an existing PolygroupSet */
        explicit PolygroupSet( const PolygroupSet* CopyIn );

        /** @return Mesh this PolygroupSet references  */
        const DynamicMesh3* GetMesh() const
        {
            return Mesh;
        }

        /** @return PolygroupAttribute this PolygroupSet references, or null if no PolygroupAttribute is in use */
        const DynamicMeshPolygroupAttribute* GetPolygroup() const
        {
            return PolygroupAttrib;
        }

        /** @return index of current PolygroupAttribute into Mesh AttributeSet, or -1 if this information does not
         * exist */
        [[nodiscard]] int32_t GetPolygroupIndex() const
        {
            return GroupLayerIndex;
        }

        /**
         * @return PolygroupID for a TriangleID
         */
        [[nodiscard]] int32_t GetGroup( int32_t TriangleID ) const
        {
            return ( ( PolygroupAttrib ) != nullptr ) ? PolygroupAttrib->GetValue( TriangleID )
                                                      : Mesh->GetTriangleGroup( TriangleID );
        }

        /**
         * @return PolygroupID for a TriangleID
         */
        [[nodiscard]] int32_t GetTriangleGroup( int32_t TriangleID ) const
        {
            return ( ( PolygroupAttrib ) != nullptr ) ? PolygroupAttrib->GetValue( TriangleID )
                                                      : Mesh->GetTriangleGroup( TriangleID );
        }

        /**
         * Set the PolygroupID for a TriangleID
         */
        void SetGroup( int32_t TriangleID, int32_t NewGroupID, DynamicMesh3& WritableMesh )
        {
            assert( &WritableMesh == this->Mesh ); // require the same mesh
            if ( WritableMesh.IsTriangle( TriangleID ) )
            {
                if ( PolygroupAttrib != nullptr )
                {
                    DynamicMeshPolygroupAttribute* WritableGroupAttrib =
                         WritableMesh.Attributes()->GetPolygroupLayer( GroupLayerIndex );
                    assert( WritableGroupAttrib == PolygroupAttrib );
                    WritableGroupAttrib->SetValue( TriangleID, NewGroupID );
                }
                else
                {
                    WritableMesh.SetTriangleGroup( TriangleID, NewGroupID );
                }
            }
            MaxGroupID = std::max( MaxGroupID, NewGroupID + 1 );
        }

        /**
         * Calculate the current maximum PolygroupID used in the active set and store in MaxGroupID member
         */
        void RecalculateMaxGroupID();

        /**
         * Allocate a new unused PolygroupID by incrementing the MaxGroupID member
         */
        int32_t AllocateNewGroupID()
        {
            return MaxGroupID++;
        }
    };

} // namespace Desert::Geometry
