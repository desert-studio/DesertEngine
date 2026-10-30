#include "DynamicMeshXformOperations.hpp"
#include "DynamicMeshSelection.hpp" // AllTriangleIds

#include "MeshRegionOperation.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/matrix.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace Desert::Geometry
{
    namespace
    {
        // Which attribute layers a result carries (MergeMeshes: the ones every part has). NormalLayers is
        // DynamicMesh3's count: 1 = normals, 3 = normals + tangent + bitangent (a tangent space).
        struct LayerSet
        {
            bool Attributes   = false;
            bool Groups       = false;
            int  NormalLayers = 0;
            bool Colors       = false;
            bool Material     = false;
            int  UVLayers     = 0;
        };

        LayerSet LayersOf( const DynamicMesh3& mesh )
        {
            LayerSet out;
            out.Groups = mesh.HasTriangleGroups();
            if ( const DynamicMeshAttributeSet* a = mesh.Attributes() )
            {
                out.Attributes   = true;
                out.NormalLayers = a->NumNormalLayers();
                out.Colors       = a->HasPrimaryColors();
                out.Material     = a->HasMaterialID();
                out.UVLayers     = a->NumUVLayers();
            }
            return out;
        }

        void EnableLayers( DynamicMesh3& mesh, const LayerSet& layers )
        {
            if ( layers.Groups )
                mesh.EnableTriangleGroups();
            if ( !layers.Attributes )
                return;
            mesh.EnableAttributes();
            DynamicMeshAttributeSet* a = mesh.Attributes();
            a->SetNumUVLayers( layers.UVLayers );
            a->SetNumNormalLayers( layers.NormalLayers );
            if ( layers.Colors )
                a->EnablePrimaryColors();
            if ( layers.Material )
                a->EnableMaterialID();
        }

        // The linear part of an affine map, and what the attribute rules need from it.
        struct LinearMap
        {
            glm::mat3 Linear{ 1.0f };
            glm::mat3 NormalMatrix{ 1.0f }; // inverse transpose
            float     Det       = 1.0f;
            bool      Conformal = true; // a rotation (or reflection) times a uniform scale
        };

        Common::ResultStr<LinearMap> Analyse( const glm::mat4& m, const char* what )
        {
            constexpr float kAffine = 1e-6f;
            if ( std::abs( m[0][3] ) > kAffine || std::abs( m[1][3] ) > kAffine || std::abs( m[2][3] ) > kAffine ||
                 std::abs( m[3][3] - 1.0f ) > kAffine )
                return Common::MakeFormattedError<LinearMap>(
                     "{}: the transform is not affine (bottom row {:.6f} {:.6f} {:.6f} {:.6f})", what, m[0][3],
                     m[1][3], m[2][3], m[3][3] );
            LinearMap out;
            out.Linear    = glm::mat3( m );
            out.Det       = glm::determinant( out.Linear );
            const float s = std::max(
                 { glm::length( out.Linear[0] ), glm::length( out.Linear[1] ), glm::length( out.Linear[2] ) } );
            if ( !std::isfinite( out.Det ) || !( std::abs( out.Det ) > 1e-9f * s * s * s ) )
                return Common::MakeFormattedError<LinearMap>(
                     "{}: the transform is singular (determinant {:.3g}, largest axis {:.3g}) - it would flatten "
                     "the mesh",
                     what, out.Det, s );
            out.NormalMatrix     = glm::transpose( glm::inverse( out.Linear ) );
            const glm::mat3 gram = glm::transpose( out.Linear ) * out.Linear;
            const float     mean = ( gram[0][0] + gram[1][1] + gram[2][2] ) / 3.0f;
            for ( int i = 0; i < 3; ++i )
                for ( int j = 0; j < 3; ++j )
                    if ( std::abs( gram[i][j] - ( i == j ? mean : 0.0f ) ) > 1e-4f * mean )
                        out.Conformal = false;
            return Common::MakeSuccess( out );
        }

        glm::vec3 SafeNormalize( const glm::vec3& v )
        {
            const float length = glm::length( v );
            return length > 0.0f ? v / length : v;
        }

        // One overlay of one part: every source element used by the copied triangles gets one element in the
        // result (so seams stay seams), valued through `map`; a reversed triangle takes its elements reversed.
        template <typename Overlay, typename Map>
        Common::BoolResultStr CopyLayer( const Overlay* from, Overlay* to,
                                         const std::vector<std::pair<int, int>>& triangles, bool reverse, Map map,
                                         const char* name )
        {
            if ( from == nullptr || to == nullptr )
                return Common::MakeSuccess( true );
            std::vector<int> elements( static_cast<size_t>( from->MaxElementID() ), DynamicMesh3::InvalidID );
            for ( const auto& [source, target] : triangles )
            {
                if ( !from->IsSetTriangle( source ) )
                    continue;
                const Index3i in = from->GetTriangle( source );
                Index3i       out;
                for ( int j = 0; j < 3; ++j )
                {
                    int& mapped = elements[static_cast<size_t>( in[j] )];
                    if ( mapped == DynamicMesh3::InvalidID )
                        mapped = to->AppendElement( map( from->GetElement( in[j] ) ) );
                    out[j] = mapped;
                }
                if ( reverse )
                    std::swap( out[1], out[2] );
                if ( const MeshResult r = to->SetTriangle( target, out ); r != MeshResult::Ok )
                    return Common::MakeFormattedError<bool>( "the {} of triangle {} could not be carried ({})",
                                                             name, source, static_cast<int>( r ) );
            }
            return Common::MakeSuccess( true );
        }

        // Appends `triangles` of `source`, mapped by `transform`, to `result` (whose layers are already set up;
        // a layer `result` lacks is not carried). Polygroups are shifted by `groupOffset`, materials remapped.
        // A non-conformal map leaves the tangent space uncopied and sets `tangentsRebuilt`: the caller rebuilds
        // it once every part is in (RecomputeTangentSpace).
        Common::BoolResultStr AppendPart( DynamicMesh3& result, const DynamicMesh3& source,
                                          const glm::mat4& transform, const LinearMap& linear,
                                          const std::vector<int>& triangles, int groupOffset,
                                          const std::vector<int>& materialRemap, bool& tangentsRebuilt )
        {
            const bool       reverse = linear.Det < 0.0f;
            const glm::dmat4 toResult( transform );
            std::vector<int> vertices( static_cast<size_t>( source.MaxVertexID() ), DynamicMesh3::InvalidID );
            std::vector<std::pair<int, int>> made;
            made.reserve( triangles.size() );
            const DynamicMeshAttributeSet*      from         = source.Attributes();
            DynamicMeshAttributeSet*            to           = result.Attributes();
            const DynamicMeshMaterialAttribute* fromMaterial = from ? from->GetMaterialID() : nullptr;
            DynamicMeshMaterialAttribute*       toMaterial   = to ? to->GetMaterialID() : nullptr;
            for ( const int t : triangles )
            {
                const Index3i corners = source.GetTriangle( t );
                Index3i       mapped;
                for ( int j = 0; j < 3; ++j )
                {
                    int& v = vertices[static_cast<size_t>( corners[j] )];
                    if ( v == DynamicMesh3::InvalidID )
                        v = result.AppendVertex(
                             glm::dvec3( toResult * glm::dvec4( source.GetVertex( corners[j] ), 1.0 ) ) );
                    mapped[j] = v;
                }
                if ( reverse )
                    std::swap( mapped[1], mapped[2] );
                const int group = source.HasTriangleGroups() ? source.GetTriangleGroup( t ) + groupOffset : 0;
                const int added = result.AppendTriangle( mapped, group );
                if ( added < 0 )
                    return Common::MakeFormattedError<bool>( "triangle {} could not be added (error {})", t,
                                                             added );
                if ( toMaterial != nullptr )
                {
                    int32_t material = fromMaterial ? fromMaterial->GetValue( t ) : 0;
                    if ( !materialRemap.empty() )
                    {
                        if ( material < 0 || material >= static_cast<int>( materialRemap.size() ) )
                            return Common::MakeFormattedError<bool>(
                                 "triangle {} has material {}, outside the part's {} material slots", t, material,
                                 materialRemap.size() );
                        material = materialRemap[static_cast<size_t>( material )];
                    }
                    toMaterial->SetValue( added, &material );
                }
                made.emplace_back( t, added );
            }
            if ( from == nullptr || to == nullptr )
                return Common::MakeSuccess( true );

            auto      same2  = []( const glm::vec2& v ) { return v; };
            auto      same4  = []( const glm::vec4& v ) { return v; };
            auto      normal = [&]( const glm::vec3& n ) { return SafeNormalize( linear.NormalMatrix * n ); };
            auto      frame  = [&]( const glm::vec3& v ) { return SafeNormalize( linear.Linear * v ); };
            const int normalLayers = std::min( from->NumNormalLayers(), to->NumNormalLayers() );
            for ( int layer = 0; layer < normalLayers; ++layer )
            {
                if ( layer > 0 && !linear.Conformal )
                {
                    // A non-conformal map is rebuilt by the caller instead: copying first would be overwritten.
                    tangentsRebuilt = true;
                    continue;
                }
                auto r = layer == 0 ? CopyLayer( from->GetNormalLayer( 0 ), to->GetNormalLayer( 0 ), made, reverse,
                                                 normal, "normal" )
                                    : CopyLayer( from->GetNormalLayer( layer ), to->GetNormalLayer( layer ), made,
                                                 reverse, frame, layer == 1 ? "tangent" : "bitangent" );
                if ( !r.IsSuccess() )
                    return r;
            }
            if ( from->HasPrimaryColors() && to->HasPrimaryColors() )
                if ( auto r =
                          CopyLayer( from->PrimaryColors(), to->PrimaryColors(), made, reverse, same4, "colour" );
                     !r.IsSuccess() )
                    return r;
            const int uvLayers = std::min( from->NumUVLayers(), to->NumUVLayers() );
            for ( int layer = 0; layer < uvLayers; ++layer )
                if ( auto r = CopyLayer( from->GetUVLayer( layer ), to->GetUVLayer( layer ), made, reverse, same2,
                                         "UV" );
                     !r.IsSuccess() )
                    return r;
            return Common::MakeSuccess( true );
        }

        Common::BoolResultStr FinishTangents( DynamicMesh3& mesh, bool rebuilt, const char* what )
        {
            if ( !rebuilt )
                return Common::MakeSuccess( true );
            if ( auto r = RecomputeTangentSpace( mesh, what ); !r.IsSuccess() )
                return Common::MakeFormattedError<bool>( "the tangents could not be rebuilt: {}", r.GetError() );
            return Common::MakeSuccess( true );
        }

        int MaxPolyGroup( const DynamicMesh3& mesh )
        {
            int top = -1;
            if ( mesh.HasTriangleGroups() )
                for ( const int t : mesh.TriangleIndicesItr() )
                    top = std::max( top, mesh.GetTriangleGroup( t ) );
            return top;
        }

        glm::vec3 Axis( int index )
        {
            glm::vec3 axis( 0.0f );
            axis[index] = 1.0f;
            return axis;
        }

        bool Finite( const glm::vec3& v )
        {
            return std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z );
        }
    } // namespace

    glm::mat4 Trs::Matrix() const
    {
        return glm::translate( glm::mat4( 1.0f ), Translation ) * glm::mat4_cast( glm::quat( Rotation ) ) *
               glm::scale( glm::mat4( 1.0f ), Scale );
    }

    Common::ResultStr<MeshBox> ComputeMeshBox( const DynamicMesh3& mesh )
    {
        if ( mesh.VertexCount() == 0 )
            return Common::MakeError<MeshBox>( "Bounds: the mesh has no vertex" );
        MeshBox box{ glm::vec3( std::numeric_limits<float>::max() ),
                     glm::vec3( std::numeric_limits<float>::lowest() ) };
        for ( const int v : mesh.VertexIndicesItr() )
        {
            const glm::vec3 p( mesh.GetVertex( v ) );
            box.Min = glm::min( box.Min, p );
            box.Max = glm::max( box.Max, p );
        }
        return Common::MakeSuccess( box );
    }

    Common::ResultStr<TransformedMesh> TransformMesh( const DynamicMesh3& mesh, const glm::mat4& transform )
    {
        auto linear = Analyse( transform, "Transform" );
        if ( !linear.IsSuccess() )
            return Common::MakeError<TransformedMesh>( linear.GetError() );
        TransformedMesh out;
        EnableLayers( out.Mesh, LayersOf( mesh ) );
        if ( auto r = AppendPart( out.Mesh, mesh, transform, linear.GetValue(), AllTriangleIds( mesh ), 0, {},
                                  out.TangentsRebuilt );
             !r.IsSuccess() )
            return Common::MakeFormattedError<TransformedMesh>( "Transform: {}", r.GetError() );
        if ( auto r = FinishTangents( out.Mesh, out.TangentsRebuilt, "Transform" ); !r.IsSuccess() )
            return Common::MakeFormattedError<TransformedMesh>( "Transform: {}", r.GetError() );
        out.WindingReversed = linear.GetValue().Det < 0.0f;
        return Common::MakeSuccess( std::move( out ) );
    }

    // ── Edit Pivot ─────────────────────────────────────────────────────────────────────────────────────

    const char* ToString( PivotLocation location )
    {
        switch ( location )
        {
            case PivotLocation::BoundsCenter:
                return "Bounds Center";
            case PivotLocation::BoundsBase:
                return "Bounds Base";
            case PivotLocation::WorldOrigin:
                return "World Origin";
            case PivotLocation::WorldPoint:
                return "World Point";
        }
        return "?";
    }

    Common::ResultStr<glm::vec3> ResolvePivot( const DynamicMesh3& mesh, PivotLocation location,
                                               const glm::mat4& meshToWorld, const glm::vec3& worldPoint )
    {
        if ( location == PivotLocation::BoundsCenter || location == PivotLocation::BoundsBase )
        {
            auto box = ComputeMeshBox( mesh );
            if ( !box.IsSuccess() )
                return Common::MakeFormattedError<glm::vec3>( "Edit Pivot ({}): {}", ToString( location ),
                                                              box.GetError() );
            glm::vec3 pivot = 0.5f * ( box.GetValue().Min + box.GetValue().Max );
            if ( location == PivotLocation::BoundsBase )
                pivot.y = box.GetValue().Min.y;
            return Common::MakeSuccess( pivot );
        }
        auto linear = Analyse( meshToWorld, "Edit Pivot" );
        if ( !linear.IsSuccess() )
            return Common::MakeFormattedError<glm::vec3>( "Edit Pivot ({}): the entity's world transform: {}",
                                                          ToString( location ), linear.GetError() );
        const glm::vec3 target = location == PivotLocation::WorldOrigin ? glm::vec3( 0.0f ) : worldPoint;
        return Common::MakeSuccess( glm::vec3( glm::inverse( meshToWorld ) * glm::vec4( target, 1.0f ) ) );
    }

    Common::ResultStr<XformOutcome> EditPivot( const DynamicMesh3& mesh, const Trs& local, const glm::vec3& pivot )
    {
        if ( !Finite( pivot ) )
            return Common::MakeError<XformOutcome>( "Edit Pivot: the pivot is not a finite point" );
        if ( local.Scale.x == 0.0f || local.Scale.y == 0.0f || local.Scale.z == 0.0f )
            return Common::MakeFormattedError<XformOutcome>(
                 "Edit Pivot: the entity's scale ({:.4g}, {:.4g}, {:.4g}) has a zero axis", local.Scale.x,
                 local.Scale.y, local.Scale.z );
        auto moved = TransformMesh( mesh, glm::translate( glm::mat4( 1.0f ), -pivot ) );
        if ( !moved.IsSuccess() )
            return Common::MakeFormattedError<XformOutcome>( "Edit Pivot: {}", moved.GetError() );
        XformOutcome out;
        out.Mesh      = std::move( moved.ExtractValue().Mesh );
        out.Transform = local;
        // local * v == translate(T) * RS * v; with v' = v - p that is translate(T + RS p) * RS * v'.
        out.Transform.Translation += glm::mat3_cast( glm::quat( local.Rotation ) ) * ( local.Scale * pivot );
        out.Report = fmt::format( "the pivot moved by ({:.3f}, {:.3f}, {:.3f}) cm in the mesh's space", pivot.x,
                                  pivot.y, pivot.z );
        return Common::MakeSuccess( std::move( out ) );
    }

    // ── Bake Transform ─────────────────────────────────────────────────────────────────────────────────

    Common::ResultStr<XformOutcome> BakeTransform( const DynamicMesh3& mesh, const Trs& local,
                                                   const BakeOptions& options )
    {
        if ( !options.Rotation && !options.Scale && !options.Translation )
            return Common::MakeError<XformOutcome>( "Bake Transform: nothing is chosen to bake" );
        Trs kept = local;
        if ( options.Translation )
            kept.Translation = glm::vec3( 0.0f );
        if ( options.Rotation )
            kept.Rotation = glm::vec3( 0.0f );
        if ( options.Scale )
            kept.Scale = glm::vec3( 1.0f );
        const glm::mat4 before = local.Matrix();
        if ( auto check = Analyse( before, "Bake Transform" ); !check.IsSuccess() )
            return Common::MakeFormattedError<XformOutcome>( "{} (scale {:.4g}, {:.4g}, {:.4g})", check.GetError(),
                                                             local.Scale.x, local.Scale.y, local.Scale.z );
        auto baked = TransformMesh( mesh, glm::inverse( kept.Matrix() ) * before );
        if ( !baked.IsSuccess() )
            return Common::MakeFormattedError<XformOutcome>( "Bake Transform: {}", baked.GetError() );
        XformOutcome out;
        out.Transform       = kept;
        const bool reversed = baked.GetValue().WindingReversed;
        const bool rebuilt  = baked.GetValue().TangentsRebuilt;
        out.Mesh            = std::move( baked.ExtractValue().Mesh );
        out.Report          = fmt::format( "baked{}{}{}{}{}", options.Rotation ? " rotation" : "",
                                  options.Scale ? " scale" : "", options.Translation ? " translation" : "",
                                  reversed ? "; the winding was reversed (negative scale)" : "",
                                  rebuilt ? "; the tangents were rebuilt (non-uniform scale)" : "" );
        return Common::MakeSuccess( std::move( out ) );
    }

    // ── Merge ──────────────────────────────────────────────────────────────────────────────────────────

    Common::ResultStr<MergeOutcome> MergeMeshes( std::span<const MergePart> parts )
    {
        if ( parts.empty() )
            return Common::MakeError<MergeOutcome>( "Merge: no mesh to merge" );
        LayerSet common{ true, true, 3, true, true, INT_MAX };
        LayerSet any;
        for ( size_t i = 0; i < parts.size(); ++i )
        {
            if ( parts[i].Mesh.TriangleCount() == 0 )
                return Common::MakeFormattedError<MergeOutcome>( "Merge: part {} has no triangle", i );
            const LayerSet has = LayersOf( parts[i].Mesh );
            common.Attributes &= has.Attributes;
            common.Groups &= has.Groups;
            common.NormalLayers = std::min( common.NormalLayers, has.NormalLayers );
            common.Colors &= has.Colors;
            common.Material &= has.Material;
            common.UVLayers = std::min( common.UVLayers, has.UVLayers );
            any.Groups |= has.Groups;
            any.NormalLayers = std::max( any.NormalLayers, has.NormalLayers );
            any.Colors |= has.Colors;
            any.Material |= has.Material;
            any.UVLayers = std::max( any.UVLayers, has.UVLayers );
        }
        MergeOutcome out;
        EnableLayers( out.Mesh, common );
        int  groupOffset = 0;
        bool rebuilt     = false;
        for ( size_t i = 0; i < parts.size(); ++i )
        {
            auto linear = Analyse( parts[i].ToResult, "Merge" );
            if ( !linear.IsSuccess() )
                return Common::MakeFormattedError<MergeOutcome>( "{} (part {})", linear.GetError(), i );
            if ( auto r =
                      AppendPart( out.Mesh, parts[i].Mesh, parts[i].ToResult, linear.GetValue(),
                                  AllTriangleIds( parts[i].Mesh ), groupOffset, parts[i].MaterialRemap, rebuilt );
                 !r.IsSuccess() )
                return Common::MakeFormattedError<MergeOutcome>( "Merge: part {}: {}", i, r.GetError() );
            groupOffset += MaxPolyGroup( parts[i].Mesh ) + 1;
        }
        if ( auto r = FinishTangents( out.Mesh, rebuilt, "Merge" ); !r.IsSuccess() )
            return Common::MakeFormattedError<MergeOutcome>( "Merge: {}", r.GetError() );
        std::vector<std::string> dropped;
        if ( any.Groups && !common.Groups )
            dropped.emplace_back( "polygroups" );
        if ( any.NormalLayers > 0 && common.NormalLayers == 0 )
            dropped.emplace_back( "normals" );
        if ( any.NormalLayers > 1 && common.NormalLayers <= 1 )
            dropped.emplace_back( "tangents" );
        if ( any.Material && !common.Material )
            dropped.emplace_back( "material IDs" );
        if ( any.Colors && !common.Colors )
            dropped.emplace_back( "colours" );
        if ( any.UVLayers > common.UVLayers )
            dropped.push_back( fmt::format( "UV layers {}..{}", common.UVLayers, any.UVLayers - 1 ) );
        if ( !dropped.empty() )
        {
            out.Report = "dropped (not on every part): ";
            for ( size_t i = 0; i < dropped.size(); ++i )
                out.Report += ( i == 0 ? "" : ", " ) + dropped[i];
        }
        return Common::MakeSuccess( std::move( out ) );
    }

    // ── Split ──────────────────────────────────────────────────────────────────────────────────────────

    const char* ToString( SplitMethod method )
    {
        switch ( method )
        {
            case SplitMethod::ConnectedComponents:
                return "Connected Components";
            case SplitMethod::PolyGroups:
                return "PolyGroups";
        }
        return "?";
    }

    Common::ResultStr<std::vector<DynamicMesh3>> SplitMesh( const DynamicMesh3& mesh, SplitMethod method )
    {
        if ( mesh.TriangleCount() == 0 )
            return Common::MakeError<std::vector<DynamicMesh3>>( "Split: the mesh has no triangle" );
        // A key per triangle: its union-find root (components) or its polygroup.
        std::vector<int> parent( static_cast<size_t>( mesh.MaxTriangleID() ) );
        std::iota( parent.begin(), parent.end(), 0 );
        auto find = [&]( int t )
        {
            while ( parent[static_cast<size_t>( t )] != t )
                t = parent[static_cast<size_t>( t )] =
                     parent[static_cast<size_t>( parent[static_cast<size_t>( t )] )];
            return t;
        };
        if ( method == SplitMethod::ConnectedComponents )
        {
            for ( const int e : mesh.EdgeIndicesItr() )
            {
                const Index2i pair = mesh.GetEdgeT( e );
                if ( pair.A == DynamicMesh3::InvalidID || pair.B == DynamicMesh3::InvalidID )
                    continue;
                const int a = find( pair.A ), b = find( pair.B );
                if ( a != b )
                    parent[static_cast<size_t>( std::max( a, b ) )] = std::min( a, b );
            }
        }
        auto keyOf = [&]( int t )
        {
            return method == SplitMethod::ConnectedComponents ? find( t )
                   : mesh.HasTriangleGroups()                 ? mesh.GetTriangleGroup( t )
                                                              : 0;
        };

        std::vector<int>              keys;
        std::vector<std::vector<int>> groups;
        for ( const int t : mesh.TriangleIndicesItr() )
        {
            const int key = keyOf( t );
            auto      it  = std::find( keys.begin(), keys.end(), key );
            if ( it == keys.end() )
            {
                keys.push_back( key );
                groups.emplace_back();
                it = keys.end() - 1;
            }
            groups[static_cast<size_t>( it - keys.begin() )].push_back( t );
        }
        if ( groups.size() < 2 )
            return Common::MakeFormattedError<std::vector<DynamicMesh3>>(
                 "Split ({}): the mesh is one part already", ToString( method ) );
        std::vector<DynamicMesh3> out( groups.size() );
        const LinearMap           identity;
        for ( size_t i = 0; i < groups.size(); ++i )
        {
            bool unused = false;
            EnableLayers( out[i], LayersOf( mesh ) );
            if ( auto r = AppendPart( out[i], mesh, glm::mat4( 1.0f ), identity, groups[i], 0, {}, unused );
                 !r.IsSuccess() )
                return Common::MakeFormattedError<std::vector<DynamicMesh3>>( "Split: part {}: {}", i,
                                                                              r.GetError() );
        }
        return Common::MakeSuccess( std::move( out ) );
    }

    // ── Pattern ────────────────────────────────────────────────────────────────────────────────────────

    const char* ToString( PatternShape shape )
    {
        switch ( shape )
        {
            case PatternShape::Line:
                return "Line";
            case PatternShape::Grid:
                return "Grid";
            case PatternShape::Circle:
                return "Circle";
        }
        return "?";
    }

    Common::ResultStr<std::vector<glm::mat4>> PatternTransforms( const PatternSettings& s )
    {
        using Out        = std::vector<glm::mat4>;
        const char* name = ToString( s.Shape );
        if ( s.AxisA < 0 || s.AxisA > 2 || ( s.Shape == PatternShape::Grid && ( s.AxisB < 0 || s.AxisB > 2 ) ) )
            return Common::MakeFormattedError<Out>(
                 "Pattern ({}): an axis must be 0 (X), 1 (Y) or 2 (Z), not {}/{}", name, s.AxisA, s.AxisB );
        if ( s.Shape == PatternShape::Grid && s.AxisA == s.AxisB )
            return Common::MakeFormattedError<Out>( "Pattern (Grid): both axes are {}", s.AxisA );
        const int countB = s.Shape == PatternShape::Grid ? s.CountB : 1;
        if ( s.Count < 1 || countB < 1 )
            return Common::MakeFormattedError<Out>( "Pattern ({}): a count must be at least 1, not {} x {}", name,
                                                    s.Count, countB );
        const long long total = static_cast<long long>( s.Count ) * countB;
        if ( total < 2 || total > kMaxPatternCopies )
            return Common::MakeFormattedError<Out>( "Pattern ({}): {} copies - the pattern needs 2 .. {}", name,
                                                    total, kMaxPatternCopies );
        Out out;
        out.reserve( static_cast<size_t>( total ) );
        const glm::vec3 a = Axis( s.AxisA );
        if ( s.Shape == PatternShape::Circle )
        {
            if ( !( s.Radius > 0.0f ) )
                return Common::MakeFormattedError<Out>( "Pattern (Circle): the radius must be > 0, not {}",
                                                        s.Radius );
            if ( s.SweepDegrees == 0.0f || std::abs( s.SweepDegrees ) > 360.0f )
                return Common::MakeFormattedError<Out>(
                     "Pattern (Circle): the sweep must be in [-360, 360] and not "
                     "0, not {}",
                     s.SweepDegrees );
            const bool  ring = std::abs( s.SweepDegrees ) >= 360.0f - 1e-3f;
            const float step = glm::radians( s.SweepDegrees ) / static_cast<float>( ring ? s.Count : s.Count - 1 );
            const glm::vec3 centre = -s.Radius * Axis( ( s.AxisA + 1 ) % 3 );
            for ( int i = 0; i < s.Count; ++i )
            {
                const glm::mat4 turn = glm::rotate( glm::mat4( 1.0f ), step * static_cast<float>( i ), a );
                if ( s.OrientToCircle )
                    out.push_back( glm::translate( glm::mat4( 1.0f ), centre ) * turn *
                                   glm::translate( glm::mat4( 1.0f ), -centre ) );
                else
                    out.push_back( glm::translate( glm::mat4( 1.0f ),
                                                   centre + glm::vec3( turn * glm::vec4( -centre, 0.0f ) ) ) );
            }
            return Common::MakeSuccess( std::move( out ) );
        }
        if ( s.Spacing == 0.0f || ( s.Shape == PatternShape::Grid && s.SpacingB == 0.0f ) )
            return Common::MakeFormattedError<Out>( "Pattern ({}): a zero spacing puts every copy on the source",
                                                    name );
        const glm::vec3 b = s.Shape == PatternShape::Grid ? Axis( s.AxisB ) : glm::vec3( 0.0f );
        for ( int j = 0; j < countB; ++j )
            for ( int i = 0; i < s.Count; ++i )
                out.push_back(
                     glm::translate( glm::mat4( 1.0f ), static_cast<float>( i ) * s.Spacing * a +
                                                             static_cast<float>( j ) * s.SpacingB * b ) );
        return Common::MakeSuccess( std::move( out ) );
    }

    void RefreshFaceNormalsAround( DynamicMesh3& mesh, std::span<const int> movedVertices )
    {
        auto* normals = mesh.HasAttributes() ? mesh.Attributes()->PrimaryNormals() : nullptr;
        if ( !normals )
            return;
        std::vector<int> touched;
        for ( const int vtx : movedVertices )
            for ( const int t : mesh.VtxTrianglesItr( vtx ) )
                touched.push_back( t );
        std::sort( touched.begin(), touched.end() );
        touched.erase( std::unique( touched.begin(), touched.end() ), touched.end() );
        for ( const int t : touched )
            if ( normals->IsSetTriangle( t ) )
            {
                const glm::vec3 n  = glm::vec3( mesh.GetTriNormal( t ) );
                const Index3i   el = normals->GetTriangle( t );
                for ( int j = 0; j < 3; ++j )
                    normals->SetElement( el[j], n );
            }
    }
} // namespace Desert::Geometry
