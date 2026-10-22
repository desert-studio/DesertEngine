// The UVs palette's operations. Ported structure, by tool:
//   AutoUV  - UE 5.8 DynamicMesh/Private/Parameterization/PatchBasedMeshUVGenerator.cpp:24-121 (AutoComputeUVs:
//             patches -> islands -> per-island solve -> AutoOrientUVArea -> QuickPack) and :623-660
//             (ComputeUVsFromTriangleSets: ExpMap per island); patch building as AutoUVSettings describes.
//   Project - UE MeshModelingToolset/Operators/Private/UVProjectionOp.cpp (CalculateResult_Plane/_Box/_Cylinder,
//             then the UV transform).
//   Unwrap  - UE ModelingOperators/Private/ParameterizationOps/RecomputeUVsOp.cpp (islands by polygroup or by
//             existing UVs, ExpMap / spectral conformal per island, NormalizeAreas, AutoRotation, Repack).
//   Layout  - UE ModelingOperators/Private/ParameterizationOps/UVLayoutOp.cpp (Repack through the
//             FDynamicMeshUVPacker, then UVScaleFactor and UVTranslation; Transform only applies those two).
#include "Engine/Geometry/DynamicMeshUVOperations.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/Parameterization/DynamicMeshUVEditor.hpp"
#include "Engine/Geometry/MeshCore/Parameterization/UVPacking.hpp"
#include "Engine/Geometry/MeshCore/Selections/MeshConnectedComponents.hpp"
#include "Engine/Geometry/MeshRegionOperation.hpp"

#include <spdlog/fmt/fmt.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <numbers>
#include <vector>

namespace Desert::Geometry
{
    namespace
    {
        DynamicMeshUVOverlay& PrepareUVLayer( DynamicMesh3& mesh )
        {
            if ( !mesh.HasAttributes() )
                mesh.EnableAttributes();
            if ( mesh.Attributes()->NumUVLayers() < 1 )
                mesh.Attributes()->SetNumUVLayers( 1 );
            return *mesh.Attributes()->PrimaryUV();
        }

        std::vector<int32_t> AllTriangles( const DynamicMesh3& mesh )
        {
            std::vector<int32_t> out;
            out.reserve( static_cast<size_t>( mesh.TriangleCount() ) );
            for ( const int tid : mesh.TriangleIndicesItr() )
                out.push_back( tid );
            return out;
        }

        // The UV islands: triangles with UVs, connected through shared UV elements.
        std::vector<std::vector<int32_t>> UVIslands( const DynamicMesh3& mesh, const DynamicMeshUVOverlay& uvs )
        {
            std::vector<int> set;
            for ( const int tid : mesh.TriangleIndicesItr() )
                if ( uvs.IsSetTriangle( tid ) )
                    set.push_back( tid );
            MeshConnectedComponents components( &mesh );
            components.FindConnectedTriangles( set, [&uvs]( int32_t a, int32_t b )
                                               { return uvs.AreTrianglesConnected( a, b ); } );
            std::vector<std::vector<int32_t>> out;
            for ( int32_t i = 0; i < components.Num(); ++i )
                out.emplace_back( components[i].Indices.begin(), components[i].Indices.end() );
            return out;
        }

        Common::ResultStr<UVOutcome> Finish( DynamicMesh3&& mesh, const char* name, std::string report )
        {
            if ( auto tangents = RecomputeTangentSpace( mesh, name ); !tangents.IsSuccess() )
                return Common::MakeError<UVOutcome>( tangents.GetError() );
            UVOutcome out;
            out.Islands = static_cast<int32_t>( UVIslands( mesh, *mesh.Attributes()->PrimaryUV() ).size() );
            out.Report  = fmt::format( "{} UV islands; {}", out.Islands, report );
            out.Mesh    = std::move( mesh );
            return Common::MakeSuccess( std::move( out ) );
        }

