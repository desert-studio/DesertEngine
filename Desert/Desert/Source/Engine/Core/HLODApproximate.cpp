#include <Engine/Core/HLODApproximate.hpp>

#include <Engine/Geometry/DynamicMeshUVOperations.hpp>
#include <Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp>
#include <Engine/Geometry/MeshCore/DynamicMesh/MeshNormals.hpp>
#include <Engine/Geometry/MeshCore/Spatial/MeshAABBTree3.hpp>
#include <Engine/Geometry/MeshSimplifyOperation.hpp>
#include <Engine/Geometry/MeshVoxelOperations.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <utility>

namespace Desert::Core
{
    namespace
    {
        // Bisection steps of the voxel surface search (UE UVoxelProperties::SurfaceSearchSteps default).
        constexpr int32_t kSurfaceSearchSteps = 5;
        // UE FMeshApproximationSettings::TargetMinThicknessMultiplier (bAttemptAutoThickening, on by default): an
        // open shell (a single-sided wall or roof) is thickened to this many voxels before the wrap, or it would
        // fall between the samples and leave a hole in the shell. Two, not UE's 1.5: VoxWrap thickens by at
        // least two cells whatever it is asked (MeshVoxelOperations.cpp, as UE TImplicitSolidify), so 1.5 would
        // be a number nobody's result follows.
        constexpr float kMinThicknessVoxels = 2.0f;
        // The fewest triangles the tolerance search tries (a tetrahedron).
        constexpr int kMinTriangles = 4;
        // Bisection steps between the last count within tolerance and the first beyond it.
        constexpr int kToleranceBisections = 5;
        // Gutter passes: how many texels a UV island's colour is spread outward (mip and bilinear bleed).
        constexpr int   kGutterPasses = 16;
        constexpr float kFlatEpsilon  = 1.0e-6f;
        constexpr float kMinTangentZ  = 1.0e-3f;

        [[nodiscard]] int32_t VoxelsAlongLongestSide( const Geometry::DynamicMesh3&    mesh,
                                                      const HLODApproximationSettings& settings, double pad )
        {
            glm::dvec3 lo( std::numeric_limits<double>::max() );
            glm::dvec3 hi( std::numeric_limits<double>::lowest() );
            for ( const int v : mesh.VertexIndicesItr() )
            {
                const glm::dvec3 p = mesh.GetVertex( v );
                lo                 = glm::min( lo, p );
                hi                 = glm::max( hi, p );
            }
            const double longest = std::max( { hi.x - lo.x, hi.y - lo.y, hi.z - lo.z } ) + 2.0 * pad;
            const double cells   = std::ceil( longest / static_cast<double>( settings.WorldSpaceApproximationAccuracy ) );
            return static_cast<int32_t>( std::clamp( cells, 8.0, static_cast<double>( settings.ClampVoxelDimension ) ) );
        }

        // Drops every closed component wound against the outer surface: Marching Cubes orients each component of
        // the iso-surface away from the solid, so an inner cavity's signed volume has the opposite sign of the
        // outer shell's (the component with the largest volume is outer by construction). Answers how many.
        [[nodiscard]] int32_t RemoveInnerCavities( Geometry::DynamicMesh3& mesh )
        {
            std::vector<int> parent( static_cast<std::size_t>( std::max( 0, mesh.MaxVertexID() ) ) );
            std::iota( parent.begin(), parent.end(), 0 );
            const auto find = [&parent]( int v )
            {
                while ( parent[v] != v )
                {
                    parent[v] = parent[parent[v]];
                    v         = parent[v];
                }
                return v;
            };
            for ( const int t : mesh.TriangleIndicesItr() )
            {
                const Geometry::Index3i tri = mesh.GetTriangle( t );
                parent[find( tri.B )]       = find( tri.A );
                parent[find( tri.C )]       = find( tri.A );
            }
            std::unordered_map<int, double> volume;
            for ( const int t : mesh.TriangleIndicesItr() )
            {
                const Geometry::Index3i tri = mesh.GetTriangle( t );
                const glm::dvec3        a   = mesh.GetVertex( tri.A );
                const glm::dvec3        b   = mesh.GetVertex( tri.B );
                const glm::dvec3        c   = mesh.GetVertex( tri.C );
                volume[find( tri.A )] += glm::dot( a, glm::cross( b, c ) ) / 6.0;
            }
            double outer = 0.0;
            for ( const auto& component : volume )
                if ( std::abs( component.second ) > std::abs( outer ) )
                    outer = component.second;
            int32_t          cavities = 0;
            std::vector<int> drop;
            for ( const auto& component : volume )
                if ( component.second * outer < 0.0 )
                    ++cavities;
            if ( cavities == 0 )
                return 0;
            for ( const int t : mesh.TriangleIndicesItr() )
            {
                const auto found = volume.find( find( mesh.GetTriangle( t ).A ) );
                if ( found != volume.end() && found->second * outer < 0.0 )
                    drop.push_back( t );
            }
            for ( const int t : drop )
                mesh.RemoveTriangle( t, true );
            return cavities;
        }

