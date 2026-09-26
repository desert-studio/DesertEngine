// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/DynamicMesh/DynamicMeshAttributeSet.h:1-765,
// adapted: UE Core via UECore.hpp, namespace Desert::Geometry; TIndirectArray is std::vector<std::unique_ptr>,
// TUniquePtr is std::unique_ptr, std::string is std::string; ported layers: UV, normal (+tangent/bitangent),
// colour, MaterialID, polygroup layers and generic attached attributes. NOT ported: weight layers, triangle
// labels, skin weights, morph targets, bones, sculpt layers (no Desert consumer), IsSameAs and FArchive
// serialization.

#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"
#include "Engine/Geometry/UECore/MapLookup.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicAttribute.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshOverlay.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshTriangleAttribute.hpp"

#include <memory>
#include <vector>

namespace Desert::Geometry
{

    /** Standard UV overlay type - 2-element float */
    using DynamicMeshUVOverlay = DynamicMeshVectorOverlay<float, 2, glm::vec2>;
    /** Standard Normal overlay type - 3-element float */
    using DynamicMeshNormalOverlay = DynamicMeshVectorOverlay<float, 3, glm::vec3>;
    /** Standard Color overlay type - 4-element float (rbga) */
    using DynamicMeshColorOverlay = DynamicMeshVectorOverlay<float, 4, glm::vec4>;
    /** Standard per-triangle integer material ID */
    using DynamicMeshMaterialAttribute = DynamicMeshScalarTriangleAttribute<int32_t>;

    /** Per-triangle integer polygroup ID */
    using DynamicMeshPolygroupAttribute = DynamicMeshScalarTriangleAttribute<int32_t>;

    /**
     * DynamicMeshAttributeSet manages a set of extended attributes for a DynamicMesh3.
     * This includes UV and Normal overlays, etc.
     *
     * Currently the default is to always have one UV layer and one Normal layer, but the number of layers can be
     * requested on construction.
     *
     * @todo current internal structure is a work-in-progress
     */
    class DynamicMeshAttributeSet : public DynamicMeshAttributeSetBase
    {
    public:
        DynamicMeshAttributeSet( DynamicMesh3* Mesh );

        DynamicMeshAttributeSet( DynamicMesh3* Mesh, int32_t NumUVLayers, int32_t NumNormalLayers );

        ~DynamicMeshAttributeSet() override;

        void Copy( const DynamicMeshAttributeSet& Copy );

        /** returns true if the attached overlays/attributes are compact */
        [[nodiscard]] bool IsCompact() const;

        /**
         * Performs a CompactCopy of the attached overlays/attributes.
         * Called by the parent mesh CompactCopy function.
         *
         * @param CompactMaps Maps indicating how vertices and triangles were changes in the parent
         * @param Copy The attribute set to be copied
         */
        void CompactCopy( const DynamicMeshCompactMaps& CompactMaps, const DynamicMeshAttributeSet& Copy );

        /**
         * Compacts the attribute set in place
         * Called by the parent mesh CompactInPlace function
         *
         * @param CompactMaps Maps of how the vertices and triangles were compacted in the parent
         */
        void CompactInPlace( const DynamicMeshCompactMaps& CompactMaps );

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
        void EnableMatchingAttributes( const DynamicMeshAttributeSet& ToMatch, bool bClearExisting = true,
                                       bool bDiscardExtraAttributes = false );

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

    private:
        /** @set the parent mesh for this overlay.  Only safe for use during FDynamicMesh move */
        void Reparent( DynamicMesh3* NewParent );

    public:
        /** @return true if the given edge is a seam edge in any overlay */
        [[nodiscard]] virtual bool IsSeamEdge( int EdgeID ) const;

        /** @return true if the given edge is the termination of a seam in any overlay*/
        [[nodiscard]] virtual bool IsSeamEndEdge( int EdgeID ) const;

        /** @return true if the given edge is a seam edge in any overlay, and reports which overlay types */
        virtual bool IsSeamEdge( int EdgeID, bool& bIsUVSeamOut, bool& bIsNormalSeamOut, bool& bIsColorSeamOut,
                                 bool& bIsTangentSeamOut ) const;

        /** @return true if the given vertex is a seam vertex in any overlay */
        [[nodiscard]] virtual bool IsSeamVertex( int VID, bool bBoundaryIsSeam = true ) const;

        /** @return true if the given vertex is a seam intersection vertex in any overlay */
        [[nodiscard]] virtual bool IsSeamIntersectionVertex( int32_t VertexID ) const;

        /** @return true if the given edge is a material ID boundary */
        [[nodiscard]] virtual bool IsMaterialBoundaryEdge( int EdgeID ) const;

        //
        // UV Layers
        //

        /** @return number of UV layers */
        [[nodiscard]] virtual int NumUVLayers() const
        {
            return static_cast<int>( m_UVLayers.size() );
        }

        /** Set number of UV (2-vector float overlay) layers */
        virtual void SetNumUVLayers( int Num );

