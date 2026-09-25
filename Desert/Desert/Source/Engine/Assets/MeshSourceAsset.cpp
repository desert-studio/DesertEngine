// Ported from UE 5.8 Engine/Source/Runtime/Engine/Classes/Engine/StaticMesh.h:657 (SourceModels /
// FStaticMeshSourceModel), :1096 (StaticMaterials), :1439 (AssetImportData) and
// Engine/Source/Runtime/Engine/Classes/EditorFramework/AssetImportData.h:14-40 (FAssetImportInfo::FSourceFile),
// adapted: the pattern "editable source + material slots + import provenance inside the asset, render data
// derived" is kept; the storage is AF1's envelope sections instead of UObject properties, FMeshDescription is our
// EditMeshSer written as a fixed little-endian binary image (not FMeshDescription's bulk data), one source model
// instead of one per LOD (authored LODs are the deriver's business, AF4c), FSourceFile's MD5 + timestamp is one
// PakContentHash (freshness is content, never mtime), and the skin keeps named bones + sparse influences in the
// shape of FSkeletalMeshImportData (RefBonesBinary names, Influences) rather than a USkeleton reference.
#include <Engine/Assets/MeshSourceAsset.hpp>

#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <utility>

namespace Desert::Assets
{
    namespace CC = Common::Content;

    namespace
    {
        constexpr uint32_t kImportInfoVersion = 1;
        constexpr uint32_t kSourceVersion     = 1;

        const CC::SubsystemVersion kKnown[] = { { kMeshAssetSubsystemTag, kMeshAssetSubsystemVersion } };

        // The one order every mesh asset's sections are in (see the header).
        constexpr std::array<CC::EnvelopeSection, 3> kSectionOrder = {
             CC::EnvelopeSection::Meta, CC::EnvelopeSection::ImportInfo, CC::EnvelopeSection::Source };

        // ── writing ───────────────────────────────────────────────────────────────────────────────────
        struct Writer
        {
            std::vector<std::byte> Out;

            void U8( const uint8_t v )
            {
                Out.push_back( static_cast<std::byte>( v ) );
            }
            void U32( const uint32_t v )
            {
                for ( int i = 0; i < 4; ++i )
                    Out.push_back( static_cast<std::byte>( ( v >> ( 8 * i ) ) & 0xFFu ) );
            }
            void U64( const uint64_t v )
            {
                for ( int i = 0; i < 8; ++i )
                    Out.push_back( static_cast<std::byte>( ( v >> ( 8 * i ) ) & 0xFFu ) );
            }
            void F32( const float v )
            {
                U32( std::bit_cast<uint32_t>( v ) );
            }
            void I32( const int v )
            {
                U32( std::bit_cast<uint32_t>( static_cast<int32_t>( v ) ) );
            }
            void String( const std::string_view s )
            {
                U32( static_cast<uint32_t>( s.size() ) );
                for ( const char c : s )
                    Out.push_back( static_cast<std::byte>( c ) );
            }
            void Floats( const std::vector<float>& v )
            {
                U32( static_cast<uint32_t>( v.size() ) );
                for ( const float f : v )
                    F32( f );
            }
            void Ints( const std::vector<int>& v )
            {
                U32( static_cast<uint32_t>( v.size() ) );
                for ( const int i : v )
                    I32( i );
            }
            void Overlay( const Geometry::EditMeshOverlaySer& o )
            {
                Floats( o.Values );
                Ints( o.Triangles );
            }
            void OptionalOverlay( const std::optional<Geometry::EditMeshOverlaySer>& o )
            {
                U8( o ? 1 : 0 );
                if ( o )
                    Overlay( *o );
            }
        };

        // ── reading ───────────────────────────────────────────────────────────────────────────────────
        struct Reader
        {
            std::span<const std::byte> Bytes;
            size_t                     At = 0;
            bool                       Ok = true;

