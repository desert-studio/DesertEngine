#include <Engine/Destruction/FractureBake.hpp>

#include <Engine/Geometry/DynamicMeshSerialization.hpp>
#include <Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp>
#include <Engine/Geometry/MeshCore/IndexTypes.hpp>
#include <Engine/Geometry/MeshCore/Operations/MeshBoolean.hpp>
#include <Engine/Geometry/MeshCore/Selections/MeshConnectedComponents.hpp>
#include <Engine/Geometry/MeshCore/VectorUtil.hpp>

#include <Jolt/Jolt.h>

#include <Jolt/Geometry/ConvexHullBuilder.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>

namespace Desert::Destruction
{
    using Geometry::DynamicMesh3;
    using Geometry::Index3i;

    const char* ToString( FractureMethod method )
    {
        switch ( method )
        {
            case FractureMethod::Uniform:
                return "Uniform";
            case FractureMethod::Clustered:
                return "Clustered";
            case FractureMethod::Planar:
                return "Planar";
            case FractureMethod::Brick:
                return "Brick";
        }
        return "?";
    }

    namespace
    {
        // The port's outward winding is UE's clockwise-from-the-front (VectorUtil::Normal), so the raw
        // divergence sum is negative for an outward mesh; the sign is read off VectorUtil itself rather than
        // assumed, so the two can never disagree.
        double OutwardSign()
        {
            const glm::dvec3 a( 0, 0, 0 );
            const glm::dvec3 b( 1, 0, 0 );
            const glm::dvec3 c( 0, 1, 0 );
            return glm::dot( Geometry::VectorUtil::Normal( a, b, c ), glm::cross( b - a, c - a ) ) > 0 ? 1.0
                                                                                                       : -1.0;
        }

        struct MassProperties
        {
            double     Volume = 0.0;
            glm::dvec3 Center{ 0.0 };
        };

        MassProperties MassOf( const DynamicMesh3& mesh )
        {
            const double   sign = OutwardSign();
            MassProperties out;
            glm::dvec3     moment( 0.0 );
            for ( const int t : mesh.TriangleIndicesItr() )
            {
                glm::dvec3 a;
                glm::dvec3 b;
                glm::dvec3 c;
                mesh.GetTriVertices( t, a, b, c );
                const double v = sign * glm::dot( a, glm::cross( b, c ) ) / 6.0;
                out.Volume += v;
                moment += v * ( a + b + c ) * 0.25;
            }
            out.Center = out.Volume != 0.0 ? moment / out.Volume : glm::dvec3( 0.0 );
            return out;
        }