        /** @return the UV layer at the given Index  if exists, else nullptr */
        DynamicMeshUVOverlay* GetUVLayer( int Index )
        {
            return ( Index >= 0 && Index < NumUVLayers() ) ? m_UVLayers[Index].get() : nullptr;
        }

        /** @return the UV layer at the given Index  if exists, else nullptr */
        [[nodiscard]] const DynamicMeshUVOverlay* GetUVLayer( int Index ) const
        {
            return ( Index >= 0 && Index < NumUVLayers() ) ? m_UVLayers[Index].get() : nullptr;
        }

        /** @return the primary UV layer (layer 0) */
        DynamicMeshUVOverlay* PrimaryUV()
        {
            return GetUVLayer( 0 );
        }
        /** @return the primary UV layer (layer 0) */
        [[nodiscard]] const DynamicMeshUVOverlay* PrimaryUV() const
        {
            return GetUVLayer( 0 );
        }

        //
        // Normal Layers
        //

        /** @return number of Normals layers */
        [[nodiscard]] virtual int NumNormalLayers() const
        {
            return static_cast<int>( m_NormalLayers.size() );
        }

        /** Set number of Normals (3-vector float overlay) layers */
        virtual void SetNumNormalLayers( int Num );

        /** Enable Tangents overlays (Tangent = Normal layer 1, Bitangent = Normal layer 2) */
        void EnableTangents();

        /** Disable Tangents overlays */
        void DisableTangents();

        /** @return the Normal layer at the given Index  if exists, else nullptr */
        DynamicMeshNormalOverlay* GetNormalLayer( int Index )
        {
            return ( Index >= 0 && Index < NumNormalLayers() ) ? m_NormalLayers[Index].get() : nullptr;
        }

        /** @return the Normal layer at the given Index  if exists, else nullptr */
        [[nodiscard]] const DynamicMeshNormalOverlay* GetNormalLayer( int Index ) const
        {
            return ( Index >= 0 && Index < NumNormalLayers() ) ? m_NormalLayers[Index].get() : nullptr;
        }

        /** @return the primary Normal layer (normal layer 0) if it exists */
        DynamicMeshNormalOverlay* PrimaryNormals()
        {
            return GetNormalLayer( 0 );
        }
        /** @return the primary Normal layer (normal layer 0) if it exists */
        [[nodiscard]] const DynamicMeshNormalOverlay* PrimaryNormals() const
        {
            return GetNormalLayer( 0 );
        }
        /** @return the primary tangent layer (normal layer 1) if it exists */
        DynamicMeshNormalOverlay* PrimaryTangents()
        {
            return GetNormalLayer( 1 );
        }
        /** @return the primary tangent layer (normal layer 1) if it exists */
        [[nodiscard]] const DynamicMeshNormalOverlay* PrimaryTangents() const
        {
            return GetNormalLayer( 1 );
        }
        /** @return the primary bitangent layer (normal layer 2) if it exists */
        DynamicMeshNormalOverlay* PrimaryBiTangents()
        {
            return GetNormalLayer( 2 );
        }
        /** @return the primary bitangent layer (normal layer 2) if it exists */
        [[nodiscard]] const DynamicMeshNormalOverlay* PrimaryBiTangents() const
        {
            return GetNormalLayer( 2 );
        }

        /** @return true if normal layers exist for the normal, tangent, and bitangent */
        [[nodiscard]] bool HasTangentSpace() const
        {
            return ( PrimaryNormals() != nullptr && PrimaryTangents() != nullptr &&
                     PrimaryBiTangents() != nullptr );
        }

        [[nodiscard]] bool HasPrimaryColors() const
        {
            return !!m_ColorLayer;
        }

        DynamicMeshColorOverlay* PrimaryColors()
        {
            return m_ColorLayer.get();
        }