            bool Has( const size_t n )
            {
                if ( !Ok || Bytes.size() - At < n )
                    Ok = false;
                return Ok;
            }
            uint64_t Get( const int width )
            {
                if ( !Has( static_cast<size_t>( width ) ) )
                    return 0;
                uint64_t v = 0;
                for ( int i = 0; i < width; ++i )
                    v |= static_cast<uint64_t>( std::to_integer<uint8_t>( Bytes[At + i] ) ) << ( 8 * i );
                At += static_cast<size_t>( width );
                return v;
            }
            uint8_t U8()
            {
                return static_cast<uint8_t>( Get( 1 ) );
            }
            uint32_t U32()
            {
                return static_cast<uint32_t>( Get( 4 ) );
            }
            uint64_t U64()
            {
                return Get( 8 );
            }
            float F32()
            {
                return std::bit_cast<float>( U32() );
            }
            int I32()
            {
                return static_cast<int>( std::bit_cast<int32_t>( U32() ) );
            }
            // A count whose elements take at least `elementBytes` each: refused before anything is allocated
            // when the rest of the section cannot hold that many.
            size_t Count( const size_t elementBytes )
            {
                const auto n = static_cast<size_t>( U32() );
                if ( Ok && elementBytes != 0 && n > ( Bytes.size() - At ) / elementBytes )
                    Ok = false;
                return Ok ? n : 0;
            }
            std::string String()
            {
                const size_t n = Count( 1 );
                std::string  s;
                s.reserve( n );
                for ( size_t i = 0; i < n; ++i )
                    s.push_back( static_cast<char>( std::to_integer<uint8_t>( Bytes[At + i] ) ) );
                At += n;
                return s;
            }
            std::vector<float> Floats()
            {
                std::vector<float> v( Count( 4 ) );
                for ( float& f : v )
                    f = F32();
                return v;
            }
            std::vector<int> Ints()
            {
                std::vector<int> v( Count( 4 ) );
                for ( int& i : v )
                    i = I32();
                return v;
            }
            Geometry::EditMeshOverlaySer Overlay()
            {
                Geometry::EditMeshOverlaySer o;
                o.Values    = Floats();
                o.Triangles = Ints();
                return o;
            }
            std::optional<Geometry::EditMeshOverlaySer> OptionalOverlay()
            {
                const uint8_t present = U8();
                if ( Ok && present > 1 )
                    Ok = false;
                if ( !Ok || present == 0 )
                    return std::nullopt;
                return Overlay();
            }
        };

        // ── validation (shared by encode and decode: a file this build writes is a file it reads) ─────
        Common::BoolResultStr ValidateOverlay( const Geometry::EditMeshOverlaySer& o, const std::string_view layer,
                                               const size_t components, const size_t triangleCount )
        {
            if ( o.Values.size() % components != 0 )
                return Common::MakeFormattedError<bool>(
                     "mesh source {} layer has {} floats, not a multiple of {}", layer, o.Values.size(),
                     components );
            if ( o.Triangles.size() != triangleCount * 3 )
                return Common::MakeFormattedError<bool>(
                     "mesh source {} layer binds {} corners, the mesh has {} triangles ({} corners)", layer,
                     o.Triangles.size(), triangleCount, triangleCount * 3 );
            const auto elements = static_cast<int64_t>( o.Values.size() / components );
            for ( size_t t = 0; t < triangleCount; ++t )
            {
                const int  a     = o.Triangles[t * 3];
                const int  b     = o.Triangles[t * 3 + 1];
                const int  c     = o.Triangles[t * 3 + 2];
                const bool unset = a == -1 && b == -1 && c == -1;
                if ( unset )
                    continue;
                for ( const int e : { a, b, c } )
                    if ( e < 0 || e >= elements )
                        return Common::MakeFormattedError<bool>(
                             "mesh source {} layer: triangle {} binds element {}, the layer has {} (a triangle is "
                             "unset only with -1 on all three corners)",
                             layer, t, e, elements );
            }
            return Common::MakeSuccess( true );
        }