        // The two-sided vertex distance between @p candidate and the surface it was simplified from, cm.
        [[nodiscard]] double SurfaceDeviation( const Geometry::DynamicMesh3&         candidate,
                                               const Geometry::DynamicMesh3&         reference,
                                               const Geometry::DynamicMeshAABBTree3& referenceTree )
        {
            const Geometry::DynamicMeshAABBTree3 candidateTree( &candidate );
            double                               worst = 0.0;
            for ( const int v : candidate.VertexIndicesItr() )
            {
                double distanceSqr = 0.0;
                if ( referenceTree.FindNearestTriangle( candidate.GetVertex( v ), distanceSqr ) !=
                     Geometry::DynamicMesh3::InvalidID )
                    worst = std::max( worst, distanceSqr );
            }
            for ( const int v : reference.VertexIndicesItr() )
            {
                double distanceSqr = 0.0;
                if ( candidateTree.FindNearestTriangle( reference.GetVertex( v ), distanceSqr ) !=
                     Geometry::DynamicMesh3::InvalidID )
                    worst = std::max( worst, distanceSqr );
            }
            return std::sqrt( worst );
        }

        // UE SimplifyMethod GeometricTolerance: the fewest triangles whose shell stays within @p tolerance of
        // @p shell. The count halves while the result stays within it, then bisects between the last count that
        // did and the first that did not; each trial starts from the last accepted mesh.
        [[nodiscard]] Geometry::DynamicMesh3 SimplifyToTolerance( const Geometry::DynamicMesh3& shell,
                                                                  double tolerance, double& deviation )
        {
            const Geometry::DynamicMeshAABBTree3 referenceTree( &shell );
            Geometry::DynamicMesh3               accepted = shell;
            deviation                                     = 0.0;
            int failed                                    = 0; // a count beyond the tolerance; 0 = none found yet
            for ( int target = accepted.TriangleCount() / 2; target >= kMinTriangles; )
            {
                Geometry::DynamicMesh3 trial = accepted;
                Geometry::SimplifyToTriangleCount( trial, target );
                if ( trial.TriangleCount() >= accepted.TriangleCount() )
                    break; // no edge may collapse any more
                const double trialDeviation = SurfaceDeviation( trial, shell, referenceTree );
                if ( trialDeviation > tolerance )
                {
                    failed = target;
                    break;
                }
                accepted  = std::move( trial );
                deviation = trialDeviation;
                target    = accepted.TriangleCount() / 2;
            }
            for ( int step = 0; failed > 0 && step < kToleranceBisections; ++step )
            {
                const int middle = ( failed + accepted.TriangleCount() ) / 2;
                if ( middle <= failed || middle >= accepted.TriangleCount() )
                    break;
                Geometry::DynamicMesh3 trial = accepted;
                Geometry::SimplifyToTriangleCount( trial, middle );
                const double trialDeviation = SurfaceDeviation( trial, shell, referenceTree );
                if ( trialDeviation > tolerance )
                {
                    failed = middle;
                    continue;
                }
                accepted  = std::move( trial );
                deviation = trialDeviation;
            }
            return accepted;
        }

        [[nodiscard]] uint8_t UnitToByte( float value )
        {
            return static_cast<uint8_t>( std::clamp( std::lround( value * 255.0f ), 0L, 255L ) );
        }

