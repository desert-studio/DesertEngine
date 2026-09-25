// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/DynamicMesh/DynamicMeshAttributeSet.h:1-765,
// adapted: UE Core via UECore.hpp, namespace Desert::Geometry; TIndirectArray is std::vector<std::unique_ptr>,
// TUniquePtr is std::unique_ptr, FName is std::string; ported layers: UV, normal (+tangent/bitangent), colour,
// MaterialID, polygroup layers and generic attached attributes. NOT ported: weight layers, triangle labels, skin
// weights, morph targets, bones, sculpt layers (no Desert consumer), IsSameAs and FArchive serialization.

#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicAttribute.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshOverlay.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshTriangleAttribute.hpp"

#include <memory>
#include <vector>

namespace Desert::Geometry
{

    /** Standard UV overlay type - 2-element float */
    typedef TDynamicMeshVectorOverlay<float, 2, FVector2f> FDynamicMeshUVOverlay;
    /** Standard Normal overlay type - 3-element float */
    typedef TDynamicMeshVectorOverlay<float, 3, FVector3f> FDynamicMeshNormalOverlay;
    /** Standard Color overlay type - 4-element float (rbga) */
    typedef TDynamicMeshVectorOverlay<float, 4, FVector4f> FDynamicMeshColorOverlay;
    /** Standard per-triangle integer material ID */
    using FDynamicMeshMaterialAttribute = TDynamicMeshScalarTriangleAttribute<int32_t>;

    /** Per-triangle integer polygroup ID */
    using FDynamicMeshPolygroupAttribute = TDynamicMeshScalarTriangleAttribute<int32_t>;

    /**
     * FDynamicMeshAttributeSet manages a set of extended attributes for a FDynamicMesh3.
     * This includes UV and Normal overlays, etc.
     *
     * Currently the default is to always have one UV layer and one Normal layer, but the number of layers can be
     * requested on construction.
     *
     * @todo current internal structure is a work-in-progress
     */
    class FDynamicMeshAttributeSet : public FDynamicMeshAttributeSetBase
    {
    public:
        FDynamicMeshAttributeSet( FDynamicMesh3* Mesh );

        FDynamicMeshAttributeSet( FDynamicMesh3* Mesh, int32_t NumUVLayers, int32_t NumNormalLayers );

        virtual ~FDynamicMeshAttributeSet() override;

        void Copy( const FDynamicMeshAttributeSet& Copy );

        /** returns true if the attached overlays/attributes are compact */
        bool IsCompact() const;

        /**
         * Performs a CompactCopy of the attached overlays/attributes.
         * Called by the parent mesh CompactCopy function.
         *
         * @param CompactMaps Maps indicating how vertices and triangles were changes in the parent
         * @param Copy The attribute set to be copied
         */
        void CompactCopy( const FCompactMaps& CompactMaps, const FDynamicMeshAttributeSet& Copy );

        /**
         * Compacts the attribute set in place
         * Called by the parent mesh CompactInPlace function
         *
         * @param CompactMaps Maps of how the vertices and triangles were compacted in the parent
         */
        void CompactInPlace( const FCompactMaps& CompactMaps );

        /**
         * Split all bowtie vertices in all layers
         * @param bParallel if true, layers are processed in parallel
         */
        void SplitAllBowties( bool bParallel = true );

        /**
         * Enable the matching attributes and overlay layers as the reference Copy set, but do not copy any data
         * across. If bClearExisting=true, all existing attributes are cleared, so after calling this function this
         * set has the same attributes as ToMatch. If bDiscardExtraAttributes=true and bClearExisting=false, extra
         * attributes not in ToMatch are discarded, but existing attributes are not cleared/reset
         */
        void EnableMatchingAttributes( const FDynamicMeshAttributeSet& ToMatch, bool bClearExisting = true,
                                       bool bDiscardExtraAttributes = false );