        Common::BoolResultStr ValidateSource( const MeshSourceData& s, const CC::ContentKind kind )
        {
            const Geometry::EditMeshSer& m = s.Mesh;
            if ( m.Positions.size() % 3 != 0 || m.Triangles.size() % 3 != 0 )
                return Common::MakeFormattedError<bool>(
                     "mesh source has {} position floats and {} triangle indices; both must be multiples of 3",
                     m.Positions.size(), m.Triangles.size() );
            if ( m.Triangles.empty() )
                return Common::MakeError<bool>( "mesh source has no triangles" );
            for ( const float p : m.Positions )
                if ( !std::isfinite( p ) )
                    return Common::MakeError<bool>( "mesh source has a non-finite position" );
            const size_t triangles = m.Triangles.size() / 3;
            const auto   vertices  = static_cast<int64_t>( m.Positions.size() / 3 );
            for ( size_t i = 0; i < m.Triangles.size(); ++i )
                if ( m.Triangles[i] < 0 || m.Triangles[i] >= vertices )
                    return Common::MakeFormattedError<bool>(
                         "mesh source triangle {} names vertex {}, the mesh has {}", i / 3, m.Triangles[i],
                         vertices );
            if ( m.PolyGroups.size() != triangles || m.MaterialIds.size() != triangles )
                return Common::MakeFormattedError<bool>(
                     "mesh source has {} triangles but {} polygroups and {} material ids (one each per triangle)",
                     triangles, m.PolyGroups.size(), m.MaterialIds.size() );
            for ( size_t t = 0; t < triangles; ++t )
                if ( m.MaterialIds[t] < 0 || static_cast<size_t>( m.MaterialIds[t] ) >= s.MaterialSlots.size() )
                    return Common::MakeFormattedError<bool>(
                         "mesh source triangle {} uses material slot {}, the asset has {} slots", t,
                         m.MaterialIds[t], s.MaterialSlots.size() );
            if ( m.Normals )
                if ( auto r = ValidateOverlay( *m.Normals, "normal", 3, triangles ); !r.IsSuccess() )
                    return r;
            if ( m.Tangents )
                if ( auto r = ValidateOverlay( *m.Tangents, "tangent", 4, triangles ); !r.IsSuccess() )
                    return r;
            if ( m.Colors )
                if ( auto r = ValidateOverlay( *m.Colors, "colour", 4, triangles ); !r.IsSuccess() )
                    return r;
            for ( const Geometry::EditMeshOverlaySer& uv : m.UVs )
                if ( auto r = ValidateOverlay( uv, "UV", 2, triangles ); !r.IsSuccess() )
                    return r;

            const bool skinned = kind == CC::ContentKind::SkinnedMesh;
            if ( skinned != s.Skin.has_value() )
                return Common::MakeFormattedError<bool>( "a {} asset {} a skin", CC::KindName( kind ),
                                                         skinned ? "needs" : "cannot carry" );
            if ( s.Skin )
                for ( const MeshSkinInfluence& inf : s.Skin->Influences )
                {
                    if ( static_cast<int64_t>( inf.Vertex ) >= vertices || inf.Bone >= s.Skin->BoneNames.size() )
                        return Common::MakeFormattedError<bool>( "mesh skin influence names vertex {} / bone {}; "
                                                                 "the mesh has {} vertices and {} bones",
                                                                 inf.Vertex, inf.Bone, vertices,
                                                                 s.Skin->BoneNames.size() );
                    if ( !std::isfinite( inf.Weight ) || inf.Weight < 0.0f || inf.Weight > 1.0f )
                        return Common::MakeFormattedError<bool>(
                             "mesh skin weight {} on vertex {} is not in [0, 1]", inf.Weight, inf.Vertex );
                }
            return Common::MakeSuccess( true );
        }

        Common::BoolResultStr ValidateImport( const MeshImportInfo& info )
        {
            const MeshImportSettings& s = info.Settings;
            if ( !std::isfinite( s.UniformScale ) || s.UniformScale <= 0.0f )
                return Common::MakeFormattedError<bool>( "mesh import scale {} is not a finite positive number",
                                                         s.UniformScale );
            const bool named = !info.SourceFile.empty() && info.SourceHash != 0;
            const bool blank = info.SourceFile.empty() && info.SourceHash == 0;
            if ( info.Provenance == MeshSourceProvenance::Imported && !named )
                return Common::MakeError<bool>(
                     "an imported mesh names its source file and that file's hash; one is missing" );
            if ( info.Provenance == MeshSourceProvenance::Recovered && !blank )
                return Common::MakeError<bool>(
                     "a recovered mesh has no source file, but this one names a file or a hash" );
            return Common::MakeSuccess( true );
        }