        glm::dvec3 AverageNormal( const DynamicMesh3& mesh, const std::vector<int32_t>& triangles )
        {
            glm::dvec3 sum( 0.0 );
            for ( const int32_t tid : triangles )
                sum += mesh.GetTriNormal( tid ) * mesh.GetTriArea( tid );
            const double len = glm::length( sum );
            return len > 1e-12 ? sum / len : glm::dvec3( 0.0, 1.0, 0.0 );
        }

        // A frame whose Z is `normal`, at the triangles' centroid: the fallback plane of an island that has no
        // boundary to flatten.
        Frame3d PlaneFrameFor( const DynamicMesh3& mesh, const std::vector<int32_t>& triangles )
        {
            const glm::dvec3 n = AverageNormal( mesh, triangles );
            glm::dvec3       c( 0.0 );
            for ( const int32_t tid : triangles )
            {
                glm::dvec3 a, b, d;
                mesh.GetTriVertices( tid, a, b, d );
                c += ( a + b + d ) / 3.0;
            }
            c /= static_cast<double>( std::max<size_t>( 1, triangles.size() ) );
            const glm::dvec3 helper = std::abs( n.y ) < 0.9 ? glm::dvec3( 0, 1, 0 ) : glm::dvec3( 1, 0, 0 );
            const glm::dvec3 x      = glm::normalize( glm::cross( helper, n ) );
            const glm::dvec3 y      = glm::cross( n, x );
            return Frame3d( c, x, y, n );
        }

        // One island flattened by `method`; false when the solver refused (no boundary, or a failed solve).
        bool SolveIsland( DynamicMeshUVEditor& editor, const std::vector<int32_t>& island, UVUnwrapMethod method )
        {
            if ( method == UVUnwrapMethod::ExpMap )
                return editor.SetTriangleUVsFromExpMap( island );
            return editor.SetTriangleUVsFromFreeBoundarySpectralConformal( island, false, false );
        }

        // Per island: solve, or project onto its own plane when it cannot be solved; returns how many were
        // projected.
        int32_t SolveIslands( DynamicMesh3& mesh, DynamicMeshUVEditor& editor,
                              const std::vector<std::vector<int32_t>>& islands, UVUnwrapMethod method )
        {
            int32_t projected = 0;
            for ( const std::vector<int32_t>& island : islands )
            {
                if ( SolveIsland( editor, island, method ) )
                    continue;
                editor.SetTriangleUVsFromPlanarProjection(
                     island, []( const glm::dvec3& p ) { return p; }, PlaneFrameFor( mesh, island ),
                     glm::dvec2( 1.0 ) );
                ++projected;
            }
            return projected;
        }

        void ApplyUVTransform( DynamicMeshUVOverlay& uvs, float rotationDeg, const glm::vec2& scale,
                               const glm::vec2& translate )
        {
            const float c = std::cos( glm::radians( rotationDeg ) );
            const float s = std::sin( glm::radians( rotationDeg ) );
            for ( const int eid : uvs.ElementIndicesItr() )
            {
                const glm::vec2 uv = uvs.GetElement( eid );
                const glm::vec2 r( c * uv.x - s * uv.y, s * uv.x + c * uv.y );
                uvs.SetElement( eid, r * scale + translate );
            }
        }
    } // namespace

    const char* ToString( UVProjectionType type )
    {
        switch ( type )
        {
            case UVProjectionType::Plane:
                return "Plane";
            case UVProjectionType::Box:
                return "Box";
            case UVProjectionType::Cylinder:
                return "Cylinder";
        }
        return "?";
    }

    const char* ToString( UVIslandSource source )
    {
        switch ( source )
        {
            case UVIslandSource::PolyGroups:
                return "PolyGroups";
            case UVIslandSource::ExistingUVs:
                return "Existing UVs";
        }
        return "?";
    }