        /** @return the parent mesh for this overlay */
        const FDynamicMesh3* GetParentMesh() const
        {
            return ParentMesh;
        }
        /** @return the parent mesh for this overlay */
        FDynamicMesh3* GetParentMesh()
        {
            return ParentMesh;
        }

    private:
        /** @set the parent mesh for this overlay.  Only safe for use during FDynamicMesh move */
        void Reparent( FDynamicMesh3* NewParent );

    public:
        /** @return true if the given edge is a seam edge in any overlay */
        virtual bool IsSeamEdge( int EdgeID ) const;

        /** @return true if the given edge is the termination of a seam in any overlay*/
        virtual bool IsSeamEndEdge( int EdgeID ) const;

        /** @return true if the given edge is a seam edge in any overlay, and reports which overlay types */
        virtual bool IsSeamEdge( int EdgeID, bool& bIsUVSeamOut, bool& bIsNormalSeamOut, bool& bIsColorSeamOut,
                                 bool& bIsTangentSeamOut ) const;

        /** @return true if the given vertex is a seam vertex in any overlay */
        virtual bool IsSeamVertex( int VertexID, bool bBoundaryIsSeam = true ) const;

        /** @return true if the given vertex is a seam intersection vertex in any overlay */
        [[nodiscard]] virtual bool IsSeamIntersectionVertex( int32_t VertexID ) const;

        /** @return true if the given edge is a material ID boundary */
        virtual bool IsMaterialBoundaryEdge( int EdgeID ) const;

        //
        // UV Layers
        //

        /** @return number of UV layers */
        virtual int NumUVLayers() const
        {
            return (int)UVLayers.size();
        }

        /** Set number of UV (2-vector float overlay) layers */
        virtual void SetNumUVLayers( int Num );

        /** @return the UV layer at the given Index  if exists, else nullptr */
        FDynamicMeshUVOverlay* GetUVLayer( int Index )
        {
            return ( Index >= 0 && Index < NumUVLayers() ) ? UVLayers[Index].get() : nullptr;
        }

        /** @return the UV layer at the given Index  if exists, else nullptr */
        const FDynamicMeshUVOverlay* GetUVLayer( int Index ) const
        {
            return ( Index >= 0 && Index < NumUVLayers() ) ? UVLayers[Index].get() : nullptr;
        }

        /** @return the primary UV layer (layer 0) */
        FDynamicMeshUVOverlay* PrimaryUV()
        {
            return GetUVLayer( 0 );
        }
        /** @return the primary UV layer (layer 0) */
        const FDynamicMeshUVOverlay* PrimaryUV() const
        {
            return GetUVLayer( 0 );
        }

        //
        // Normal Layers
        //

        /** @return number of Normals layers */
        virtual int NumNormalLayers() const
        {
            return (int)NormalLayers.size();
        }

        /** Set number of Normals (3-vector float overlay) layers */
        virtual void SetNumNormalLayers( int Num );

        /** Enable Tangents overlays (Tangent = Normal layer 1, Bitangent = Normal layer 2) */
        void EnableTangents();

        /** Disable Tangents overlays */
        void DisableTangents();

        /** @return the Normal layer at the given Index  if exists, else nullptr */
        FDynamicMeshNormalOverlay* GetNormalLayer( int Index )
        {
            return ( Index >= 0 && Index < NumNormalLayers() ) ? NormalLayers[Index].get() : nullptr;
        }

        /** @return the Normal layer at the given Index  if exists, else nullptr */
        const FDynamicMeshNormalOverlay* GetNormalLayer( int Index ) const
        {
            return ( Index >= 0 && Index < NumNormalLayers() ) ? NormalLayers[Index].get() : nullptr;
        }