        // ── IMPT ──────────────────────────────────────────────────────────────────────────────────────
        // Enums travel by NAME: reordering an enum must not silently change what every asset says.
        std::vector<std::byte> EncodeImportInfo( const MeshImportInfo& info )
        {
            Writer w;
            w.U32( kImportInfoVersion );
            w.String( MeshSourceProvenanceName( info.Provenance ) );
            w.String( info.SourceFile );
            w.U64( info.SourceHash );
            w.F32( info.Settings.UniformScale );
            w.String( MeshSourceUpAxisName( info.Settings.UpAxis ) );
            w.String( MeshLodPolicyName( info.Settings.LodPolicy ) );
            return std::move( w.Out );
        }

        Common::ResultStr<MeshImportInfo> DecodeImportInfo( std::span<const std::byte> bytes )
        {
            Reader     r{ bytes };
            const auto version = r.U32();
            if ( r.Ok && version != kImportInfoVersion )
                return Common::MakeFormattedError<MeshImportInfo>(
                     "mesh ImportInfo version {} is not the {} this build reads", version, kImportInfoVersion );
            const std::string provenance = r.String();
            MeshImportInfo    info;
            info.SourceFile             = r.String();
            info.SourceHash             = r.U64();
            info.Settings.UniformScale  = r.F32();
            const std::string axis      = r.String();
            const std::string lodPolicy = r.String();
            if ( !r.Ok || r.At != bytes.size() )
                return Common::MakeError<MeshImportInfo>( "mesh ImportInfo is truncated or has trailing bytes" );
            const auto p = MeshSourceProvenanceFromName( provenance );
            const auto a = MeshSourceUpAxisFromName( axis );
            const auto l = MeshLodPolicyFromName( lodPolicy );
            if ( !p || !a || !l )
                return Common::MakeFormattedError<MeshImportInfo>(
                     "mesh ImportInfo names provenance '{}', up axis '{}', LOD policy '{}'; one is unknown to "
                     "this "
                     "build",
                     provenance, axis, lodPolicy );
            info.Provenance         = *p;
            info.Settings.UpAxis    = *a;
            info.Settings.LodPolicy = *l;
            return Common::MakeSuccess( std::move( info ) );
        }

        // ── SRCE ──────────────────────────────────────────────────────────────────────────────────────
        std::vector<std::byte> EncodeSource( const MeshSourceData& s )
        {
            Writer                       w;
            const Geometry::EditMeshSer& m = s.Mesh;
            w.U32( kSourceVersion );
            w.Floats( m.Positions );
            w.Ints( m.Triangles );
            w.Ints( m.PolyGroups );
            w.Ints( m.MaterialIds );
            w.OptionalOverlay( m.Normals );
            w.OptionalOverlay( m.Tangents );
            w.OptionalOverlay( m.Colors );
            w.U32( static_cast<uint32_t>( m.UVs.size() ) );
            for ( const Geometry::EditMeshOverlaySer& uv : m.UVs )
                w.Overlay( uv );
            w.U32( static_cast<uint32_t>( s.MaterialSlots.size() ) );
            for ( const MeshMaterialSlot& slot : s.MaterialSlots )
            {
                w.String( slot.Name );
                w.U64( slot.Material.Hi );
                w.U64( slot.Material.Lo );
            }
            w.U8( s.Skin ? 1 : 0 );
            if ( s.Skin )
            {
                w.U64( s.Skin->SkeletonSignature );
                w.U32( static_cast<uint32_t>( s.Skin->BoneNames.size() ) );
                for ( const std::string& bone : s.Skin->BoneNames )
                    w.String( bone );
                w.U32( static_cast<uint32_t>( s.Skin->Influences.size() ) );
                for ( const MeshSkinInfluence& inf : s.Skin->Influences )
                {
                    w.U32( inf.Vertex );
                    w.U32( inf.Bone );
                    w.F32( inf.Weight );
                }
            }
            return std::move( w.Out );
        }