    const char* ToString( UVUnwrapMethod method )
    {
        switch ( method )
        {
            case UVUnwrapMethod::ExpMap:
                return "ExpMap";
            case UVUnwrapMethod::SpectralConformal:
                return "Spectral Conformal";
        }
        return "?";
    }

    const char* ToString( UVLayoutType type )
    {
        switch ( type )
        {
            case UVLayoutType::Repack:
                return "Repack";
            case UVLayoutType::Transform:
                return "Transform";
        }
        return "?";
    }

    Common::ResultStr<UVOutcome> AutoUV( const DynamicMesh3& mesh, const AutoUVSettings& settings )
    {
        if ( mesh.TriangleCount() == 0 )
            return Common::MakeError<UVOutcome>( "AutoUV: the mesh has no triangle" );
        DynamicMesh3          out = mesh;
        DynamicMeshUVOverlay& uvs = PrepareUVLayer( out );
        DynamicMeshUVEditor   editor( &out, &uvs );
        editor.ResetUVs( AllTriangles( out ) );

        // Step 1-2: patches, grown from each unassigned triangle while a neighbour's normal stays within the
        // deviation bound of the patch's area-weighted normal (and, with RespectPolygroups, in its polygroup).
        const double cosBound = std::cos( glm::radians( static_cast<double>( settings.MaxNormalDeviationDeg ) ) );
        const bool   groups   = settings.RespectPolygroups && out.HasTriangleGroups();
        std::vector<uint8_t>              assigned( static_cast<size_t>( out.MaxTriangleID() ), 0 );
        std::vector<std::vector<int32_t>> patches;
        for ( const int seed : out.TriangleIndicesItr() )
        {
            if ( assigned[seed] )
                continue;
            std::vector<int32_t> patch;
            glm::dvec3           normalSum = out.GetTriNormal( seed ) * out.GetTriArea( seed );
            std::deque<int32_t>  queue{ seed };
            assigned[seed] = 1;
            while ( !queue.empty() )
            {
                const int32_t tid = queue.front();
                queue.pop_front();
                patch.push_back( tid );
                const Index3i nbrs = out.GetTriNeighbourTris( tid );
                for ( int j = 0; j < 3; ++j )
                {
                    const int32_t nbr = nbrs[j];
                    if ( nbr < 0 || assigned[nbr] )
                        continue;
                    if ( groups && out.GetTriangleGroup( nbr ) != out.GetTriangleGroup( seed ) )
                        continue;
                    const glm::dvec3 patchNormal = glm::normalize( normalSum );
                    if ( glm::dot( out.GetTriNormal( nbr ), patchNormal ) < cosBound )
                        continue;
                    assigned[nbr] = 1;
                    normalSum += out.GetTriNormal( nbr ) * out.GetTriArea( nbr );
                    queue.push_back( nbr );
                }
            }
            patches.push_back( std::move( patch ) );
        }

        // Step 3: UVs per island (UE ComputeUVsFromTriangleSets: ExpMap)
        const int32_t projected = SolveIslands( out, editor, patches, UVUnwrapMethod::ExpMap );
        for ( const std::vector<int32_t>& patch : patches )
            editor.ScaleUVAreaTo3DArea( patch, false );

        // Step 4: orientation, step 5: packing
        if ( settings.AutoAlign )
            for ( const std::vector<int32_t>& patch : patches )
                editor.AutoOrientUVArea( patch );
        if ( settings.AutoPack && !editor.QuickPack( settings.PackingResolution ) )
            return Common::MakeFormattedError<UVOutcome>( "AutoUV: {} patches do not pack into {} texels",
                                                          patches.size(), settings.PackingResolution );

        return Finish( std::move( out ), "AutoUV",
                       fmt::format( "{} patches within {} deg{}", patches.size(), settings.MaxNormalDeviationDeg,
                                    projected > 0 ? fmt::format( ", {} closed patches projected flat", projected )
                                                  : std::string() ) );
    }

