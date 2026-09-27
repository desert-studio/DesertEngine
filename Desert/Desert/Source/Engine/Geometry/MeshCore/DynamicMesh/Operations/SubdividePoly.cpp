// Ported from UE 5.8
// Engine/Plugins/Runtime/MeshModelingToolset/Source/ModelingComponentsEditorOnly/Private/Operations/SubdividePoly.cpp:1-1466,
// adapted: see SubdividePoly.hpp.

#include "Engine/Geometry/MeshCore/DynamicMesh/Operations/SubdividePoly.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshNormals.hpp"

#include <algorithm>
#include <format>
#include <unordered_map>
#include <utility>
#include <vector>

// OpenSubdiv's scheme headers use M_PI, which <cmath> only defines on MSVC under _USE_MATH_DEFINES - and a
// precompiled header may have included <cmath> before this file could ask for it (UE works around the same,
// SubdividePoly.cpp:23-27).
#ifndef M_PI
#define M_PI 3.14159265358979323846
#define DESERT_SUBDIVIDE_LOCAL_M_PI 1
#endif
#include <opensubdiv/far/primvarRefiner.h>
#include <opensubdiv/far/topologyDescriptor.h>
#include <opensubdiv/far/topologyRefiner.h>
#include <opensubdiv/far/topologyRefinerFactory.h>
#ifdef DESERT_SUBDIVIDE_LOCAL_M_PI
#undef M_PI
#undef DESERT_SUBDIVIDE_LOCAL_M_PI
#endif

namespace Desert::Geometry
{
    namespace
    {
        namespace Osd = OpenSubdiv::Far;

        // Generic interpolation wrapper for the PrimvarRefiner: positions, UVs, colours, normals. m_bIsValid
        // carries an overlay element's absence through AddWithWeight (UE: TSubdValue, :47-68).
        template <typename T>
        struct SubdValue
        {
            T    m_Value{};
            bool m_bIsValid = true;

            SubdValue() = default;
            explicit SubdValue( const T& value ) : m_Value( value )
            {
            }

            void Clear()
            {
                m_Value    = T{};
                m_bIsValid = true;
            }

            void AddWithWeight( const SubdValue& source, float weight )
            {
                m_Value += static_cast<typename T::value_type>( weight ) * source.m_Value;
                m_bIsValid = m_bIsValid && source.m_bIsValid;
            }
        };

        using SubdVec2f = SubdValue<glm::vec2>;
        using SubdVec3f = SubdValue<glm::vec3>;
        using SubdVec3d = SubdValue<glm::dvec3>;
        using SubdVec4f = SubdValue<glm::vec4>;

        // The GroupTopology corners of one group boundary, in walking order (UE: GetBoundaryCorners, :76-104).
        std::vector<int> GetBoundaryCorners( const GroupTopology::GroupBoundary& boundary,
                                             const GroupTopology&                topology )
        {
            std::vector<int> corners;
            const int        firstEdgeIndex = boundary.GroupEdges[0];
            corners.push_back( topology.m_Edges[firstEdgeIndex].EndpointCorners[0] );
            corners.push_back( topology.m_Edges[firstEdgeIndex].EndpointCorners[1] );

            const Index2i nextEdgeCorners = topology.m_Edges[boundary.GroupEdges[1]].EndpointCorners;
            if ( corners[1] != nextEdgeCorners[0] && corners[1] != nextEdgeCorners[1] )
                std::swap( corners[0], corners[1] );

            for ( size_t i = 1; i + 1 < boundary.GroupEdges.size(); ++i )
            {
                const Index2i currEdgeCorners = topology.m_Edges[boundary.GroupEdges[i]].EndpointCorners;
                corners.push_back( corners.back() == currEdgeCorners[0] ? currEdgeCorners[1]
                                                                        : currEdgeCorners[0] );
            }
            return corners;
        }

        // The most common value of a per-triangle attribute in a group: one material / polygroup per OpenSubdiv
        // face (UE: MostCommonTriangleValueInGroup, :108-127). Ties go to the smallest value, where UE's TMap
        // iteration order decides.
        template <typename AttrType>
        int32_t MostCommonTriangleValueInGroup( const GroupTopology::Group& group, const AttrType& attr )
        {
            std::unordered_map<int32_t, int32_t> votes;
            for ( const int triangleID : group.Triangles )
                ++votes[attr.GetValue( triangleID )];
            int32_t maxVotes = -1;
            int32_t bestVal  = 0;
            for ( const auto& [value, count] : votes )
                if ( count > maxVotes || ( count == maxVotes && value < bestVal ) )
                {
                    maxVotes = count;
                    bestVal  = value;
                }
            return bestVal;
        }