        Common::ResultStr<MeshSourceData> DecodeSource( std::span<const std::byte> bytes )
        {
            Reader     r{ bytes };
            const auto version = r.U32();
            if ( r.Ok && version != kSourceVersion )
                return Common::MakeFormattedError<MeshSourceData>(
                     "mesh Source version {} is not the {} this build reads", version, kSourceVersion );
            MeshSourceData         s;
            Geometry::EditMeshSer& m = s.Mesh;
            m.Positions              = r.Floats();
            m.Triangles              = r.Ints();
            m.PolyGroups             = r.Ints();
            m.MaterialIds            = r.Ints();
            m.Normals                = r.OptionalOverlay();
            m.Tangents               = r.OptionalOverlay();
            m.Colors                 = r.OptionalOverlay();
            m.UVs.resize( r.Count( 8 ) );
            for ( Geometry::EditMeshOverlaySer& uv : m.UVs )
                uv = r.Overlay();
            s.MaterialSlots.resize( r.Count( 20 ) );
            for ( MeshMaterialSlot& slot : s.MaterialSlots )
            {
                slot.Name        = r.String();
                slot.Material.Hi = r.U64();
                slot.Material.Lo = r.U64();
            }
            const uint8_t skinned = r.U8();
            if ( r.Ok && skinned > 1 )
                return Common::MakeFormattedError<MeshSourceData>( "mesh Source skin flag is {}, not 0 or 1",
                                                                   skinned );
            if ( skinned == 1 )
            {
                MeshSkin skin;
                skin.SkeletonSignature = r.U64();
                skin.BoneNames.resize( r.Count( 4 ) );
                for ( std::string& bone : skin.BoneNames )
                    bone = r.String();
                skin.Influences.resize( r.Count( 12 ) );
                for ( MeshSkinInfluence& inf : skin.Influences )
                {
                    inf.Vertex = r.U32();
                    inf.Bone   = r.U32();
                    inf.Weight = r.F32();
                }
                s.Skin = std::move( skin );
            }
            if ( !r.Ok || r.At != bytes.size() )
                return Common::MakeError<MeshSourceData>( "mesh Source is truncated or has trailing bytes" );
            return Common::MakeSuccess( std::move( s ) );
        }

        bool IsMeshKind( const CC::ContentKind kind )
        {
            return kind == CC::ContentKind::StaticMesh || kind == CC::ContentKind::SkinnedMesh;
        }
    } // namespace

    CC::AssetHeaderReadContext MeshAssetHeaderReadContext()
    {
        return CC::AssetHeaderReadContext{ kKnown };
    }

    std::vector<CC::AssetGuid> MeshSourceDependencies( const MeshSourceData& source )
    {
        std::vector<CC::AssetGuid> deps;
        for ( const MeshMaterialSlot& slot : source.MaterialSlots )
            if ( !slot.Material.IsNull() && std::find( deps.begin(), deps.end(), slot.Material ) == deps.end() )
                deps.push_back( slot.Material );
        return deps;
    }

    std::optional<CC::EnvelopeBounds> MeshSourceBounds( const Geometry::EditMeshSer& mesh )
    {
        if ( mesh.Positions.size() < 3 )
            return std::nullopt;
        CC::EnvelopeBounds b;
        for ( size_t axis = 0; axis < 3; ++axis )
            b.Lo[axis] = b.Hi[axis] = mesh.Positions[axis];
        for ( size_t i = 0; i + 2 < mesh.Positions.size(); i += 3 )
            for ( size_t axis = 0; axis < 3; ++axis )
            {
                b.Lo[axis] = std::min( b.Lo[axis], mesh.Positions[i + axis] );
                b.Hi[axis] = std::max( b.Hi[axis], mesh.Positions[i + axis] );
            }
        return b;
    }