    namespace
    {
        // The volume's axes before RotationDeg: Z along world +Y (up), X along +X, Y along -Z
        // (right-handed).
        const glm::dmat3 kProjectionBaseAxes( glm::dvec3( 1.0, 0.0, 0.0 ), glm::dvec3( 0.0, 0.0, -1.0 ),
                                              glm::dvec3( 0.0, 1.0, 0.0 ) );
    } // namespace

    Common::ResultStr<UVProjectionVolume> ResolveUVProjectionVolume( const DynamicMesh3&      mesh,
                                                                     const glm::dmat4&        meshToWorld,
                                                                     const UVProjectSettings& settings )
    {
        if ( mesh.VertexCount() == 0 )
            return Common::MakeError<UVProjectionVolume>( "UV Project: the mesh has no vertex" );
        const auto toWorld = [&]( int vid )
        { return glm::dvec3( meshToWorld * glm::dvec4( mesh.GetVertex( vid ), 1.0 ) ); };
        glm::dvec3 lo( std::numeric_limits<double>::max() ), hi( -std::numeric_limits<double>::max() );
        for ( const int vid : mesh.VertexIndicesItr() )
        {
            lo = glm::min( lo, toWorld( vid ) );
            hi = glm::max( hi, toWorld( vid ) );
        }
        UVProjectionVolume volume;
        const glm::dquat   turn( glm::radians( glm::dvec3( settings.RotationDeg ) ) );
        volume.AxisX  = turn * kProjectionBaseAxes[0];
        volume.AxisY  = turn * kProjectionBaseAxes[1];
        volume.AxisZ  = turn * kProjectionBaseAxes[2];
        volume.Origin = ( lo + hi ) * 0.5 + glm::dvec3( settings.CenterOffset );

        // Fitted dimensions: the bounds' extent along each volume axis.
        glm::dvec3 dims = glm::dvec3( settings.Dimensions );
        if ( settings.FitToBounds )
        {
            glm::dvec3 flo( std::numeric_limits<double>::max() ), fhi( -std::numeric_limits<double>::max() );
            for ( const int vid : mesh.VertexIndicesItr() )
            {
                const glm::dvec3 d = toWorld( vid ) - volume.Origin;
                const glm::dvec3 p( glm::dot( d, volume.AxisX ), glm::dot( d, volume.AxisY ),
                                    glm::dot( d, volume.AxisZ ) );
                flo = glm::min( flo, p );
                fhi = glm::max( fhi, p );
            }
            dims = fhi - flo;
        }
        for ( int k = 0; k < 3; ++k )
            if ( !( dims[k] > 1e-6 ) )
                dims[k] = 1.0;
        volume.Dimensions = dims;
        return Common::MakeSuccess( std::move( volume ) );
    }

    glm::vec3 UVProjectionRotationDeg( const glm::dmat3& axes )
    {
        // axes = turn * base, so turn = axes * base^T (base is orthonormal).
        const glm::dquat turn = glm::quat_cast( axes * glm::transpose( kProjectionBaseAxes ) );
        return glm::vec3( glm::degrees( glm::eulerAngles( turn ) ) );
    }