        // A triangle of the group with a corner at vertexID, and that corner's 0-2 index (UE: FindTriangleVertex,
        // :133-151).
        bool FindTriangleVertex( const GroupTopology::Group& group, int vertexID, const DynamicMesh3& mesh,
                                 std::pair<int, int>& outTriangleVertex )
        {
            for ( const int tri : group.Triangles )
            {
                const Index3i triVertices = mesh.GetTriangle( tri );
                for ( int i = 0; i < 3; ++i )
                    if ( triVertices[i] == vertexID )
                    {
                        outTriangleVertex = { tri, i };
                        return true;
                    }
            }
            return false;
        }

        // UE: AddOrFindOverlayElement (:155-186).
        template <bool bBuildingCoordBuffer, typename OverlayType, typename ElementWrapperType>
        int AddOrFindOverlayElement( const OverlayType& overlay, int cornerElementID,
                                     std::unordered_map<int, int>& elementIDToBufferIndex, int& numBufferElems,
                                     std::vector<ElementWrapperType>* outElements )
        {
            if ( const auto existing = elementIDToBufferIndex.find( cornerElementID );
                 existing != elementIDToBufferIndex.end() )
                return existing->second;

            const int newIndex = numBufferElems++;
            if ( cornerElementID != IndexConstants::InvalidID )
                elementIDToBufferIndex.emplace( cornerElementID, newIndex );
            if constexpr ( bBuildingCoordBuffer )
            {
                if ( cornerElementID == IndexConstants::InvalidID )
                {
                    ElementWrapperType invalidElem{};
                    invalidElem.m_bIsValid = false;
                    outElements->push_back( invalidElem );
                }
                else
                    outElements->push_back( ElementWrapperType( overlay.GetElement( cornerElementID ) ) );
            }
            return newIndex;
        }

        // Face-varying overlay data with each group as one polygonal face; assumes overlay seams only run along
        // group boundaries (UE: GetGroupPolyMeshOverlayData, :192-256).
        template <bool bBuildingCoordBuffer, bool bBuildingIndexBuffer, typename OverlayType,
                  typename ElementWrapperType>
        bool GetGroupPolyMeshOverlayData( const GroupTopology& topology, const DynamicMesh3& mesh,
                                          const OverlayType& overlay, std::vector<ElementWrapperType>* outElements,
                                          std::vector<int>* outFaceIndices, int* outNumBufferElems = nullptr )
        {
            std::unordered_map<int, int> elementIDToBufferIndex;
            int                          numBufferElems = 0;
            for ( const GroupTopology::Group& group : topology.m_Groups )
            {
                if ( group.Boundaries.size() != 1 || group.Triangles.empty() )
                    return false;
                for ( const int cornerID : GetBoundaryCorners( group.Boundaries[0], topology ) )
                {
                    const int           cornerVertexID = topology.m_Corners[cornerID].VertexID;
                    std::pair<int, int> triangleVertex;
                    if ( !FindTriangleVertex( group, cornerVertexID, mesh, triangleVertex ) )
                        return false;
                    const int cornerElementID = overlay.GetTriangle( triangleVertex.first )[triangleVertex.second];
                    const int bufferSlot      = AddOrFindOverlayElement<bBuildingCoordBuffer>(
                         overlay, cornerElementID, elementIDToBufferIndex, numBufferElems, outElements );
                    if constexpr ( bBuildingIndexBuffer )
                        outFaceIndices->push_back( bufferSlot );
                }
            }
            if ( outNumBufferElems != nullptr )
                *outNumBufferElems = numBufferElems;
            return true;
        }

        // Face-varying overlay data of the triangles themselves (UE: GetMeshOverlayData, :286-325).
        template <bool bBuildingCoordBuffer, bool bBuildingIndexBuffer, typename OverlayType,
                  typename ElementWrapperType>
        bool GetMeshOverlayData( const DynamicMesh3& mesh, const OverlayType& overlay,
                                 std::vector<ElementWrapperType>* outElements, std::vector<int>* outFaceIndices,
                                 int* outNumBufferElems = nullptr )
        {
            std::unordered_map<int, int> elementIDToBufferIndex;
            int                          numBufferElems = 0;
            for ( const int triangleID : mesh.TriangleIndicesItr() )
            {
                const Index3i elementTri = overlay.GetTriangle( triangleID );
                for ( int i = 0; i < 3; ++i )
                {
                    const int bufferSlot = AddOrFindOverlayElement<bBuildingCoordBuffer>(
                         overlay, elementTri[i], elementIDToBufferIndex, numBufferElems, outElements );
                    if constexpr ( bBuildingIndexBuffer )
                        outFaceIndices->push_back( bufferSlot );
                }
            }
            if ( outNumBufferElems != nullptr )
                *outNumBufferElems = numBufferElems;
            return true;
        }

        // Vertex data through every refinement level (UE: InterpolateVertexData, :342-359).
        template <typename T>
        void InterpolateVertexData( const Osd::PrimvarRefiner& interpolator, const Osd::TopologyRefiner& refiner,
                                    int level, std::vector<T>& sourceData, std::vector<T>& outRefinedData )
        {
            for ( int currentLevel = 1; currentLevel <= level; ++currentLevel )
            {
                outRefinedData.resize( static_cast<size_t>( refiner.GetLevel( currentLevel ).GetNumVertices() ) );
                auto* src = sourceData.data();
                auto* dst = outRefinedData.data();
                interpolator.Interpolate( currentLevel, src, dst );
                sourceData = outRefinedData;
            }
        }