    Common::ResultStr<std::vector<std::byte>> EncodeMeshSourceAsset( const MeshSourceAsset& asset )
    {
        if ( !IsMeshKind( asset.Kind ) )
            return Common::MakeFormattedError<std::vector<std::byte>>(
                 "a mesh asset is kind StaticMesh or SkinnedMesh, not '{}'", CC::KindName( asset.Kind ) );
        if ( auto r = ValidateImport( asset.Import ); !r.IsSuccess() )
            return Common::MakeError<std::vector<std::byte>>( r.GetError() );
        if ( auto r = ValidateSource( asset.Source, asset.Kind ); !r.IsSuccess() )
            return Common::MakeError<std::vector<std::byte>>( r.GetError() );

        CC::AssetEnvelope envelope;
        envelope.Asset.Kind         = asset.Kind;
        envelope.Asset.Guid         = asset.Guid;
        envelope.Asset.Subsystems   = { kKnown[0] };
        envelope.Asset.Dependencies = MeshSourceDependencies( asset.Source );
        CC::EnvelopeMeta meta;
        meta.Name   = asset.Name;
        meta.Bounds = MeshSourceBounds( asset.Source.Mesh );
        envelope.Sections.push_back(
             { CC::EnvelopeSection::Meta, CC::EnvelopeCodec::Stored, CC::EncodeEnvelopeMeta( meta ) } );
        envelope.Sections.push_back(
             { CC::EnvelopeSection::ImportInfo, CC::EnvelopeCodec::Stored, EncodeImportInfo( asset.Import ) } );
        envelope.Sections.push_back(
             { CC::EnvelopeSection::Source, CC::EnvelopeCodec::Stored, EncodeSource( asset.Source ) } );
        return CC::WriteAssetEnvelope( envelope );
    }

    Common::ResultStr<MeshSourceAsset> DecodeMeshSourceAsset( std::span<const std::byte> file )
    {
        auto envelope = CC::ReadAssetEnvelope( file, MeshAssetHeaderReadContext() );
        if ( !envelope.IsSuccess() )
            return Common::MakeError<MeshSourceAsset>( envelope.GetError() );
        const CC::AssetEnvelope& e = envelope.GetValue();
        if ( !IsMeshKind( e.Asset.Kind ) )
            return Common::MakeFormattedError<MeshSourceAsset>( "the envelope is kind '{}', not a mesh",
                                                                CC::KindName( e.Asset.Kind ) );
        if ( std::find( e.Asset.Subsystems.begin(), e.Asset.Subsystems.end(), kKnown[0] ) ==
             e.Asset.Subsystems.end() )
            return Common::MakeError<MeshSourceAsset>( "the mesh envelope is not stamped with the MSAS layout" );
        bool inOrder = e.Sections.size() == kSectionOrder.size();
        for ( size_t i = 0; inOrder && i < kSectionOrder.size(); ++i )
            inOrder = e.Sections[i].Tag == kSectionOrder[i];
        if ( !inOrder )
        {
            std::string seen;
            for ( const CC::EnvelopeSectionData& s : e.Sections )
                seen += ( seen.empty() ? "" : "," ) + CC::FourCCToString( static_cast<uint32_t>( s.Tag ) );
            return Common::MakeFormattedError<MeshSourceAsset>(
                 "a mesh asset has sections META,IMPT,SRCE in that order; this one has [{}]", seen );
        }

        auto meta   = CC::DecodeEnvelopeMeta( e.Sections[0].Bytes );
        auto import = DecodeImportInfo( e.Sections[1].Bytes );
        auto source = DecodeSource( e.Sections[2].Bytes );
        if ( !meta.IsSuccess() )
            return Common::MakeError<MeshSourceAsset>( meta.GetError() );
        if ( !import.IsSuccess() )
            return Common::MakeError<MeshSourceAsset>( import.GetError() );
        if ( !source.IsSuccess() )
            return Common::MakeError<MeshSourceAsset>( source.GetError() );

        MeshSourceAsset asset;
        asset.Kind   = e.Asset.Kind;
        asset.Guid   = e.Asset.Guid;
        asset.Name   = meta.GetValue().Name;
        asset.Import = import.GetValue();
        asset.Source = source.GetValue();
        if ( auto r = ValidateImport( asset.Import ); !r.IsSuccess() )
            return Common::MakeError<MeshSourceAsset>( r.GetError() );
        if ( auto r = ValidateSource( asset.Source, asset.Kind ); !r.IsSuccess() )
            return Common::MakeError<MeshSourceAsset>( r.GetError() );
        // Meta bounds and header dependencies are CACHES of SRCE for readers that skip the body; a file whose
        // cache disagrees with its source is refused, not silently re-derived.
        if ( !meta.GetValue().Tags.empty() )
            return Common::MakeError<MeshSourceAsset>( "a mesh asset's Meta carries no tags" );
        if ( meta.GetValue().Bounds != MeshSourceBounds( asset.Source.Mesh ) )
            return Common::MakeError<MeshSourceAsset>(
                 "the mesh asset's Meta bounds are not the bounds of its source" );
        if ( e.Asset.Dependencies != MeshSourceDependencies( asset.Source ) )
            return Common::MakeFormattedError<MeshSourceAsset>(
                 "the mesh asset's header names {} dependencies; its material slots name {}",
                 e.Asset.Dependencies.size(), MeshSourceDependencies( asset.Source ).size() );
        return Common::MakeSuccess( std::move( asset ) );
    }