    Common::ResultStr<UVOutcome> ProjectUVs( const DynamicMesh3& mesh, const glm::dmat4& meshToWorld,
                                             const UVProjectSettings& settings )
    {
        if ( mesh.TriangleCount() == 0 )
            return Common::MakeError<UVOutcome>( "UV Project: the mesh has no triangle" );
        auto resolved = ResolveUVProjectionVolume( mesh, meshToWorld, settings );
        if ( !resolved.IsSuccess() )
            return Common::MakeError<UVOutcome>( resolved.GetError() );
        const UVProjectionVolume& volume = resolved.GetValue();
        const Frame3d             frame( volume.Origin, volume.AxisX, volume.AxisY, volume.AxisZ );
        const glm::dvec3          dims = volume.Dimensions;

        DynamicMesh3          out = mesh;
        DynamicMeshUVOverlay& uvs = PrepareUVLayer( out );
        DynamicMeshUVEditor   editor( &out, &uvs );
        const auto            toWorld = [&]( const glm::dvec3& p )
        { return glm::dvec3( meshToWorld * glm::dvec4( p, 1.0 ) ); };
        const auto            triangles = AllTriangles( out );
        switch ( settings.Type )
        {
            case UVProjectionType::Plane:
                editor.SetTriangleUVsFromPlanarProjection( triangles, toWorld, frame,
                                                           glm::dvec2( dims.x, dims.y ) );
                break;
            case UVProjectionType::Box:
                editor.SetTriangleUVsFromBoxProjection( triangles, toWorld, frame, dims );
                break;
            case UVProjectionType::Cylinder:
                editor.SetTriangleUVsFromCylinderProjection( triangles, toWorld, frame, dims,
                                                             settings.CylinderSplitAngle );
                break;
        }
        ApplyUVTransform( uvs, settings.UVRotationDeg, settings.UVScale, settings.UVTranslate );

        return Finish( std::move( out ), "UV Project",
                       fmt::format( "{} projection, {:.1f} x {:.1f} x {:.1f} cm", ToString( settings.Type ),
                                    dims.x, dims.y, dims.z ) );
    }

    Common::ResultStr<UVOutcome> UnwrapUVs( const DynamicMesh3& mesh, const UVUnwrapSettings& settings )
    {
        if ( mesh.TriangleCount() == 0 )
            return Common::MakeError<UVOutcome>( "UV Unwrap: the mesh has no triangle" );
        DynamicMesh3          out = mesh;
        DynamicMeshUVOverlay& uvs = PrepareUVLayer( out );

        // The islands, before any UV is reset.
        std::vector<std::vector<int32_t>> islands;
        if ( settings.Source == UVIslandSource::ExistingUVs )
        {
            islands = UVIslands( out, uvs );
            if ( islands.empty() )
                return Common::MakeError<UVOutcome>(
                     "UV Unwrap: Islands from Existing UVs, but the mesh has no UVs - use PolyGroups or AutoUV" );
        }
        else
        {
            if ( !out.HasTriangleGroups() )
                return Common::MakeError<UVOutcome>( "UV Unwrap: Islands from PolyGroups, but the mesh has no "
                                                     "polygroups - use Existing UVs or AutoUV" );
            MeshConnectedComponents components( &out );
            std::vector<int>        all;
            for ( const int tid : out.TriangleIndicesItr() )
                all.push_back( tid );
            components.FindConnectedTriangles( all,
                                               [&out]( int32_t a, int32_t b ) {
                                                   return out.GetTriangleGroup( a ) == out.GetTriangleGroup( b );
                                               } );
            for ( int32_t i = 0; i < components.Num(); ++i )
                islands.emplace_back( components[i].Indices.begin(), components[i].Indices.end() );
        }

        DynamicMeshUVEditor editor( &out, &uvs );
        editor.ResetUVs( AllTriangles( out ) );
        const int32_t projected = SolveIslands( out, editor, islands, settings.Method );
        if ( settings.NormalizeAreas )
            for ( const std::vector<int32_t>& island : islands )
                editor.ScaleUVAreaTo3DArea( island, false );
        if ( settings.AutoRotate )
            for ( const std::vector<int32_t>& island : islands )
                editor.AutoOrientUVArea( island );
        if ( settings.Pack && !editor.QuickPack( settings.PackingResolution ) )
            return Common::MakeFormattedError<UVOutcome>( "UV Unwrap: {} islands do not pack into {} texels",
                                                          islands.size(), settings.PackingResolution );

        return Finish( std::move( out ), "UV Unwrap",
                       fmt::format( "{} islands from {}, {}{}", islands.size(), ToString( settings.Source ),
                                    ToString( settings.Method ),
                                    projected > 0 ? fmt::format( ", {} closed islands projected flat", projected )
                                                  : std::string() ) );
    }