        // FNV-1a, 64 bit.
        struct KeyHash
        {
            uint64_t Value = 1469598103934665603ull;

            void Bytes( const void* data, std::size_t size )
            {
                const auto* bytes = static_cast<const uint8_t*>( data );
                for ( std::size_t i = 0; i < size; ++i )
                {
                    Value ^= bytes[i];
                    Value *= 1099511628211ull;
                }
            }
            void Text( const std::string& text )
            {
                const uint64_t size = text.size();
                Bytes( &size, sizeof( size ) );
                Bytes( text.data(), text.size() );
            }
            template <typename T> void Plain( const T& value )
            {
                Bytes( &value, sizeof( T ) );
            }
        };
    } // namespace

    Common::ResultStr<ApproximateShell> BuildApproximateShell( const Geometry::DynamicMesh3&    merged,
                                                               const HLODApproximationSettings& settings )
    {
        using Result = ApproximateShell;
        if ( auto valid = Rules::ValidateHLODApproximation( settings ); !valid )
            return Common::MakeError<Result>( valid.GetError() );
        if ( merged.TriangleCount() == 0 )
            return Common::MakeError<Result>( "the merged parts hold no triangles to approximate" );
        const float accuracy = settings.WorldSpaceApproximationAccuracy;

        ApproximateShell shell;
        shell.SourceTriangles = merged.TriangleCount();

        // 1. The wrap (UE TImplicitSolidify over the fast winding number).
        Geometry::VoxWrapSettings wrap;
        wrap.VoxelCount         = VoxelsAlongLongestSide( merged, settings, accuracy );
        wrap.WindingThreshold   = settings.WindingThreshold;
        wrap.ExtendBounds       = accuracy;
        wrap.SurfaceSearchSteps = kSurfaceSearchSteps;
        wrap.SolidAtBoundaries  = true;
        wrap.ThickenShells      = true;
        wrap.ShellThickness     = kMinThicknessVoxels * accuracy;
        shell.VoxelCount        = wrap.VoxelCount;
        auto wrapped            = Geometry::VoxWrap( merged, wrap );
        if ( !wrapped )
            return Common::MakeError<Result>( "the voxel wrap: " + wrapped.GetError() );
        Geometry::DynamicMesh3 mesh = std::move( wrapped.ExtractValue().Mesh );

        // 2. The closure (UE bFillGaps: TImplicitMorphology Close by GapDistance).
        if ( settings.FillGaps )
        {
            Geometry::VoxMorphSettings close;
            close.Operation          = Geometry::VoxMorphOperation::Close;
            close.Distance           = settings.GapDistance;
            close.VoxelCount         = VoxelsAlongLongestSide( mesh, settings, settings.GapDistance );
            close.SurfaceSearchSteps = kSurfaceSearchSteps;
            auto closed              = Geometry::VoxMorph( mesh, close );
            if ( !closed )
                return Common::MakeError<Result>( "the gap closure: " + closed.GetError() );
            mesh = std::move( closed.ExtractValue().Mesh );
        }

        // 3. The cavities.
        shell.CavitiesRemoved = RemoveInnerCavities( mesh );
        if ( mesh.TriangleCount() == 0 )
            return Common::MakeError<Result>( "nothing of the shell is left outside its inner cavities" );
        shell.WrappedTriangles = mesh.TriangleCount();

        // 4. The tolerance, then the UVs and the normals on the simplified shell.
        double                 deviation  = 0.0;
        Geometry::DynamicMesh3 simplified = SimplifyToTolerance( mesh, settings.GeometricDeviation, deviation );
        shell.Deviation                   = static_cast<float>( deviation );

        Geometry::AutoUVSettings uv;
        uv.PackingResolution = settings.TextureSize;
        auto unwrapped       = Geometry::AutoUV( simplified, uv );
        if ( !unwrapped )
            return Common::MakeError<Result>( "the auto-UV: " + unwrapped.GetError() );
        shell.Mesh = std::move( unwrapped.ExtractValue().Mesh );
        if ( !shell.Mesh.HasAttributes() )
            shell.Mesh.EnableAttributes();
        Geometry::DynamicMeshNormalOverlay* normals = shell.Mesh.Attributes()->PrimaryNormals();
        if ( normals == nullptr )
            return Common::MakeError<Result>( "the shell has no normal layer to split" );
        // Smooth = no edge is sharper than 180 degrees, so nothing splits.
        const double opening =
             settings.NormalMode == HLODApproximationNormals::HardEdges ? settings.HardNormalAngle : 180.0;
        Geometry::MeshNormals::InitializeOverlayTopologyFromOpeningAngle( &shell.Mesh, normals, opening );
        Geometry::MeshNormals::QuickRecomputeOverlayNormals( shell.Mesh );
        shell.Triangles = shell.Mesh.TriangleCount();
        return Common::MakeSuccess( std::move( shell ) );
    }