        /** @return the primary Normal layer (normal layer 0) if it exists */
        FDynamicMeshNormalOverlay* PrimaryNormals()
        {
            return GetNormalLayer( 0 );
        }
        /** @return the primary Normal layer (normal layer 0) if it exists */
        const FDynamicMeshNormalOverlay* PrimaryNormals() const
        {
            return GetNormalLayer( 0 );
        }
        /** @return the primary tangent layer (normal layer 1) if it exists */
        FDynamicMeshNormalOverlay* PrimaryTangents()
        {
            return GetNormalLayer( 1 );
        }
        /** @return the primary tangent layer (normal layer 1) if it exists */
        const FDynamicMeshNormalOverlay* PrimaryTangents() const
        {
            return GetNormalLayer( 1 );
        }
        /** @return the primary bitangent layer (normal layer 2) if it exists */
        FDynamicMeshNormalOverlay* PrimaryBiTangents()
        {
            return GetNormalLayer( 2 );
        }
        /** @return the primary bitangent layer (normal layer 2) if it exists */
        const FDynamicMeshNormalOverlay* PrimaryBiTangents() const
        {
            return GetNormalLayer( 2 );
        }

        /** @return true if normal layers exist for the normal, tangent, and bitangent */
        bool HasTangentSpace() const
        {
            return ( PrimaryNormals() != nullptr && PrimaryTangents() != nullptr &&
                     PrimaryBiTangents() != nullptr );
        }

        bool HasPrimaryColors() const
        {
            return !!ColorLayer;
        }

        FDynamicMeshColorOverlay* PrimaryColors()
        {
            return ColorLayer.get();
        }

        const FDynamicMeshColorOverlay* PrimaryColors() const
        {
            return ColorLayer.get();
        }

        void EnablePrimaryColors();

        void DisablePrimaryColors();

        //
        // Polygroup layers
        //

        /** @return number of Polygroup layers */
        [[nodiscard]] virtual int32_t NumPolygroupLayers() const;

        /** Set the number of Polygroup layers */
        virtual void SetNumPolygroupLayers( int32_t Num );

        /** @return the Polygroup layer at the given Index */
        FDynamicMeshPolygroupAttribute* GetPolygroupLayer( int Index );

        /** @return the Polygroup layer at the given Index */
        const FDynamicMeshPolygroupAttribute* GetPolygroupLayer( int Index ) const;

        //
        // Per-Triangle Material ID
        //

        bool HasMaterialID() const
        {
            return !!MaterialIDAttrib;
        }

        void EnableMaterialID();

        void DisableMaterialID();

        FDynamicMeshMaterialAttribute* GetMaterialID()
        {
            return MaterialIDAttrib.get();
        }

        const FDynamicMeshMaterialAttribute* GetMaterialID() const
        {
            return MaterialIDAttrib.get();
        }

        //
        // Generic attributes
        //

        void AttachAttribute( const std::string& AttribName, FDynamicMeshAttributeBase* Attribute )
        {
            if ( GenericAttributes.Contains( AttribName ) )
            {
                UnregisterExternalAttribute( GenericAttributes[AttribName].get() );
            }
            GenericAttributes.FindOrAdd( AttribName ) = std::unique_ptr<FDynamicMeshAttributeBase>( Attribute );
            RegisterExternalAttribute( Attribute );
        }

        void RemoveAttribute( const std::string& AttribName )
        {
            if ( GenericAttributes.Contains( AttribName ) )
            {
                UnregisterExternalAttribute( GenericAttributes[AttribName].get() );
                GenericAttributes.Remove( AttribName );
            }
        }

        FDynamicMeshAttributeBase* GetAttachedAttribute( const std::string& AttribName )
        {
            return GenericAttributes.Contains( AttribName ) ? GenericAttributes[AttribName].get() : nullptr;
        }

        [[nodiscard]] const FDynamicMeshAttributeBase* GetAttachedAttribute( const std::string& AttribName ) const
        {
            const std::unique_ptr<FDynamicMeshAttributeBase>* Found = GenericAttributes.Find( AttribName );
            return Found ? Found->get() : nullptr;
        }