    Common::ResultStr<UVOutcome> LayoutUVs( const DynamicMesh3& mesh, const UVLayoutSettings& settings )
    {
        if ( !mesh.HasAttributes() || mesh.Attributes()->PrimaryUV() == nullptr )
            return Common::MakeError<UVOutcome>(
                 "UV Layout: the mesh has no UV layer - run AutoUV or Project first" );
        DynamicMesh3          out = mesh;
        DynamicMeshUVOverlay& uvs = *out.Attributes()->PrimaryUV();
        if ( UVIslands( out, uvs ).empty() )
            return Common::MakeError<UVOutcome>( "UV Layout: no triangle has UVs - run AutoUV or Project first" );

        if ( settings.Type == UVLayoutType::Repack )
        {
            // FDynamicMeshUVEditor::QuickPack with the tool's packer options (UVLayoutOp::CalculateResult).
            uvs.SplitBowties();
            UVPacker packer;
            packer.TextureResolution = settings.TextureResolution;
            packer.bPreserveScale    = settings.PreserveScale;
            packer.bPreserveRotation = settings.PreserveRotation;
            packer.bAllowFlips       = settings.AllowFlips;
            const auto islands       = UVIslands( out, uvs );
            class View final : public UVPacker::IUVMeshView
            {
            public:
                View( const DynamicMesh3& m, DynamicMeshUVOverlay& o ) : M( m ), O( o )
                {
                }
                Index3i GetTriangle( int32_t t ) const override
                {
                    return M.GetTriangle( t );
                }
                Index3i GetUVTriangle( int32_t t ) const override
                {
                    return O.GetTriangle( t );
                }
                glm::dvec3 GetVertex( int32_t v ) const override
                {
                    return M.GetVertex( v );
                }
                glm::vec2 GetUV( int32_t e ) const override
                {
                    return O.GetElement( e );
                }
                void SetUV( int32_t e, glm::vec2 uv ) override
                {
                    O.SetElement( e, uv );
                }

            private:
                const DynamicMesh3&   M;
                DynamicMeshUVOverlay& O;
            } view( out, uvs );
            if ( !packer.StandardPack( &view, static_cast<int32_t>( islands.size() ),
                                       [&islands]( int32_t i, std::vector<int32_t>& island )
                                       { island = islands[i]; } ) )
                return Common::MakeFormattedError<UVOutcome>( "UV Layout: {} islands do not pack into {} texels",
                                                              islands.size(), settings.TextureResolution );
        }
        ApplyUVTransform( uvs, 0.0f, glm::vec2( settings.Scale ), settings.Translation );

        return Finish( std::move( out ), "UV Layout",
                       fmt::format( "{} at {} texels, scale {:.3f}", ToString( settings.Type ),
                                    settings.TextureResolution, settings.Scale ) );
    }

    const char* ToString( UVTransformPivot pivot )
    {
        switch ( pivot )
        {
            case UVTransformPivot::Origin:
                return "Origin";
            case UVTransformPivot::IslandCenter:
                return "Island Center";
            case UVTransformPivot::SelectionCenter:
                return "Selection Center";
            case UVTransformPivot::Manual:
                return "Manual";
        }
        return "?";
    }