        // Face-varying data through every refinement level (UE: InterpolateFaceVaryingData, :362-380).
        template <typename T>
        void InterpolateFaceVaryingData( const Osd::PrimvarRefiner&  interpolator,
                                         const Osd::TopologyRefiner& refiner, int level, int fvarChannel,
                                         std::vector<T>& sourceData, std::vector<T>& outRefinedData )
        {
            for ( int currentLevel = 1; currentLevel <= level; ++currentLevel )
            {
                outRefinedData.resize(
                     static_cast<size_t>( refiner.GetLevel( currentLevel ).GetNumFVarValues( fvarChannel ) ) );
                auto* src = sourceData.data();
                auto* dst = outRefinedData.data();
                interpolator.InterpolateFaceVarying( currentLevel, src, dst,
                                                     fvarChannel );
                sourceData = outRefinedData;
            }
        }

        // Face-uniform data (material IDs, polygroups): each child face takes its parent's value (UE:
        // InterpolateFaceUniformData, :383-398).
        void InterpolateFaceUniformData( const Osd::PrimvarRefiner&  interpolator,
                                         const Osd::TopologyRefiner& refiner, int level,
                                         std::vector<int32_t>& sourceData, std::vector<int32_t>& outRefinedData )
        {
            for ( int currentLevel = 1; currentLevel <= level; ++currentLevel )
            {
                outRefinedData.resize( static_cast<size_t>( refiner.GetLevel( currentLevel ).GetNumFaces() ) );
                auto* src = sourceData.data();
                auto* dst = outRefinedData.data();
                interpolator.InterpolateFaceUniform( currentLevel, src, dst );
                sourceData = outRefinedData;
            }
        }

        // A face-varying overlay of the (compact) output mesh from the refined elements and per-triangle element
        // indices; a triangle with an invalid corner is left unset (UE: InitializeOverlayFromRefinedData,
        // :401-438).
        template <typename OverlayType, typename ElementType>
        void InitializeOverlayFromRefinedData( OverlayType& overlay, const std::vector<Index3i>& elementTriangles,
                                               const std::vector<ElementType>& elements )
        {
            const DynamicMesh3& mesh = *overlay.GetParentMesh();
            overlay.ClearElements();
            for ( const ElementType& element : elements )
                overlay.AppendElement( element );

            const int numTriangles = mesh.TriangleCount();
            overlay.InitializeTriangles( numTriangles );
            for ( int triangleIndex = 0; triangleIndex < numTriangles; ++triangleIndex )
            {
                const Index3i& elemTri          = elementTriangles[static_cast<size_t>( triangleIndex )];
                const Index3i  meshTriVertices  = mesh.GetTriangle( triangleIndex );
                bool           bAllCornersValid = true;
                for ( int i = 0; i < 3; ++i )
                {
                    if ( elemTri[i] != IndexConstants::InvalidID )
                        overlay.SetParentVertex( elemTri[i], meshTriVertices[i] );
                    else
                        bAllCornersValid = false;
                }
                if ( bAllCornersValid )
                    overlay.SetTriangle( triangleIndex, elemTri );
                else
                    overlay.UnsetTriangle( triangleIndex );
            }
        }

        // Unwraps the refined values and marks an invalid element's corners InvalidID, so the overlay leaves
        // those triangles unset (UE: UnwrapAndApplyValidity, :1248-1266).
        template <typename SubdValueType, typename RawType>
        void UnwrapAndApplyValidity( const std::vector<SubdValueType>& subdArray, std::vector<RawType>& outArray,
                                     std::vector<Index3i>& triangleIndices )
        {
            outArray.reserve( subdArray.size() );
            for ( const SubdValueType& elem : subdArray )
                outArray.push_back( elem.m_Value );
            for ( Index3i& tri : triangleIndices )
                for ( int i = 0; i < 3; ++i )
                    if ( tri[i] != IndexConstants::InvalidID &&
                         !subdArray[static_cast<size_t>( tri[i] )].m_bIsValid )
                        tri[i] = IndexConstants::InvalidID;
        }

    // The channel of each face-varying attribute in the descriptor, -1 if absent (UE: FFVarChannelMapping,
    // :453-465).
    struct FVarChannelMapping
    {
        std::vector<int> UVLayerFVarChannels;
        int              ColorFVarChannel  = -1;
        int              NormalFVarChannel = -1;
    };
    } // namespace

    const char* ToString( SubdivisionScheme scheme )
    {
        switch ( scheme )
        {
            case SubdivisionScheme::Bilinear:
                return "Bilinear";
            case SubdivisionScheme::CatmullClark:
                return "Catmull-Clark";
            case SubdivisionScheme::Loop:
                return "Loop";
        }
        return "?";
    }