    Common::ResultStr<MeshSourceAsset> ReadMeshSourceAssetFile( const std::filesystem::path& file )
    {
        const auto bytes = Common::Utils::FileSystem::ReadFileContent( file );
        if ( !bytes.IsSuccess() )
            return Common::MakeError<MeshSourceAsset>( bytes.GetError() );
        auto result = DecodeMeshSourceAsset( std::as_bytes( std::span<const char>( bytes.GetValue() ) ) );
        if ( !result.IsSuccess() )
            return Common::MakeFormattedError<MeshSourceAsset>( "'{}': {}", file.string(), result.GetError() );
        return result;
    }

    Common::BoolResultStr WriteMeshSourceAssetFile( const std::filesystem::path& file,
                                                    const MeshSourceAsset&       asset )
    {
        const auto bytes = EncodeMeshSourceAsset( asset );
        if ( !bytes.IsSuccess() )
            return Common::MakeError<bool>( bytes.GetError() );
        // Atomic: an asset is the whole file or the previous one, never half of the new one.
        return Common::Utils::FileSystem::WriteBytesToFileAtomic( file, bytes.GetValue() );
    }

    std::string_view MeshSourceUpAxisName( const MeshSourceUpAxis axis )
    {
        switch ( axis )
        {
            case MeshSourceUpAxis::FromFile:
                return "FromFile";
            case MeshSourceUpAxis::Y:
                return "Y";
            case MeshSourceUpAxis::Z:
                return "Z";
        }
        return "?";
    }
    std::optional<MeshSourceUpAxis> MeshSourceUpAxisFromName( const std::string_view name )
    {
        for ( const auto a : { MeshSourceUpAxis::FromFile, MeshSourceUpAxis::Y, MeshSourceUpAxis::Z } )
            if ( MeshSourceUpAxisName( a ) == name )
                return a;
        return std::nullopt;
    }

    std::string_view MeshLodPolicyName( const MeshLodPolicy policy )
    {
        switch ( policy )
        {
            case MeshLodPolicy::Generate:
                return "Generate";
            case MeshLodPolicy::None:
                return "None";
        }
        return "?";
    }
    std::optional<MeshLodPolicy> MeshLodPolicyFromName( const std::string_view name )
    {
        for ( const auto p : { MeshLodPolicy::Generate, MeshLodPolicy::None } )
            if ( MeshLodPolicyName( p ) == name )
                return p;
        return std::nullopt;
    }

    std::string_view MeshSourceProvenanceName( const MeshSourceProvenance provenance )
    {
        switch ( provenance )
        {
            case MeshSourceProvenance::Imported:
                return "Imported";
            case MeshSourceProvenance::Recovered:
                return "Recovered";
        }
        return "?";
    }
    std::optional<MeshSourceProvenance> MeshSourceProvenanceFromName( const std::string_view name )
    {
        for ( const auto p : { MeshSourceProvenance::Imported, MeshSourceProvenance::Recovered } )
            if ( MeshSourceProvenanceName( p ) == name )
                return p;
        return std::nullopt;
    }
} // namespace Desert::Assets