    Common::ResultStr<UVOutcome> TransformUVs( const DynamicMesh3& mesh, const UVTransformSettings& settings,
                                               std::span<const int32_t> triangles )
    {
        if ( !mesh.HasAttributes() || mesh.Attributes()->PrimaryUV() == nullptr )
            return Common::MakeError<UVOutcome>(
                 "UV Transform: the mesh has no UV layer - run AutoUV or Project first" );
        if ( settings.Scale.x == 0.0f || settings.Scale.y == 0.0f )
            return Common::MakeError<UVOutcome>( "UV Transform: a zero scale collapses the islands" );
        DynamicMesh3          out = mesh;
        DynamicMeshUVOverlay& uvs = *out.Attributes()->PrimaryUV();

        // The islands in scope: the UV islands, restricted to the given triangles (UE transforms the selection's
        // connected UV components).
        std::vector<int> scope;
        if ( triangles.empty() )
        {
            for ( const int tid : out.TriangleIndicesItr() )
                if ( uvs.IsSetTriangle( tid ) )
                    scope.push_back( tid );
        }
        else
        {
            for ( const int32_t tid : triangles )
                if ( out.IsTriangle( tid ) && uvs.IsSetTriangle( tid ) )
                    scope.push_back( tid );
        }
        if ( scope.empty() )
            return Common::MakeError<UVOutcome>(
                 triangles.empty() ? "UV Transform: no triangle has UVs - run AutoUV or Project first"
                                   : "UV Transform: no selected triangle has UVs" );
        MeshConnectedComponents components( &out );
        components.FindConnectedTriangles( scope, [&uvs]( int32_t a, int32_t b )
                                           { return uvs.AreTrianglesConnected( a, b ); } );

        // Each island's elements, once each (an element belongs to one island: islands connect through elements).
        std::vector<uint8_t>              seen( static_cast<size_t>( uvs.MaxElementID() ), 0 );
        std::vector<std::vector<int32_t>> islandElements;
        for ( int32_t i = 0; i < components.Num(); ++i )
        {
            std::vector<int32_t>& elements = islandElements.emplace_back();
            for ( const int tid : components[i].Indices )
            {
                const Index3i tri = uvs.GetTriangle( tid );
                for ( int k = 0; k < 3; ++k )
                    if ( !seen[static_cast<size_t>( tri[k] )] )
                    {
                        seen[static_cast<size_t>( tri[k] )] = 1;
                        elements.push_back( tri[k] );
                    }
            }
        }
        const auto boundsCentre = [&uvs]( const std::vector<int32_t>& elements, glm::vec2& lo, glm::vec2& hi )
        {
            for ( const int32_t eid : elements )
            {
                lo = glm::min( lo, uvs.GetElement( eid ) );
                hi = glm::max( hi, uvs.GetElement( eid ) );
            }
        };
        glm::vec2 allLo( std::numeric_limits<float>::max() ), allHi( -std::numeric_limits<float>::max() );
        for ( const auto& elements : islandElements )
            boundsCentre( elements, allLo, allHi );

        const float c = std::cos( glm::radians( settings.RotationDeg ) );
        const float s = std::sin( glm::radians( settings.RotationDeg ) );
        for ( const auto& elements : islandElements )
        {
            glm::vec2 pivot( 0.0f );
            switch ( settings.Pivot )
            {
                case UVTransformPivot::Origin:
                    break;
                case UVTransformPivot::IslandCenter:
                {
                    glm::vec2 lo( std::numeric_limits<float>::max() ), hi( -std::numeric_limits<float>::max() );
                    boundsCentre( elements, lo, hi );
                    pivot = ( lo + hi ) * 0.5f;
                    break;
                }
                case UVTransformPivot::SelectionCenter:
                    pivot = ( allLo + allHi ) * 0.5f;
                    break;
                case UVTransformPivot::Manual:
                    pivot = settings.ManualPivot;
                    break;
            }
            for ( const int32_t eid : elements )
            {
                const glm::vec2 d = ( uvs.GetElement( eid ) - pivot ) * settings.Scale;
                uvs.SetElement( eid,
                                pivot + glm::vec2( c * d.x - s * d.y, s * d.x + c * d.y ) + settings.Translation );
            }
        }

        return Finish( std::move( out ), "UV Transform",
                       fmt::format( "{} of {} triangles; scale {:.3f} x {:.3f}, rotation {:.1f} deg about {}",
                                    scope.size(), mesh.TriangleCount(), settings.Scale.x, settings.Scale.y,
                                    settings.RotationDeg, ToString( settings.Pivot ) ) );
    }
} // namespace Desert::Geometry