    const char* ToString( SubdivisionBoundaryScheme scheme )
    {
        switch ( scheme )
        {
            case SubdivisionBoundaryScheme::SmoothCorners:
                return "Smooth Corners";
            case SubdivisionBoundaryScheme::SharpCorners:
                return "Sharp Corners";
        }
        return "?";
    }

    const char* ToString( SubdivisionOutputNormals normals )
    {
        switch ( normals )
        {
            case SubdivisionOutputNormals::Interpolated:
                return "Interpolated";
            case SubdivisionOutputNormals::Generated:
                return "Generated";
        }
        return "?";
    }

    const char* ToString( SubdividePoly::TopologyCheckResult result )
    {
        // UE's warnings (SubdividePolyTool.cpp:107-140), without the tool's "Loop only" tail.
        switch ( result )
        {
            case SubdividePoly::TopologyCheckResult::Ok:
                return "ok";
            case SubdividePoly::TopologyCheckResult::NoGroups:
                return "the mesh has no polygroups";
            case SubdividePoly::TopologyCheckResult::UnboundedPolygroup:
                return "a polygroup has no boundary";
            case SubdividePoly::TopologyCheckResult::MultiBoundaryPolygroup:
                return "a polygroup has more than one boundary";
            case SubdividePoly::TopologyCheckResult::DegeneratePolygroup:
                return "a polygroup has fewer than three boundary edges";
        }
        return "?";
    }

    class SubdividePoly::RefinerImpl
    {
    public:
        std::unique_ptr<Osd::TopologyRefiner> TopologyRefiner;
        FVarChannelMapping                    FVarMapping;
        // Loop only: mesh vertex ID -> refiner vertex (level 0), and back.
        std::vector<int> VertexToRefiner;
        std::vector<int> RefinerToVertex;
    };

    SubdividePoly::SubdividePoly( const GroupTopology& topology, const DynamicMesh3& originalMesh, int level )
         : m_GroupTopology( topology ), m_OriginalMesh( originalMesh ), m_Level( level ),
           m_Refiner( std::make_unique<RefinerImpl>() )
    {
    }

    SubdividePoly::~SubdividePoly() = default;