        uint64_t SplitMix64( uint64_t x )
        {
            x += 0x9E3779B97F4A7C15ull;
            x = ( x ^ ( x >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
            x = ( x ^ ( x >> 27 ) ) * 0x94D049BB133111EBull;
            return x ^ ( x >> 31 );
        }

        /// The seed one parent's cut at one level draws from: independent of every other parent's.
        uint64_t PieceSeed( uint64_t seed, uint32_t level, int32_t parent )
        {
            return SplitMix64( seed ^ SplitMix64( ( static_cast<uint64_t>( level ) << 32 ) ^
                                                  static_cast<uint64_t>( static_cast<uint32_t>( parent ) ) ) );
        }

        Common::BoolResultStr Validate( const FractureLevelSettings& level, size_t index )
        {
            switch ( level.Method )
            {
                case FractureMethod::Uniform:
                    if ( level.SiteCount < 1 )
                        return Common::MakeFormattedError<bool>(
                             "level {}: Uniform needs at least one site, not {}", index + 1, level.SiteCount );
                    break;
                case FractureMethod::Clustered:
                    if ( level.Clusters < 1 || level.SitesPerCluster < 1 || level.MinRadius < 0.0 ||
                         level.MaxRadius < level.MinRadius )
                        return Common::MakeFormattedError<bool>( "level {}: Clustered needs >= 1 cluster of >= 1 "
                                                                 "site and 0 <= MinRadius <= MaxRadius, "
                                                                 "not {} x {} in [{}, {}] cm",
                                                                 index + 1, level.Clusters, level.SitesPerCluster,
                                                                 level.MinRadius, level.MaxRadius );
                    break;
                case FractureMethod::Planar:
                    if ( level.Planes.empty() )
                        return Common::MakeFormattedError<bool>( "level {}: Planar has no planes", index + 1 );
                    for ( const CutPlane& plane : level.Planes )
                        if ( glm::dot( plane.Normal, plane.Normal ) == 0.0 )
                            return Common::MakeFormattedError<bool>( "level {}: a cut plane has a zero normal",
                                                                     index + 1 );
                    break;
                case FractureMethod::Brick:
                    if ( !( level.Brick.Length > 0.0 ) || !( level.Brick.Height > 0.0 ) ||
                         !( level.Brick.Depth > 0.0 ) )
                        return Common::MakeFormattedError<bool>(
                             "level {}: Brick sizes must be positive, not {} x {} x {} cm", index + 1,
                             level.Brick.Length, level.Brick.Height, level.Brick.Depth );
                    break;
            }
            return BOOLSUCCESS;
        }

        std::vector<ConvexCell> CellsFor( const FractureLevelSettings& level, const CellBounds& pieceBounds,
                                          const CellBounds& cellBounds, FractureRandom& random )
        {
            switch ( level.Method )
            {
                case FractureMethod::Uniform:
                    return ComputeVoronoiCells( GenerateUniformSites( pieceBounds, level.SiteCount, random ),
                                                cellBounds );
                case FractureMethod::Clustered:
                {
                    std::vector<glm::dvec3> sites =
                         GenerateClusteredSites( pieceBounds, level.Clusters, level.SitesPerCluster,
                                                 level.MinRadius, level.MaxRadius, random );
                    // A site the radius pushed past the cell box yields no cell (ComputeVoronoiCells); it is
                    // dropped here, by the same rule, so the remaining sites keep their order.
                    std::erase_if( sites,
                                   [&]( const glm::dvec3& s ) {
                                       return glm::any( glm::lessThan( s, cellBounds.Min ) ) ||
                                              glm::any( glm::greaterThan( s, cellBounds.Max ) );
                                   } );
                    return ComputeVoronoiCells( sites, cellBounds );
                }
                case FractureMethod::Planar:
                    return ComputePlanarCells( level.Planes, cellBounds );
                case FractureMethod::Brick:
                    return ComputeBrickCells( level.Brick, cellBounds );
            }
            return {};
        }

        /// The cell as a closed mesh carrying @p like's attribute layout: every triangle in the interior
        /// material, flat normals, box-projected UVs (UE PlanarCut.cpp:1922-2031), a tangent frame along U.
        DynamicMesh3 CellMesh( const ConvexCell& cell, const DynamicMesh3& like, int32_t interiorMaterial,
                               double uvScale )
        {
            DynamicMesh3 mesh;
            mesh.EnableAttributes();
            mesh.Attributes()->EnableMatchingAttributes( *like.Attributes() );
            for ( const glm::dvec3& v : cell.Vertices )
                mesh.AppendVertex( v );

            glm::dvec3 centre( 0.0 );
            for ( const glm::dvec3& v : cell.Vertices )
                centre += v;
            centre /= static_cast<double>( std::max<size_t>( 1, cell.Vertices.size() ) );

            Geometry::DynamicMeshAttributeSet& attributes = *mesh.Attributes();
            for ( const std::vector<int>& face : cell.Faces )
            {
                if ( face.size() < 3 )
                    continue;
                glm::dvec3 faceCentre( 0.0 );
                for ( const int v : face )
                    faceCentre += cell.Vertices[v];
                faceCentre /= static_cast<double>( face.size() );
                const glm::dvec3 outward = faceCentre - centre;

                for ( size_t k = 1; k + 1 < face.size(); ++k )
                {
                    const int  a = face[0];
                    int        b = face[k];
                    int        c = face[k + 1];
                    glm::dvec3 n =
                         Geometry::VectorUtil::Normal( cell.Vertices[a], cell.Vertices[b], cell.Vertices[c] );
                    if ( glm::dot( n, outward ) < 0.0 )
                    {
                        std::swap( b, c );
                        n = -n;
                    }
                    const int tid = mesh.AppendTriangle( a, b, c );
                    if ( tid < 0 )
                        continue; // a degenerate fan triangle of a voro++ face: the face stays closed without it

                    if ( auto* material = attributes.GetMaterialID() )
                        material->SetValue( tid, interiorMaterial );

                    // Box projection: drop the dominant axis of the face normal.
                    const glm::dvec3 an   = glm::abs( n );
                    const int        drop = [&an]
                    {
                        if ( an.x >= an.y && an.x >= an.z )
                            return 0;
                        return an.y >= an.z ? 1 : 2;
                    }();
                    const int uAxis      = ( drop + 1 ) % 3;
                    const int vAxis      = ( drop + 2 ) % 3;
                    const int corners[3] = { a, b, c };

                    for ( int layer = 0; layer < attributes.NumUVLayers(); ++layer )
                    {
                        auto* uv = attributes.GetUVLayer( layer );
                        int   e[3];
                        for ( int i = 0; i < 3; ++i )
                        {
                            const glm::dvec3& p = cell.Vertices[corners[i]];
                            e[i] = uv->AppendElement( glm::vec2( static_cast<float>( p[uAxis] * uvScale ),
                                                                 static_cast<float>( p[vAxis] * uvScale ) ) );
                        }
                        uv->SetTriangle( tid, Index3i( e[0], e[1], e[2] ) );
                    }

                    glm::dvec3 uDir( 0.0 );
                    uDir[uAxis]               = 1.0;
                    const glm::dvec3 tang     = glm::normalize( uDir - n * glm::dot( n, uDir ) );
                    const glm::dvec3 frame[3] = { n, tang, glm::cross( n, tang ) };
                    // An overlay element belongs to ONE parent vertex, so a flat value still takes one element
                    // per corner (UE PlanarCut appends per vertex); Index3i(e,e,e) is refused by SetTriangle.
                    for ( int layer = 0; layer < attributes.NumNormalLayers() && layer < 3; ++layer )
                    {
                        auto* overlay = attributes.GetNormalLayer( layer );
                        int   e[3];
                        for ( int& element : e )
                            element = overlay->AppendElement( glm::vec3( frame[layer] ) );
                        overlay->SetTriangle( tid, Index3i( e[0], e[1], e[2] ) );
                    }
                    if ( auto* colors = attributes.PrimaryColors() )
                    {
                        int e[3];
                        for ( int& element : e )
                            element = colors->AppendElement( glm::vec4( 1.0f ) );
                        colors->SetTriangle( tid, Index3i( e[0], e[1], e[2] ) );
                    }
                }
            }
            return mesh;
        }

        CellBounds BoundsOf( const DynamicMesh3& mesh )
        {
            const auto box = mesh.GetBounds();
            return { glm::dvec3( box.Min ), glm::dvec3( box.Max ) };
        }

        /// @p piece cut by @p cells into its islands, in cell order then island order.
        Common::ResultStr<std::vector<DynamicMesh3>> CutPiece( const DynamicMesh3&            piece,
                                                               const std::vector<ConvexCell>& cells,
                                                               int32_t interiorMaterial, double uvScale )
        {
            const double              pieceVolume = MassOf( piece ).Volume;
            std::vector<DynamicMesh3> out;
            for ( size_t ci = 0; ci < cells.size(); ++ci )
            {
                const DynamicMesh3    cellMesh = CellMesh( cells[ci], piece, interiorMaterial, uvScale );
                DynamicMesh3          cut;
                Geometry::MeshBoolean boolean( &piece, &cellMesh, &cut,
                                               Geometry::MeshBoolean::BooleanOp::Intersect );
                const bool            computed = boolean.Compute();
                if ( cut.TriangleCount() == 0 )
                    continue; // the cell misses the piece
                if ( !computed || !boolean.CreatedBoundaryEdges.empty() )
                    return Common::MakeFormattedError<std::vector<DynamicMesh3>>(
                         "cell {} of {}: the intersection left {} open edges unwelded", ci, cells.size(),
                         boolean.CreatedBoundaryEdges.size() );

                Geometry::MeshConnectedComponents components( &cut );
                std::vector<int>                  all;
                all.reserve( static_cast<size_t>( cut.TriangleCount() ) );
                for ( const int t : cut.TriangleIndicesItr() )
                    all.push_back( t );
                components.FindConnectedTriangles( all );

                for ( int32_t k = 0; k < components.Num(); ++k )
                {
                    DynamicMesh3 island;
                    if ( components.Num() == 1 )
                        island.CompactCopy( cut );
                    else
                    {
                        std::vector<uint8_t> keep( static_cast<size_t>( cut.MaxTriangleID() ), 0 );
                        for ( const int t : components[k].Indices )
                            keep[static_cast<size_t>( t )] = 1;
                        DynamicMesh3 trimmed( cut );
                        for ( const int t : all )
                            if ( keep[static_cast<size_t>( t )] == 0 )
                                trimmed.RemoveTriangle( t );
                        island.CompactCopy( trimmed );
                    }
                    // A sliver the boolean leaves at a cell corner carries no mass and no stable hull; UE's
                    // fracture cleanup drops the same (its Min Volume), relative to the piece being cut.
                    if ( MassOf( island ).Volume <= 1e-9 * pieceVolume )
                        continue;
                    out.push_back( std::move( island ) );
                }
            }
            return Common::MakeSuccess( std::move( out ) );
        }

        Common::BoolResultStr BuildHull( const DynamicMesh3& mesh, FractureNode& node )
        {
            static std::once_flag allocator;
            std::call_once( allocator,
                            []
                            {
                                // Jolt's allocator is a global pointer that is null until registered
                                // (PhysicsWorld.cpp registers the default one); the bake may run before physics
                                // ever starts.
                                if ( JPH::Allocate == nullptr )
                                    JPH::RegisterDefaultAllocator();
                            } );

            JPH::Array<JPH::Vec3> positions;
            for ( const int v : mesh.VertexIndicesItr() )
            {
                const glm::dvec3 p = mesh.GetVertex( v );
                positions.push_back( JPH::Vec3( static_cast<float>( p.x ), static_cast<float>( p.y ),
                                                static_cast<float>( p.z ) ) );
            }
            JPH::ConvexHullBuilder builder( positions );
            const char*            error  = nullptr;
            const auto             result = builder.Initialize( INT_MAX, 1e-3f, error );
            if ( result != JPH::ConvexHullBuilder::EResult::Success &&
                 result != JPH::ConvexHullBuilder::EResult::MaxVerticesReached )
                return Common::MakeFormattedError<bool>( "the convex hull of a {}-vertex piece failed: {}",
                                                         positions.size(), error != nullptr ? error : "?" );

            std::map<int, int> remap;
            for ( const JPH::ConvexHullBuilder::Face* face : builder.GetFaces() )
            {
                if ( face->mRemoved )
                    continue;
                std::vector<int>                    polygon;
                const JPH::ConvexHullBuilder::Edge* e = face->mFirstEdge;
                do
                {
                    auto [it, inserted] = remap.try_emplace( e->mStartIdx, static_cast<int>( remap.size() ) );
                    if ( inserted )
                    {
                        const JPH::Vec3 p = positions[static_cast<size_t>( e->mStartIdx )];
                        node.HullVertices.emplace_back( p.GetX(), p.GetY(), p.GetZ() );
                    }
                    polygon.push_back( it->second );
                    e = e->mNextEdge;
                } while ( e != face->mFirstEdge );
                node.HullFaces.push_back( std::move( polygon ) );
            }
            return BOOLSUCCESS;
        }

        /// UE FVoronoiPartitioner::KMeansPartition over @p centres, initial centres @p sites.
        std::vector<int> KMeans( const std::vector<glm::dvec3>& centres, std::vector<glm::dvec3> sites,
                                 int maxIterations )
        {
            std::vector<int> partition( centres.size(), -1 );
            std::vector<int> size( sites.size(), 0 );
            for ( int iteration = 0; iteration < maxIterations + 1; ++iteration )
            {
                bool changed = false;
                for ( size_t i = 0; i < centres.size(); ++i )
                {
                    int    best  = 0;
                    double bestD = std::numeric_limits<double>::max();
                    for ( size_t s = 0; s < sites.size(); ++s )
                    {
                        const glm::dvec3 d  = centres[i] - sites[s];
                        const double     d2 = glm::dot( d, d );
                        if ( d2 < bestD )
                        {
                            bestD = d2;
                            best  = static_cast<int>( s );
                        }
                    }
                    if ( best != partition[i] )
                    {
                        changed = true;
                        if ( partition[i] >= 0 )
                            --size[static_cast<size_t>( partition[i] )];
                        ++size[static_cast<size_t>( best )];
                        partition[i] = best;
                    }
                }
                if ( !changed )
                    break;
                for ( size_t s = 0; s < sites.size(); ++s )
                    if ( size[s] > 0 )
                        sites[s] = glm::dvec3( 0.0 );
                for ( size_t i = 0; i < centres.size(); ++i )
                    sites[static_cast<size_t>( partition[i] )] += centres[i];
                for ( size_t s = 0; s < sites.size(); ++s )
                    if ( size[s] > 0 )
                        sites[s] /= static_cast<double>( size[s] );
            }
            return partition;
        }
    } // namespace

    double EnclosedVolume( const DynamicMesh3& mesh )
    {
        return MassOf( mesh ).Volume;
    }

    Common::ResultStr<FractureBakeResult> BakeFracture( const DynamicMesh3&     sourceIn,
                                                        const FractureSettings& settings )
    {
        using Result = FractureBakeResult;
        if ( sourceIn.TriangleCount() == 0 )
            return Common::MakeError<Result>( "the source mesh is empty" );
        if ( !sourceIn.IsClosed() )
            return Common::MakeError<Result>( "the source mesh has open edges: inside and outside are undefined" );
        for ( size_t i = 0; i < settings.Levels.size(); ++i )
            if ( auto valid = Validate( settings.Levels[i], i ); !valid )
                return Common::MakeFormattedError<Result>( "{}", valid.GetError() );

        // The source with a material layer: its own IDs kept, the interior one past the highest.
        DynamicMesh3 source( sourceIn );
        if ( !source.HasAttributes() )
            source.EnableAttributes();
        if ( !source.Attributes()->HasMaterialID() )
            source.Attributes()->EnableMaterialID();
        // A piece is stored with a polygroup per triangle; a mesh without groups reports -1 for every
        // triangle, which the reader (DynamicMeshFromSerialized) rebuilds into an invalid group. One group 0.
        if ( !source.HasTriangleGroups() )
            source.EnableTriangleGroups( 0 );
        int32_t maxMaterial = 0;
        for ( const int t : source.TriangleIndicesItr() )
            maxMaterial = std::max( maxMaterial, source.Attributes()->GetMaterialID()->GetValue( t ) );

        Result result;
        result.InteriorMaterialId = maxMaterial + 1;

        // Build: nodes in creation order, a mesh per live leaf.
        std::vector<FractureNode>                nodes( 1 );
        std::vector<std::optional<DynamicMesh3>> meshes;
        meshes.emplace_back( std::move( source ) );
        std::vector<int32_t> frontier{ 0 };

        for ( size_t li = 0; li < settings.Levels.size(); ++li )
        {
            const FractureLevelSettings& level = settings.Levels[li];
            const auto                   depth = static_cast<uint32_t>( li + 1 );
            std::vector<int32_t>         next;
            for ( const int32_t parent : frontier )
            {
                const DynamicMesh3& piece       = *meshes[static_cast<size_t>( parent )];
                const CellBounds    pieceBounds = BoundsOf( piece );
                // The cell box stands off the piece so no cell wall lies on the piece's own surface (a
                // coplanar boolean is the one case the cut cannot resolve).
                const double     margin = 0.01 * glm::length( pieceBounds.Max - pieceBounds.Min ) + 0.1;
                const CellBounds cellBounds{ pieceBounds.Min - glm::dvec3( margin ),
                                             pieceBounds.Max + glm::dvec3( margin ) };
                FractureRandom   random( PieceSeed( settings.Seed, depth, parent ) );
                const auto       cells = CellsFor( level, pieceBounds, cellBounds, random );

                auto pieces = CutPiece( piece, cells, result.InteriorMaterialId, settings.InteriorUVScale );
                if ( !pieces )
                    return Common::MakeFormattedError<Result>( "level {}, node {} ({}): {}", depth, parent,
                                                               ToString( level.Method ), pieces.GetError() );
                std::vector<DynamicMesh3> cut = pieces.ExtractValue();
                if ( cut.size() <= 1 )
                    continue; // the cut left the piece whole: it stays a leaf of its level

                for ( DynamicMesh3& child : cut )
                {
                    FractureNode node;
                    node.Parent          = parent;
                    node.DamageThreshold = level.DamageThreshold;
                    next.push_back( static_cast<int32_t>( nodes.size() ) );
                    nodes.push_back( std::move( node ) );
                    meshes.emplace_back( std::move( child ) );
                }
                meshes[static_cast<size_t>( parent )].reset();
            }
            frontier = std::move( next );
        }

        // Bounds per node (subtree), children always after their parent.
        std::vector<CellBounds> bounds( nodes.size(),
                                        CellBounds{ glm::dvec3( std::numeric_limits<double>::max() ),
                                                    glm::dvec3( -std::numeric_limits<double>::max() ) } );
        for ( size_t i = nodes.size(); i-- > 0; )
        {
            if ( meshes[i] )
                bounds[i] = BoundsOf( *meshes[i] );
            if ( nodes[i].Parent >= 0 )
            {
                CellBounds& p = bounds[static_cast<size_t>( nodes[i].Parent )];
                p.Min         = glm::min( p.Min, bounds[i].Min );
                p.Max         = glm::max( p.Max, bounds[i].Max );
            }
        }

        // Auto-cluster every parent's children (UE ByGrid).
        if ( settings.AutoCluster.Enabled )
        {
            const size_t built = nodes.size();
            for ( size_t parent = 0; parent < built; ++parent )
            {
                std::vector<int32_t> children;
                for ( size_t i = parent + 1; i < built; ++i )
                    if ( nodes[i].Parent == static_cast<int32_t>( parent ) )
                        children.push_back( static_cast<int32_t>( i ) );
                if ( children.size() < 2 )
                    continue;

                const CellBounds&       pb = bounds[parent];
                const glm::ivec3        grid( std::max( 1, settings.AutoCluster.GridX ),
                                              std::max( 1, settings.AutoCluster.GridY ),
                                              std::max( 1, settings.AutoCluster.GridZ ) );
                std::vector<glm::dvec3> sites;
                for ( int x = 0; x < grid.x; ++x )
                    for ( int y = 0; y < grid.y; ++y )
                        for ( int z = 0; z < grid.z; ++z )
                            sites.push_back( glm::mix( pb.Min, pb.Max,
                                                       ( glm::dvec3( x, y, z ) + 0.5 ) / glm::dvec3( grid ) ) );
                std::vector<glm::dvec3> centres;
                centres.reserve( children.size() );
                for ( const int32_t c : children )
                    centres.push_back(
                         0.5 * ( bounds[static_cast<size_t>( c )].Min + bounds[static_cast<size_t>( c )].Max ) );
                const std::vector<int> partition =
                     KMeans( centres, sites, std::max( 0, settings.AutoCluster.DriftIterations ) );

                for ( size_t s = 0; s < sites.size(); ++s )
                {
                    std::vector<int32_t> group;
                    for ( size_t k = 0; k < children.size(); ++k )
                        if ( partition[k] == static_cast<int>( s ) )
                            group.push_back( children[k] );
                    if ( group.size() < 2 || group.size() == children.size() )
                        continue;
                    FractureNode cluster;
                    cluster.Parent          = static_cast<int32_t>( parent );
                    cluster.Kind            = FractureNodeKind::Cluster;
                    cluster.DamageThreshold = nodes[static_cast<size_t>( group[0] )].DamageThreshold;
                    const auto id           = static_cast<int32_t>( nodes.size() );
                    nodes.push_back( std::move( cluster ) );
                    meshes.emplace_back();
                    for ( const int32_t c : group )
                        nodes[static_cast<size_t>( c )].Parent = id;
                }
            }
        }

        // Final order: breadth-first from the root, children in creation order — parents before children,
        // the root at 0, Level = depth.
        std::vector<std::vector<int32_t>> childrenOf( nodes.size() );
        for ( size_t i = 1; i < nodes.size(); ++i )
            childrenOf[static_cast<size_t>( nodes[i].Parent )].push_back( static_cast<int32_t>( i ) );
        std::vector<int32_t> order{ 0 };
        std::vector<int32_t> newIndex( nodes.size(), -1 );
        newIndex[0] = 0;
        for ( size_t head = 0; head < order.size(); ++head )
            for ( const int32_t c : childrenOf[static_cast<size_t>( order[head] )] )
            {
                newIndex[static_cast<size_t>( c )] = static_cast<int32_t>( order.size() );
                order.push_back( c );
            }

        result.Nodes.resize( order.size() );
        for ( size_t k = 0; k < order.size(); ++k )
        {
            const auto    old  = static_cast<size_t>( order[k] );
            FractureNode& node = result.Nodes[k];
            node               = std::move( nodes[old] );
            node.Parent        = node.Parent < 0 ? -1 : newIndex[static_cast<size_t>( node.Parent )];
            node.Level = node.Parent < 0 ? 0u : result.Nodes[static_cast<size_t>( node.Parent )].Level + 1u;
            if ( meshes[old] )
            {
                const MassProperties mass = MassOf( *meshes[old] );
                node.Volume               = mass.Volume;
                node.CenterOfMass         = mass.Center;
                node.Mesh                 = Geometry::ToSerialized( *meshes[old] );
                if ( auto hull = BuildHull( *meshes[old], node ); !hull )
                    return Common::MakeFormattedError<Result>( "node {}: {}", k, hull.GetError() );
            }
        }
        // Inner nodes: the mass of their leaves, accumulated bottom-up.
        std::vector<glm::dvec3> moment( result.Nodes.size(), glm::dvec3( 0.0 ) );
        for ( size_t k = result.Nodes.size(); k-- > 0; )
        {
            FractureNode& node = result.Nodes[k];
            if ( node.Mesh.Triangles.empty() )
                node.CenterOfMass = node.Volume != 0.0 ? moment[k] / node.Volume : glm::dvec3( 0.0 );
            else
                moment[k] = node.CenterOfMass * node.Volume;
            if ( node.Parent >= 0 )
            {
                result.Nodes[static_cast<size_t>( node.Parent )].Volume += node.Volume;
                moment[static_cast<size_t>( node.Parent )] += moment[k];
            }
        }
        return Common::MakeSuccess( std::move( result ) );
    }
} // namespace Desert::Destruction