        [[nodiscard]] const DynamicMeshColorOverlay* PrimaryColors() const
        {
            return m_ColorLayer.get();
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
        DynamicMeshPolygroupAttribute* GetPolygroupLayer( int Index );

        /** @return the Polygroup layer at the given Index */
        [[nodiscard]] const DynamicMeshPolygroupAttribute* GetPolygroupLayer( int Index ) const;

        //
        // Per-Triangle Material ID
        //

        [[nodiscard]] bool HasMaterialID() const
        {
            return !!m_MaterialIDAttrib;
        }

        void EnableMaterialID();

        void DisableMaterialID();

        DynamicMeshMaterialAttribute* GetMaterialID()
        {
            return m_MaterialIDAttrib.get();
        }

        [[nodiscard]] const DynamicMeshMaterialAttribute* GetMaterialID() const
        {
            return m_MaterialIDAttrib.get();
        }

        //
        // Generic attributes
        //

        void AttachAttribute( const std::string& AttribName, std::unique_ptr<DynamicMeshAttributeBase> Attribute )
        {
            if ( m_GenericAttributes.contains( AttribName ) )
            {
                UnregisterExternalAttribute( m_GenericAttributes[AttribName].get() );
            }
            DynamicMeshAttributeBase* const Registered = Attribute.get();
            m_GenericAttributes[AttribName]            = std::move( Attribute );
            RegisterExternalAttribute( Registered );
        }

        void RemoveAttribute( const std::string& AttribName )
        {
            if ( m_GenericAttributes.contains( AttribName ) )
            {
                UnregisterExternalAttribute( m_GenericAttributes[AttribName].get() );
                m_GenericAttributes.erase( AttribName );
            }
        }

        DynamicMeshAttributeBase* GetAttachedAttribute( const std::string& AttribName )
        {
            return m_GenericAttributes.contains( AttribName ) ? m_GenericAttributes[AttribName].get() : nullptr;
        }

        [[nodiscard]] const DynamicMeshAttributeBase* GetAttachedAttribute( const std::string& AttribName ) const
        {
            const std::unique_ptr<DynamicMeshAttributeBase>* Found = FindValue( m_GenericAttributes, AttribName );
            return ( Found != nullptr ) ? Found->get() : nullptr;
        }

        [[nodiscard]] int NumAttachedAttributes() const
        {
            return static_cast<int32_t>( m_GenericAttributes.size() );
        }

        [[nodiscard]] bool HasAttachedAttribute( const std::string& AttribName ) const
        {
            return m_GenericAttributes.contains( AttribName );
        }

        [[nodiscard]] size_t GetByteCount() const;

    protected:
        /** Parent mesh of this attribute set */
        DynamicMesh3* m_ParentMesh;

        std::vector<std::unique_ptr<DynamicMeshUVOverlay>>     m_UVLayers;
        std::vector<std::unique_ptr<DynamicMeshNormalOverlay>> m_NormalLayers;
        std::unique_ptr<DynamicMeshColorOverlay>               m_ColorLayer;

        std::unique_ptr<DynamicMeshMaterialAttribute> m_MaterialIDAttrib;

        std::vector<std::unique_ptr<DynamicMeshPolygroupAttribute>> m_PolygroupLayers;

        using GenericAttributesMap = std::unordered_map<std::string, std::unique_ptr<DynamicMeshAttributeBase>>;
        GenericAttributesMap m_GenericAttributes;

        friend class DynamicMesh3;

        /**
         * Initialize the existing attribute layers with the given vertex and triangle sizes
         */
        void Initialize( int /*MaxVertexID*/, int MaxTriangleID )
        {
            for ( std::unique_ptr<DynamicMeshUVOverlay>& UVLayer : m_UVLayers )
            {
                UVLayer->InitializeTriangles( MaxTriangleID );
            }
            for ( std::unique_ptr<DynamicMeshNormalOverlay>& NormalLayer : m_NormalLayers )
            {
                NormalLayer->InitializeTriangles( MaxTriangleID );
            }
        }

        // These functions are called by the DynamicMesh3 to update the various
        // attributes when the parent mesh topology has been modified.
        // TODO(danya100kg): would it be better to register all the overlays and attributes with the base set and
        // not overload these?  maybe!
        void OnNewTriangle( int TriangleID, bool bInserted ) override;
        void OnNewVertex( int VertexID, bool bInserted ) override;
        void OnRemoveTriangle( int TriangleID ) override;
        void OnRemoveVertex( int VertexID ) override;
        void OnReverseTriOrientation( int TriangleID ) override;
        void OnSplitEdge( const DynamicMeshInfo::EdgeSplitInfo& splitInfo ) override;
        void OnFlipEdge( const DynamicMeshInfo::EdgeFlipInfo& flipInfo ) override;
        void OnCollapseEdge( const DynamicMeshInfo::EdgeCollapseInfo& collapseInfo ) override;
        void OnPokeTriangle( const DynamicMeshInfo::PokeTriangleInfo& pokeInfo ) override;
        void OnMergeEdges( const DynamicMeshInfo::MergeEdgesInfo& mergeInfo ) override;
        void OnMergeVertices( const DynamicMeshInfo::MergeVerticesInfo& mergeInfo ) override;
        void OnSplitVertex( const DynamicMeshInfo::VertexSplitInfo& SplitInfo,
                            const std::span<const int>&             TrianglesToUpdate ) override;

        /**
         * Check validity of attributes
         *
         * @param bAllowNonmanifold Accept non-manifold topology as valid. Note that this should almost always be
         * true for attributes; non-manifold overlays are generally valid.
         * @param FailMode Desired behavior if mesh is found invalid
         */
        [[nodiscard]] bool CheckValidity( bool bAllowNonmanifold, ValidityCheckFailMode FailMode ) const override;

    private:
        void Append( const DynamicMeshAttributeSet& ToAppend, const DynamicMesh3::AppendInfo& AppendInfo );
        void AppendDefaulted( const DynamicMesh3::AppendInfo& AppendInfo );
    };

} // namespace Desert::Geometry