    bool SubdividePoly::ComputeTopologySubdivision()
    {
        if ( m_Level < 1 )
        {
            m_Failure = std::format( "the subdivision level must be at least 1, not {}", m_Level );
            return false;
        }

        std::vector<int>                                  boundaryVertsPerFace;
        std::vector<int>                                  numVertsPerFace;
        std::vector<Osd::TopologyDescriptor::FVarChannel> fvarChannels;
        // The descriptor holds raw pointers into these; TopologyRefinerFactory::Create copies them.
        std::vector<std::vector<int>> fvarIndexBuffers;
        FVarChannelMapping&           fvarMapping = m_Refiner->FVarMapping;
        fvarMapping                               = FVarChannelMapping();
        const bool bUseGroupTopology              = m_SubdivisionScheme != SubdivisionScheme::Loop;

        const auto registerFVarChannelForOverlay = [&]( const auto& overlay, const char* name ) -> int
        {
            const int         channelIndex   = static_cast<int>( fvarChannels.size() );
            std::vector<int>& indexBuffer    = fvarIndexBuffers.emplace_back();
            int               numBufferElems = 0;
            const bool        bGetIndexOK =
                 bUseGroupTopology
                             ? GetGroupPolyMeshOverlayData<false, true, std::decay_t<decltype( overlay )>, int>(
                             m_GroupTopology, m_OriginalMesh, overlay, nullptr, &indexBuffer, &numBufferElems )
                             : GetMeshOverlayData<false, true, std::decay_t<decltype( overlay )>, int>(
                             m_OriginalMesh, overlay, nullptr, &indexBuffer, &numBufferElems );
            if ( !bGetIndexOK )
            {
                m_Failure = std::format( "the {} overlay could not be read per polygroup corner", name );
                return -1;
            }
            Osd::TopologyDescriptor::FVarChannel channel;
            channel.numValues    = numBufferElems;
            channel.valueIndices = nullptr; // set below, once fvarIndexBuffers stops growing
            fvarChannels.push_back( channel );
            return channelIndex;
        };

        const auto registerAllFVarChannels = [&]( Osd::TopologyDescriptor& descriptor ) -> bool
        {
            const DynamicMeshAttributeSet* attributes = m_OriginalMesh.Attributes();
            if ( attributes != nullptr )
            {
                const int numUVLayers = attributes->NumUVLayers();
                fvarMapping.UVLayerFVarChannels.assign( static_cast<size_t>( numUVLayers ), -1 );
                for ( int layerIdx = 0; layerIdx < numUVLayers; ++layerIdx )
                {
                    const DynamicMeshUVOverlay* uvLayer = attributes->GetUVLayer( layerIdx );
                    if ( uvLayer != nullptr && uvLayer->ElementCount() > 0 )
                    {
                        const int channel = registerFVarChannelForOverlay( *uvLayer, "UV" );
                        if ( channel < 0 )
                            return false;
                        fvarMapping.UVLayerFVarChannels[static_cast<size_t>( layerIdx )] = channel;
                    }
                }
                if ( attributes->HasPrimaryColors() && attributes->PrimaryColors()->ElementCount() > 0 )
                {
                    const int channel = registerFVarChannelForOverlay( *attributes->PrimaryColors(), "colour" );
                    if ( channel < 0 )
                        return false;
                    fvarMapping.ColorFVarChannel = channel;
                }
                if ( m_NormalComputationMethod == SubdivisionOutputNormals::Interpolated &&
                     attributes->NumNormalLayers() > 0 && attributes->PrimaryNormals()->ElementCount() > 0 )
                {
                    const int channel = registerFVarChannelForOverlay( *attributes->PrimaryNormals(), "normal" );
                    if ( channel < 0 )
                        return false;
                    fvarMapping.NormalFVarChannel = channel;
                }
            }
            for ( size_t i = 0; i < fvarChannels.size(); ++i )
                fvarChannels[i].valueIndices = fvarIndexBuffers[i].data();
            descriptor.numFVarChannels = static_cast<int>( fvarChannels.size() );
            descriptor.fvarChannels    = fvarChannels.empty() ? nullptr : fvarChannels.data();
            return true;
        };

        Osd::TopologyDescriptor descriptor;
        if ( m_SubdivisionScheme == SubdivisionScheme::Loop )
        {
            // UE: DescriptorFromTriangleMesh (:610-628), with the vertices numbered densely.
            m_Refiner->VertexToRefiner.assign( static_cast<size_t>( m_OriginalMesh.MaxVertexID() ),
                                               IndexConstants::InvalidID );
            m_Refiner->RefinerToVertex.clear();
            for ( const int vertexID : m_OriginalMesh.VertexIndicesItr() )
            {
                m_Refiner->VertexToRefiner[static_cast<size_t>( vertexID )] =
                     static_cast<int>( m_Refiner->RefinerToVertex.size() );
                m_Refiner->RefinerToVertex.push_back( vertexID );
            }
            for ( const int triangleID : m_OriginalMesh.TriangleIndicesItr() )
            {
                const Index3i triangleVertices = m_OriginalMesh.GetTriangle( triangleID );
                numVertsPerFace.push_back( 3 );
                for ( int i = 0; i < 3; ++i )
                    boundaryVertsPerFace.push_back(
                         m_Refiner->VertexToRefiner[static_cast<size_t>( triangleVertices[i] )] );
            }
            descriptor.numVertices = static_cast<int>( m_Refiner->RefinerToVertex.size() );
            descriptor.numFaces    = m_OriginalMesh.TriangleCount();
        }
        else
        {
            // UE: DescriptorFromGroupTopology (:630-656).
            for ( const GroupTopology::Group& group : m_GroupTopology.m_Groups )
            {
                if ( group.Boundaries.size() != 1 || group.Boundaries[0].GroupEdges.size() < 2 )
                {
                    m_Failure = std::format( "polygroup {} has {} boundaries ({} group edges on the first); the "
                                             "{} cage needs one boundary of at least two group edges per group",
                                             group.GroupID, group.Boundaries.size(),
                                             group.Boundaries.empty() ? 0 : group.Boundaries[0].GroupEdges.size(),
                                             ToString( m_SubdivisionScheme ) );
                    return false;
                }
                const std::vector<int> corners = GetBoundaryCorners( group.Boundaries[0], m_GroupTopology );
                numVertsPerFace.push_back( static_cast<int>( corners.size() ) );
                boundaryVertsPerFace.insert( boundaryVertsPerFace.end(), corners.begin(), corners.end() );
            }
            descriptor.numVertices = static_cast<int>( m_GroupTopology.m_Corners.size() );
            descriptor.numFaces    = static_cast<int>( m_GroupTopology.m_Groups.size() );
        }
        descriptor.numVertsPerFace    = numVertsPerFace.data();
        descriptor.vertIndicesPerFace = boundaryVertsPerFace.data();
        if ( !registerAllFVarChannels( descriptor ) )
            return false;

        using RefinerFactory = Osd::TopologyRefinerFactory<Osd::TopologyDescriptor>;
        RefinerFactory::Options refinerOptions;
        refinerOptions.schemeOptions.SetVtxBoundaryInterpolation(
             m_BoundaryScheme == SubdivisionBoundaryScheme::SharpCorners
                  ? OpenSubdiv::Sdc::Options::VTX_BOUNDARY_EDGE_AND_CORNER
                  : OpenSubdiv::Sdc::Options::VTX_BOUNDARY_EDGE_ONLY );
        switch ( m_SubdivisionScheme )
        {
            case SubdivisionScheme::Bilinear:
                refinerOptions.schemeType = OpenSubdiv::Sdc::SCHEME_BILINEAR;
                break;
            case SubdivisionScheme::CatmullClark:
                refinerOptions.schemeType = OpenSubdiv::Sdc::SCHEME_CATMARK;
                break;
            case SubdivisionScheme::Loop:
                refinerOptions.schemeType = OpenSubdiv::Sdc::SCHEME_LOOP;
                break;
        }

        m_Refiner->TopologyRefiner.reset( RefinerFactory::Create( descriptor, refinerOptions ) );
        if ( m_Refiner->TopologyRefiner == nullptr )
        {
            m_Failure =
                 std::format( "OpenSubdiv rejected the {} cage of {} vertices and {} faces",
                              ToString( m_SubdivisionScheme ), descriptor.numVertices, descriptor.numFaces );
            return false;
        }
        m_Refiner->TopologyRefiner->RefineUniform( Osd::TopologyRefiner::UniformOptions( m_Level ) );
        return true;
    }