        int NumAttachedAttributes() const
        {
            return GenericAttributes.Num();
        }

        [[nodiscard]] bool HasAttachedAttribute( const std::string& AttribName ) const
        {
            return GenericAttributes.Contains( AttribName );
        }

        [[nodiscard]] size_t GetByteCount() const;

    protected:
        /** Parent mesh of this attribute set */
        FDynamicMesh3* ParentMesh;

        std::vector<std::unique_ptr<FDynamicMeshUVOverlay>>     UVLayers;
        std::vector<std::unique_ptr<FDynamicMeshNormalOverlay>> NormalLayers;
        std::unique_ptr<FDynamicMeshColorOverlay>               ColorLayer;

        std::unique_ptr<FDynamicMeshMaterialAttribute> MaterialIDAttrib;

        std::vector<std::unique_ptr<FDynamicMeshPolygroupAttribute>> PolygroupLayers;

        using GenericAttributesMap = TMap<std::string, std::unique_ptr<FDynamicMeshAttributeBase>>;
        GenericAttributesMap GenericAttributes;

    protected:
        friend class FDynamicMesh3;

        /**
         * Initialize the existing attribute layers with the given vertex and triangle sizes
         */
        void Initialize( int MaxVertexID, int MaxTriangleID )
        {
            for ( std::unique_ptr<FDynamicMeshUVOverlay>& UVLayer : UVLayers )
            {
                UVLayer->InitializeTriangles( MaxTriangleID );
            }
            for ( std::unique_ptr<FDynamicMeshNormalOverlay>& NormalLayer : NormalLayers )
            {
                NormalLayer->InitializeTriangles( MaxTriangleID );
            }
        }

        // These functions are called by the FDynamicMesh3 to update the various
        // attributes when the parent mesh topology has been modified.
        // TODO: would it be better to register all the overlays and attributes with the base set and not overload
        // these?  maybe!
        virtual void OnNewTriangle( int TriangleID, bool bInserted ) override;
        virtual void OnNewVertex( int VertexID, bool bInserted ) override;
        virtual void OnRemoveTriangle( int TriangleID ) override;
        virtual void OnRemoveVertex( int VertexID ) override;
        virtual void OnReverseTriOrientation( int TriangleID ) override;
        virtual void OnSplitEdge( const DynamicMeshInfo::FEdgeSplitInfo& splitInfo ) override;
        virtual void OnFlipEdge( const DynamicMeshInfo::FEdgeFlipInfo& flipInfo ) override;
        virtual void OnCollapseEdge( const DynamicMeshInfo::FEdgeCollapseInfo& collapseInfo ) override;
        virtual void OnPokeTriangle( const DynamicMeshInfo::FPokeTriangleInfo& pokeInfo ) override;
        virtual void OnMergeEdges( const DynamicMeshInfo::FMergeEdgesInfo& mergeInfo ) override;
        virtual void OnMergeVertices( const DynamicMeshInfo::FMergeVerticesInfo& mergeInfo ) override;
        virtual void OnSplitVertex( const DynamicMeshInfo::FVertexSplitInfo& SplitInfo,
                                    const TArrayView<const int>&             TrianglesToUpdate ) override;

        /**
         * Check validity of attributes
         *
         * @param bAllowNonmanifold Accept non-manifold topology as valid. Note that this should almost always be
         * true for attributes; non-manifold overlays are generally valid.
         * @param FailMode Desired behavior if mesh is found invalid
         */
        virtual bool CheckValidity( bool bAllowNonmanifold, EValidityCheckFailMode FailMode ) const override;

    private:
        void Append( const FDynamicMeshAttributeSet& ToAppend, const FDynamicMesh3::FAppendInfo& AppendInfo );
        void AppendDefaulted( const FDynamicMesh3::FAppendInfo& AppendInfo );
    };

} // namespace Desert::Geometry