    std::string HLODApproximationKey( const Rules::HLODMeshRequest& request )
    {
        KeyHash hash;
        if ( request.Approximation.has_value() )
        {
            const HLODApproximationSettings& s = *request.Approximation;
            hash.Plain( s.WorldSpaceApproximationAccuracy );
            hash.Plain( s.ClampVoxelDimension );
            hash.Plain( s.WindingThreshold );
            hash.Plain( static_cast<uint8_t>( s.FillGaps ) );
            hash.Plain( s.GapDistance );
            hash.Plain( s.GeometricDeviation );
            hash.Plain( s.TextureSize );
            hash.Plain( s.CaptureResolution );
            hash.Plain( static_cast<int32_t>( s.NormalMode ) );
            hash.Plain( s.HardNormalAngle );
        }
        for ( const Assets::InstancedStaticMeshComponentSer& part : request.Parts )
        {
            hash.Text( part.MeshGuid.value_or( std::string() ) );
            hash.Text( part.MeshPath.value_or( std::string() ) );
            hash.Plain( part.Primitive.has_value() ? static_cast<int32_t>( *part.Primitive ) : -1 );
            for ( const std::string& guid : part.MaterialGuids.value_or( std::vector<std::string>{} ) )
                hash.Text( guid );
            for ( const std::string& path : part.MaterialPaths.value_or( std::vector<std::string>{} ) )
                hash.Text( path );
            for ( const std::array<float, 16>& world :
                  part.InstanceTransforms.value_or( std::vector<std::array<float, 16>>{} ) )
                hash.Bytes( world.data(), sizeof( float ) * world.size() );
        }
        return std::format( "{:016x}", hash.Value );
    }

    HLODCaptureView HLODCaptureViewFacing( const glm::vec3& forward, const glm::vec3& centre, float radius )
    {
        // The camera frame as LandscapeHLODBake builds it: right, up, backward (= -forward), right-handed.
        const glm::vec3 backward = -forward;
        const glm::vec3 upHint =
             std::abs( forward.y ) > 0.99f ? glm::vec3( 0.0f, 0.0f, -1.0f ) : glm::vec3( 0.0f, 1.0f, 0.0f );
        HLODCaptureView view;
        view.Forward    = forward;
        view.Right      = glm::normalize( glm::cross( upHint, backward ) );
        view.Up         = glm::cross( backward, view.Right );
        view.Eye        = centre - forward * ( radius + 1.0f );
        view.HalfExtent = radius;
        view.NearCm     = 0.5f;
        view.FarCm      = 2.0f * radius + 2.0f;
        return view;
    }