    bool SubdividePoly::ComputeSubdividedMesh( DynamicMesh3& outMesh )
    {
        if ( m_Refiner->TopologyRefiner == nullptr )
        {
            m_Failure = "ComputeSubdividedMesh before a successful ComputeTopologySubdivision";
            return false;
        }
        const Osd::TopologyRefiner& refiner = *m_Refiner->TopologyRefiner;
        const Osd::PrimvarRefiner   interpolator( refiner );
        const bool                  bLoop = m_SubdivisionScheme == SubdivisionScheme::Loop;

        const auto forEachSourceVertex = [&]( auto&& fn )
        {
            if ( bLoop )
                for ( const int vertexID : m_Refiner->RefinerToVertex )
                    fn( vertexID );
            else
                for ( const GroupTopology::Corner& corner : m_GroupTopology.m_Corners )
                    fn( corner.VertexID );
        };

        const auto collectFaceUniformAttributeSource = [&]( const auto& attr, std::vector<int32_t>& outSource )
        {
            if ( bLoop )
                for ( const int triangleID : m_OriginalMesh.TriangleIndicesItr() )
                    outSource.push_back( attr.GetValue( triangleID ) );
            else
                for ( const GroupTopology::Group& group : m_GroupTopology.m_Groups )
                    outSource.push_back( MostCommonTriangleValueInGroup( group, attr ) );
        };

        const auto extractAndInterpolateFVarOverlay = [&]( int fvarChannel, const auto& overlay, auto& outRefined )
        {
            using SubdType = typename std::decay_t<decltype( outRefined )>::value_type;
            using Overlay  = std::decay_t<decltype( overlay )>;
            std::vector<SubdType> source;
            const bool bOK = bLoop ? GetMeshOverlayData<true, false, Overlay, SubdType>( m_OriginalMesh, overlay,
                                                                                         &source, nullptr )
                                   : GetGroupPolyMeshOverlayData<true, false, Overlay, SubdType>(
                                          m_GroupTopology, m_OriginalMesh, overlay, &source, nullptr );
            if ( bOK )
                InterpolateFaceVaryingData( interpolator, refiner, m_Level, fvarChannel, source, outRefined );
            return bOK;
        };

        // Positions.
        std::vector<SubdVec3d> sourcePositions;
        forEachSourceVertex( [&]( int vertexID )
                             { sourcePositions.emplace_back( m_OriginalMesh.GetVertex( vertexID ) ); } );
        std::vector<SubdVec3d> refinedPositions;
        InterpolateVertexData( interpolator, refiner, m_Level, sourcePositions, refinedPositions );

        // Face group IDs.
        std::vector<int32_t> sourceGroupIDs;
        if ( bLoop )
            for ( const int triangleID : m_OriginalMesh.TriangleIndicesItr() )
                sourceGroupIDs.push_back( m_OriginalMesh.GetTriangleGroup( triangleID ) );
        else
            for ( const GroupTopology::Group& group : m_GroupTopology.m_Groups )
                sourceGroupIDs.push_back( group.GroupID );
        std::vector<int32_t> refinedGroupIDs;
        if ( !m_bNewPolyGroups )
            InterpolateFaceUniformData( interpolator, refiner, m_Level, sourceGroupIDs, refinedGroupIDs );

        // Material IDs.
        const DynamicMeshAttributeSet* attributes      = m_OriginalMesh.Attributes();
        const bool                     bHasMaterialIDs = attributes != nullptr && attributes->HasMaterialID();
        std::vector<int32_t>           refinedMaterialIDs;
        if ( bHasMaterialIDs )
        {
            std::vector<int32_t> sourceMaterialIDs;
            collectFaceUniformAttributeSource( *attributes->GetMaterialID(), sourceMaterialIDs );
            InterpolateFaceUniformData( interpolator, refiner, m_Level, sourceMaterialIDs, refinedMaterialIDs );
        }

        // UV layers.
        const FVarChannelMapping& fvarMapping = m_Refiner->FVarMapping;
        const int                 numUVLayers =
             attributes != nullptr ? static_cast<int>( fvarMapping.UVLayerFVarChannels.size() ) : 0;
        std::vector<std::vector<SubdVec2f>> allRefinedUVs( static_cast<size_t>( numUVLayers ) );
        for ( int layerIdx = 0; layerIdx < numUVLayers; ++layerIdx )
        {
            const int fvarChannel = fvarMapping.UVLayerFVarChannels[static_cast<size_t>( layerIdx )];
            if ( fvarChannel < 0 )
                continue;
            if ( !extractAndInterpolateFVarOverlay( fvarChannel, *attributes->GetUVLayer( layerIdx ),
                                                    allRefinedUVs[static_cast<size_t>( layerIdx )] ) )
            {
                m_Failure = std::format( "UV layer {} could not be read per polygroup corner", layerIdx );
                return false;
            }
        }

        // Colours.
        const bool bShouldInterpolateColors = attributes != nullptr && fvarMapping.ColorFVarChannel >= 0;
        std::vector<SubdVec4f> refinedColors;
        if ( bShouldInterpolateColors &&
             !extractAndInterpolateFVarOverlay( fvarMapping.ColorFVarChannel, *attributes->PrimaryColors(),
                                                refinedColors ) )
        {
            m_Failure = "the colour overlay could not be read per polygroup corner";
            return false;
        }

        // Normals (Interpolated).
        const bool bShouldInterpolateNormals = attributes != nullptr && fvarMapping.NormalFVarChannel >= 0;
        std::vector<SubdVec3f> refinedNormals;
        if ( bShouldInterpolateNormals &&
             !extractAndInterpolateFVarOverlay( fvarMapping.NormalFVarChannel, *attributes->PrimaryNormals(),
                                                refinedNormals ) )
        {
            m_Failure = "the normal overlay could not be read per polygroup corner";
            return false;
        }

        // Additional polygroup layers (face-uniform).
        std::vector<std::vector<int32_t>> refinedPolygroupLayers;
        if ( attributes != nullptr )
            for ( int layerIdx = 0; layerIdx < attributes->NumPolygroupLayers(); ++layerIdx )
            {
                std::vector<int32_t> sourcePGIDs;
                collectFaceUniformAttributeSource( *attributes->GetPolygroupLayer( layerIdx ), sourcePGIDs );
                InterpolateFaceUniformData( interpolator, refiner, m_Level, sourcePGIDs,
                                            refinedPolygroupLayers.emplace_back() );
            }

        // Transfer to the output mesh (UE :1100-1296).
        outMesh.Clear();
        outMesh.EnableTriangleGroups();
        outMesh.EnableAttributes();
        for ( const SubdVec3d& v : refinedPositions )
            outMesh.AppendVertex( v.m_Value );

        const auto& finalLevel = refiner.GetLevel( m_Level );

        // One (channel, triangles) collector per face-varying attribute, filled in one pass over the faces.
        struct FVarTriangleCollector
        {
            int                  FVarChannel = -1;
            std::vector<Index3i> Triangles;
        };
        std::vector<FVarTriangleCollector> fvarCollectors;
        std::vector<int>                   uvLayerToCollector( static_cast<size_t>( numUVLayers ), -1 );
        for ( int layerIdx = 0; layerIdx < numUVLayers; ++layerIdx )
            if ( const int ch = fvarMapping.UVLayerFVarChannels[static_cast<size_t>( layerIdx )]; ch >= 0 )
            {
                uvLayerToCollector[static_cast<size_t>( layerIdx )] = static_cast<int>( fvarCollectors.size() );
                fvarCollectors.push_back( { ch, {} } );
            }
        int colorCollectorIdx = -1;
        if ( bShouldInterpolateColors )
        {
            colorCollectorIdx = static_cast<int>( fvarCollectors.size() );
            fvarCollectors.push_back( { fvarMapping.ColorFVarChannel, {} } );
        }
        int normalCollectorIdx = -1;
        if ( bShouldInterpolateNormals )
        {
            normalCollectorIdx = static_cast<int>( fvarCollectors.size() );
            fvarCollectors.push_back( { fvarMapping.NormalFVarChannel, {} } );
        }

        if ( bHasMaterialIDs )
            outMesh.Attributes()->EnableMaterialID();
        const int numPolygroupLayerAttrs = static_cast<int>( refinedPolygroupLayers.size() );
        if ( numPolygroupLayerAttrs > 0 )
            outMesh.Attributes()->SetNumPolygroupLayers( numPolygroupLayerAttrs );

        for ( int faceID = 0; faceID < finalLevel.GetNumFaces(); ++faceID )
        {
            const int                              groupID = m_bNewPolyGroups ? outMesh.AllocateTriangleGroup()
                                                                              : refinedGroupIDs[static_cast<size_t>( faceID )];
            const OpenSubdiv::Far::ConstIndexArray face    = finalLevel.GetFaceVertices( faceID );
            if ( face.size() != 3 && face.size() != 4 )
            {
                m_Failure = std::format( "refined face {} has {} vertices, 3 or 4 expected", faceID, face.size() );
                return false;
            }
            const int triA = outMesh.AppendTriangle( Index3i( face[0], face[1], face[2] ), groupID );
            const int triB =
                 face.size() == 4 ? outMesh.AppendTriangle( Index3i( face[0], face[2], face[3] ), groupID ) : -1;
            if ( triA < 0 && triB < 0 )
                continue;

            for ( FVarTriangleCollector& collector : fvarCollectors )
            {
                const OpenSubdiv::Far::ConstIndexArray fvar =
                     finalLevel.GetFaceFVarValues( faceID, collector.FVarChannel );
                if ( triA >= 0 )
                    collector.Triangles.emplace_back( fvar[0], fvar[1], fvar[2] );
                if ( triB >= 0 )
                    collector.Triangles.emplace_back( fvar[0], fvar[2], fvar[3] );
            }
            for ( const int tri : { triA, triB } )
            {
                if ( tri < 0 )
                    continue;
                if ( bHasMaterialIDs )
                    outMesh.Attributes()->GetMaterialID()->SetValue(
                         tri, refinedMaterialIDs[static_cast<size_t>( faceID )] );
                for ( int layerIdx = 0; layerIdx < numPolygroupLayerAttrs; ++layerIdx )
                    outMesh.Attributes()
                         ->GetPolygroupLayer( layerIdx )
                         ->SetValue( tri, refinedPolygroupLayers[static_cast<size_t>( layerIdx )]
                                                                [static_cast<size_t>( faceID )] );
            }
        }

        // UE checks the output is compact (InitializeOverlayFromRefinedData :408-411); a refused triangle above
        // would leave a gap between triangle IDs and the collectors' order.
        if ( outMesh.TriangleCount() != outMesh.MaxTriangleID() )
        {
            m_Failure = std::format( "{} of the refined triangles were refused by the mesh",
                                     outMesh.MaxTriangleID() - outMesh.TriangleCount() );
            return false;
        }

        DynamicMeshAttributeSet& outAttributes = *outMesh.Attributes();
        outAttributes.SetNumUVLayers( std::max( numUVLayers, 1 ) );
        for ( int layerIdx = 0; layerIdx < numUVLayers; ++layerIdx )
        {
            const int collectorIdx = uvLayerToCollector[static_cast<size_t>( layerIdx )];
            if ( collectorIdx < 0 || allRefinedUVs[static_cast<size_t>( layerIdx )].empty() )
                continue;
            std::vector<glm::vec2> uvElements;
            auto&                  triangles = fvarCollectors[static_cast<size_t>( collectorIdx )].Triangles;
            UnwrapAndApplyValidity( allRefinedUVs[static_cast<size_t>( layerIdx )], uvElements, triangles );
            InitializeOverlayFromRefinedData( *outAttributes.GetUVLayer( layerIdx ), triangles, uvElements );
        }
        if ( colorCollectorIdx >= 0 )
        {
            outAttributes.EnablePrimaryColors();
            std::vector<glm::vec4> colorElements;
            auto&                  triangles = fvarCollectors[static_cast<size_t>( colorCollectorIdx )].Triangles;
            UnwrapAndApplyValidity( refinedColors, colorElements, triangles );
            InitializeOverlayFromRefinedData( *outAttributes.PrimaryColors(), triangles, colorElements );
        }
        if ( normalCollectorIdx >= 0 )
        {
            std::vector<glm::vec3> normalElements;
            auto&                  triangles = fvarCollectors[static_cast<size_t>( normalCollectorIdx )].Triangles;
            UnwrapAndApplyValidity( refinedNormals, normalElements, triangles );
            InitializeOverlayFromRefinedData( *outAttributes.PrimaryNormals(), triangles, normalElements );
        }
        else if ( m_NormalComputationMethod == SubdivisionOutputNormals::Generated )
            MeshNormals::InitializeOverlayToPerVertexNormals( outAttributes.PrimaryNormals(), false );
        else
        {
            // UE leaves the normals unset here (:1296-1303); ToRenderMesh would refuse every triangle, so the
            // cause is named instead.
            m_Failure = "Interpolated normals requested, but the mesh has no normal overlay to interpolate";
            return false;
        }

        // Vertices no face references (the Loop cage's isolated vertices).
        outMesh.RemoveUnusedVertices();
        return true;
    }

    SubdividePoly::TopologyCheckResult SubdividePoly::ValidateTopology() const
    {
        // UE: ValidateTopology (:1429-1464).
        if ( m_GroupTopology.m_Groups.empty() )
            return TopologyCheckResult::NoGroups;
        for ( const GroupTopology::Group& group : m_GroupTopology.m_Groups )
        {
            if ( group.Boundaries.empty() )
                return TopologyCheckResult::UnboundedPolygroup;
            if ( group.Boundaries.size() > 1 )
                return TopologyCheckResult::MultiBoundaryPolygroup;
            if ( group.Boundaries[0].GroupEdges.size() < 3 )
                return TopologyCheckResult::DegeneratePolygroup;
        }
        return TopologyCheckResult::Ok;
    }
} // namespace Desert::Geometry