    std::vector<HLODCaptureView> HLODCaptureViews( const glm::vec3& boundsMin, const glm::vec3& boundsMax )
    {
        const glm::vec3 centre = ( boundsMin + boundsMax ) * 0.5f;
        // One centimetre beyond the bounding sphere, so nothing of the cell touches the near plane or the edge.
        const float     radius = glm::length( boundsMax - boundsMin ) * 0.5f + 1.0f;
        std::vector<glm::vec3> directions = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
                                              { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        for ( const float x : { -1.0f, 1.0f } )
            for ( const float y : { -1.0f, 1.0f } )
                for ( const float z : { -1.0f, 1.0f } )
                    directions.push_back( glm::normalize( glm::vec3( x, y, z ) ) );
        std::vector<HLODCaptureView> views;
        views.reserve( directions.size() );
        for ( const glm::vec3& forward : directions )
            views.push_back( HLODCaptureViewFacing( forward, centre, radius ) );
        return views;
    }

    Common::ResultStr<HLODBakedTextures> BakeApproximationTextures( const Geometry::RenderMeshData&   shell,
                                                                    const Geometry::DynamicMesh3&     source,
                                                                    std::span<const HLODCapturePhoto> photos,
                                                                    const HLODApproximationSettings&  settings )
    {
        using Result = HLODBakedTextures;
        if ( photos.empty() )
            return Common::MakeError<Result>( "no capture photos to bake from" );
        for ( const HLODCapturePhoto& photo : photos )
        {
            const std::size_t pixels = static_cast<std::size_t>( photo.Width ) * photo.Height;
            if ( pixels == 0 || photo.BaseColorMetallic.size() != pixels * 4 ||
                 photo.NormalRoughness.size() != pixels || photo.Occlusion.size() != pixels )
                return Common::MakeError<Result>(
                     std::format( "a {}x{} capture photo holds {} / {} / {} values", photo.Width, photo.Height,
                                  photo.BaseColorMetallic.size(), photo.NormalRoughness.size(),
                                  photo.Occlusion.size() ) );
        }
        if ( source.TriangleCount() == 0 )
            return Common::MakeError<Result>( "the source mesh holds no triangles to see" );

        const Geometry::DynamicMeshAABBTree3 sourceTree( &source );
        // A photo sees the shell's point when its ray meets the source this close to it: the shell is within the
        // voxel size of the wrap, the closure and the simplification tolerance of the source surface.
        const float accept = settings.WorldSpaceApproximationAccuracy +
                             ( settings.FillGaps ? settings.GapDistance : 0.0f ) + settings.GeometricDeviation;

        HLODBakedTextures out;
        out.Size                 = static_cast<uint32_t>( settings.TextureSize );
        const std::size_t texels = static_cast<std::size_t>( out.Size ) * out.Size;
        out.BaseColor.assign( texels * 4, 0 );
        out.Normal.assign( texels * 4, 0 );
        out.ORM.assign( texels * 4, 0 );
        std::vector<uint8_t> covered( texels, 0 );

        const float side = static_cast<float>( out.Size );
        const auto  edge = []( const glm::vec2& a, const glm::vec2& b, const glm::vec2& p )
        { return ( b.x - a.x ) * ( p.y - a.y ) - ( b.y - a.y ) * ( p.x - a.x ); };
        std::vector<std::pair<float, std::size_t>> order;
        order.reserve( photos.size() );

        for ( const Desert::Submesh& submesh : shell.Submeshes )
            for ( uint32_t t = submesh.IndexOffset / 3; t < ( submesh.IndexOffset + submesh.IndexCount ) / 3; ++t )
            {
                const Desert::Index&  triangle = shell.Indices[t];
                const Desert::Vertex* v[3]     = { &shell.Vertices[submesh.VertexOffset + triangle.V1],
                                                   &shell.Vertices[submesh.VertexOffset + triangle.V2],
                                                   &shell.Vertices[submesh.VertexOffset + triangle.V3] };
                const glm::vec2       p[3]     = { v[0]->TexCoord * side, v[1]->TexCoord * side,
                                                   v[2]->TexCoord * side };
                const float           area     = edge( p[0], p[1], p[2] );
                if ( std::abs( area ) < kFlatEpsilon )
                    continue;
                const long c0 = std::max( 0L, std::lround( std::floor( std::min( { p[0].x, p[1].x, p[2].x } ) - 0.5f ) ) );
                const long c1 = std::min( static_cast<long>( out.Size ) - 1,
                                          std::lround( std::ceil( std::max( { p[0].x, p[1].x, p[2].x } ) - 0.5f ) ) );
                const long r0 = std::max( 0L, std::lround( std::floor( std::min( { p[0].y, p[1].y, p[2].y } ) - 0.5f ) ) );
                const long r1 = std::min( static_cast<long>( out.Size ) - 1,
                                          std::lround( std::ceil( std::max( { p[0].y, p[1].y, p[2].y } ) - 0.5f ) ) );
                for ( long r = r0; r <= r1; ++r )
                    for ( long c = c0; c <= c1; ++c )
                    {
                        const glm::vec2 q( static_cast<float>( c ) + 0.5f, static_cast<float>( r ) + 0.5f );
                        const float     w0 = edge( p[1], p[2], q ) / area;
                        const float     w1 = edge( p[2], p[0], q ) / area;
                        const float     w2 = 1.0f - w0 - w1;
                        if ( w0 < -kFlatEpsilon || w1 < -kFlatEpsilon || w2 < -kFlatEpsilon )
                            continue;
                        const std::size_t texel = static_cast<std::size_t>( r ) * out.Size + c;
                        if ( covered[texel] != 0 )
                            continue; // a shared edge's texel: the first triangle already took it
                        const glm::vec3 point = w0 * v[0]->Position + w1 * v[1]->Position + w2 * v[2]->Position;
                        const glm::vec3 normalSum = w0 * v[0]->Normal + w1 * v[1]->Normal + w2 * v[2]->Normal;
                        if ( glm::dot( normalSum, normalSum ) < kFlatEpsilon )
                            continue;
                        const glm::vec3 normal = glm::normalize( normalSum );

                        // The photos facing the point, most squarely first; the first that sees it wins.
                        order.clear();
                        for ( std::size_t i = 0; i < photos.size(); ++i )
                        {
                            const float facing = -glm::dot( normal, photos[i].View.Forward );
                            if ( facing > 0.0f )
                                order.emplace_back( facing, i );
                        }
                        std::sort( order.begin(), order.end(),
                                   []( const auto& a, const auto& b ) { return a.first > b.first; } );
                        for ( const auto& candidate : order )
                        {
                            const HLODCapturePhoto& photo = photos[candidate.second];
                            const HLODCaptureView&  view  = photo.View;
                            const glm::vec3         local = point - view.Eye;
                            const float             x     = glm::dot( local, view.Right ) / view.HalfExtent;
                            const float             y     = glm::dot( local, view.Up ) / view.HalfExtent;
                            const float             depth = glm::dot( local, view.Forward );
                            if ( std::abs( x ) >= 1.0f || std::abs( y ) >= 1.0f || depth <= view.NearCm ||
                                 depth >= view.FarCm )
                                continue;
                            const glm::dvec3 origin = glm::dvec3( point - view.Forward * depth );
                            double           hitT   = 0.0;
                            if ( sourceTree.FindNearestHitTriangle( origin, glm::dvec3( view.Forward ), hitT ) ==
                                 Geometry::DynamicMesh3::InvalidID )
                                continue;
                            if ( std::abs( hitT - static_cast<double>( depth ) ) > static_cast<double>( accept ) )
                                continue; // something else of the cell stands in front of this point here
                            const auto column = std::min( photo.Width - 1, static_cast<uint32_t>( ( x * 0.5f + 0.5f ) *
                                                                                                  static_cast<float>( photo.Width ) ) );
                            const auto row = std::min( photo.Height - 1, static_cast<uint32_t>( ( 0.5f - y * 0.5f ) *
                                                                                               static_cast<float>( photo.Height ) ) );
                            const std::size_t pixel = static_cast<std::size_t>( row ) * photo.Width + column;
                            const glm::vec4&  nr    = photo.NormalRoughness[pixel];
                            const glm::vec3   world( nr.x, nr.y, nr.z );
                            if ( glm::dot( world, world ) < kFlatEpsilon )
                                continue; // the clear colour: nothing drew here
                            std::memcpy( &out.BaseColor[texel * 4], &photo.BaseColorMetallic[pixel * 4], 3 );
                            out.BaseColor[texel * 4 + 3] = 255;
                            out.ORM[texel * 4 + 0]       = UnitToByte( photo.Occlusion[pixel] );
                            out.ORM[texel * 4 + 1]       = UnitToByte( nr.w );
                            out.ORM[texel * 4 + 2]       = photo.BaseColorMetallic[pixel * 4 + 3];
                            out.ORM[texel * 4 + 3]       = 255;
                            // The photo's world normal in the tangent frame the runtime shades this texel with
                            // (Mesh/Surface/Vertex_Static.glslh interpolates T, B, N; SurfaceTypes.glslh applies TBN).
                            const glm::mat3 tbn( w0 * v[0]->Tangent + w1 * v[1]->Tangent + w2 * v[2]->Tangent,
                                                 w0 * v[0]->Bitangent + w1 * v[1]->Bitangent + w2 * v[2]->Bitangent,
                                                 normal );
                            glm::vec3 tangent( 0.0f, 0.0f, 1.0f );
                            if ( std::abs( glm::determinant( tbn ) ) > kFlatEpsilon )
                            {
                                const glm::vec3 n = glm::inverse( tbn ) * glm::normalize( world );
                                if ( glm::dot( n, n ) > kFlatEpsilon )
                                    tangent = glm::normalize( n );
                                if ( tangent.z < kMinTangentZ )
                                    tangent = glm::normalize( glm::vec3( tangent.x, tangent.y, kMinTangentZ ) );
                            }
                            out.Normal[texel * 4 + 0] = UnitToByte( tangent.x * 0.5f + 0.5f );
                            out.Normal[texel * 4 + 1] = UnitToByte( tangent.y * 0.5f + 0.5f );
                            out.Normal[texel * 4 + 2] = UnitToByte( tangent.z * 0.5f + 0.5f );
                            out.Normal[texel * 4 + 3] = 255;
                            covered[texel]            = 1;
                            ++out.CapturedTexels;
                            break;
                        }
                    }
            }
        if ( out.CapturedTexels == 0 )
            return Common::MakeError<Result>(
                 std::format( "none of the {} photos sees the shell's surface (is the source in the bake's scene?)",
                              photos.size() ) );

        // The gutter and the unseen texels: each pass gives an empty texel the mean of its taken neighbours.
        const auto spread = [&]( std::vector<uint8_t>& map, std::size_t texel, const std::vector<std::size_t>& from )
        {
            for ( int channel = 0; channel < 4; ++channel )
            {
                uint32_t sum = 0;
                for ( const std::size_t n : from )
                    sum += map[n * 4 + channel];
                map[texel * 4 + channel] = static_cast<uint8_t>( sum / from.size() );
            }
        };
        std::vector<std::size_t> from;
        for ( int pass = 0; pass < kGutterPasses; ++pass )
        {
            std::vector<uint8_t> next = covered;
            for ( uint32_t r = 0; r < out.Size; ++r )
                for ( uint32_t c = 0; c < out.Size; ++c )
                {
                    const std::size_t texel = static_cast<std::size_t>( r ) * out.Size + c;
                    if ( covered[texel] != 0 )
                        continue;
                    from.clear();
                    for ( int dr = -1; dr <= 1; ++dr )
                        for ( int dc = -1; dc <= 1; ++dc )
                        {
                            const long nr = static_cast<long>( r ) + dr;
                            const long nc = static_cast<long>( c ) + dc;
                            if ( nr < 0 || nc < 0 || nr >= static_cast<long>( out.Size ) ||
                                 nc >= static_cast<long>( out.Size ) )
                                continue;
                            const std::size_t n = static_cast<std::size_t>( nr ) * out.Size + nc;
                            if ( covered[n] != 0 )
                                from.push_back( n );
                        }
                    if ( from.empty() )
                        continue;
                    spread( out.BaseColor, texel, from );
                    spread( out.Normal, texel, from );
                    spread( out.ORM, texel, from );
                    next[texel] = 1;
                    ++out.FilledTexels;
                }
            covered = std::move( next );
        }
        // Texels farther than the gutter from any island are never sampled; they keep a flat normal.
        for ( std::size_t texel = 0; texel < texels; ++texel )
            if ( covered[texel] == 0 )
            {
                out.Normal[texel * 4 + 0] = 128;
                out.Normal[texel * 4 + 1] = 128;
                out.Normal[texel * 4 + 2] = 255;
                out.Normal[texel * 4 + 3] = 255;
                out.BaseColor[texel * 4 + 3] = 255;
                out.ORM[texel * 4 + 3]       = 255;
            }
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Core
