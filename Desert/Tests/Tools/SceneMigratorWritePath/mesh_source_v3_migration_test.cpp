// SKEL-TREE: the MSAS SRCE 2 -> 3 step (SceneMigration.cpp MigrateMeshSourceToV3). A skinned source's bone
// signature becomes its skeleton's GUID and a header dependency; a static source changes its version only. The
// SRCE 2 input is the engine's own SRCE 3 file taken back to version 2 (GUID -> signature, dependency dropped), so
// the raise is judged against the bytes the engine writes for the same asset today: byte for byte.
#include <SceneMigration.hpp>

#include <Engine/Animation/SkeletonReference.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>

#include <Common/Content/AssetEnvelope.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    namespace CC = Common::Content;
    using namespace Desert;

    const CC::AssetGuid kMaterial{ 0x1111222233334444ULL, 0x5555666677778888ULL };
    const CC::AssetGuid kSkeleton{ 0x5EE1E70110000000ULL, 0x00000000000000A1ULL };
    const CC::AssetGuid kOtherSkeleton{ 0x5EE1E70120000000ULL, 0x00000000000000B2ULL };
    constexpr uint64_t  kSignature      = 0x0B0E5161A7E00001ULL;
    constexpr uint64_t  kOtherSignature = 0x0B0E5161A7E00002ULL;
    constexpr auto      kPath           = "Meshes/Quad.skmesh";

    // The quad of mesh_source_asset_test.cpp, one slot: a well-formed SRCE 3 asset of either kind.
    Assets::MeshSourceAsset MakeQuad( const bool skinned )
    {
        Assets::MeshSourceAsset a;
        a.Kind                         = skinned ? CC::ContentKind::SkinnedMesh : CC::ContentKind::StaticMesh;
        a.Guid                         = { 0xA5A5A5A5A5A5A5A5ULL, 0x0123456789ABCDEFULL };
        a.Name                         = "Quad";
        a.Import.SourceFile            = "assets:Meshes/Quad.fbx";
        a.Import.SourceHash            = 0xFEEDFACECAFEBEEFULL;
        a.Import.Settings.UniformScale = 1.0f;
        auto& m                        = a.Source.Models.emplace_back().Mesh;
        m.Positions            = { -50.f, 0.f, -25.f, 50.f, 0.f, -25.f, 50.f, 10.f, 25.f, -50.f, 0.f, 25.f };
        m.Triangles            = { 0, 1, 2, 0, 2, 3 };
        m.PolyGroups           = { 7, 7 };
        m.MaterialIds          = { 0, 0 };
        m.Normals              = Geometry::EditMeshOverlaySer{ { 0.f, 1.f, 0.f }, { 0, 0, 0, 0, 0, 0 } };
        m.UVs                  = { { { 0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.f, 1.f }, { 0, 1, 2, 0, 2, 3 } } };
        a.Source.MaterialSlots = { { "Body", kMaterial } };
        if ( skinned )
            a.Source.Skin = Assets::MeshSkin{ kSkeleton, { "root", "arm" }, { { 0, 0, 1.f }, { 2, 1, 1.f } } };
        return a;
    }

    // Bytes as the std::string the migration step hands back.
    std::string TextOf( const std::vector<std::byte>& bytes )
    {
        std::string text( bytes.size(), '\0' );
        std::ranges::transform( bytes, text.begin(), []( const std::byte b ) { return static_cast<char>( b ); } );
        return text;
    }

    std::string Encode( const Assets::MeshSourceAsset& asset )
    {
        const auto encoded = Assets::EncodeMeshSourceAsset( asset );
        EXPECT_TRUE( encoded.IsSuccess() ) << encoded.GetError();
        if ( !encoded.IsSuccess() )
            return {};
        return TextOf( encoded.GetValue() );
    }

    std::array<std::byte, 16> GuidBytes( const CC::AssetGuid& guid )
    {
        std::array<std::byte, 16> out{};
        std::memcpy( out.data(), &guid.Hi, 8 );
        std::memcpy( out.data() + 8, &guid.Lo, 8 );
        return out;
    }

    // The SRCE 2 file of `v3`: version 2, and for a skin the skeleton GUID replaced by `signature` and dropped
    // from the header's dependencies - what the engine wrote before SKEL-TREE.
    std::string ToSourceV2( const std::string& v3, const bool skinned )
    {
        auto read = CC::ReadAssetEnvelope( std::as_bytes( std::span( v3.data(), v3.size() ) ),
                                           Assets::MeshAssetHeaderReadContext() );
        EXPECT_TRUE( read.IsSuccess() ) << read.GetError();
        if ( !read.IsSuccess() )
            return {};
        CC::AssetEnvelope e      = read.ExtractValue();
        const auto        source = std::find_if( e.Sections.begin(), e.Sections.end(),
                                                 []( const auto& s ) { return s.Tag == CC::EnvelopeSection::Source; } );
        EXPECT_NE( source, e.Sections.end() );
        if ( source == e.Sections.end() )
            return {};
        std::vector<std::byte>& bytes   = source->Bytes;
        uint32_t                version = 0;
        std::memcpy( &version, bytes.data(), 4 );
        EXPECT_EQ( version, 3u ) << "the engine no longer writes SRCE 3: this test raises to it";
        const uint32_t two = 2u;
        std::memcpy( bytes.data(), &two, 4 );
        if ( skinned )
        {
            const auto guid = GuidBytes( kSkeleton );
            const auto at   = std::search( bytes.begin(), bytes.end(), guid.begin(), guid.end() );
            EXPECT_NE( at, bytes.end() ) << "the SRCE 3 skin does not state the skeleton GUID";
            if ( at == bytes.end() )
                return {};
            EXPECT_EQ( std::search( at + 1, bytes.end(), guid.begin(), guid.end() ), bytes.end() );
            std::array<std::byte, 8> signature{};
            std::memcpy( signature.data(), &kSignature, 8 );
            const auto offset = at - bytes.begin();
            bytes.erase( at, at + 16 );
            bytes.insert( bytes.begin() + offset, signature.begin(), signature.end() );
            std::erase( e.Asset.Dependencies, kSkeleton );
        }
        const auto written = CC::WriteAssetEnvelope( e );
        EXPECT_TRUE( written.IsSuccess() ) << written.GetError();
        if ( !written.IsSuccess() )
            return {};
        return TextOf( written.GetValue() );
    }

    std::vector<Animation::SkeletonCandidate> Candidates()
    {
        return { { kOtherSkeleton, kOtherSignature, "Rigs/Other.skeleton" },
                 { kSkeleton, kSignature, "Rigs/Quad.skeleton" } };
    }
} // namespace

// A static source: only the SRCE version changes, and the result is the engine's own SRCE 3 bytes.
// Mutation: SceneMigration.cpp MigrateMeshSourceToV3 `const uint32_t three = 3u;` -> 2u => the raised file does
// not read (DecodeMeshSourceAsset refuses version 2) => red here.
TEST( MeshSourceV3Migration, AStaticSourceChangesItsVersionOnly )
{
    const std::string v3 = Encode( MakeQuad( false ) );
    const std::string v2 = ToSourceV2( v3, false );
    ASSERT_FALSE( v2.empty() );
    ASSERT_NE( v2, v3 );

    const auto raised = Migration::MigrateMeshSourceToV3( kPath, v2, Candidates() );
    ASSERT_TRUE( raised.IsSuccess() ) << raised.GetError();
    EXPECT_EQ( raised.GetValue(), v3 ) << "the raised static source is not the bytes the engine writes for it";
}

// A skinned source: the one candidate with its signature becomes the skin's Skeleton and the header's dependency
// after the material - byte for byte what the engine writes for the same asset naming that skeleton.
// Mutation: SceneMigration.cpp MigrateMeshSourceToV3 without `e.Asset.Dependencies.push_back( skeleton )` => the
// engine refuses the raised header's dependencies => red here.
TEST( MeshSourceV3Migration, ASkinnedSourceNamesTheSkeletonItsSignatureMatches )
{
    const std::string v3 = Encode( MakeQuad( true ) );
    const std::string v2 = ToSourceV2( v3, true );
    ASSERT_FALSE( v2.empty() );

    const auto raised = Migration::MigrateMeshSourceToV3( kPath, v2, Candidates() );
    ASSERT_TRUE( raised.IsSuccess() ) << raised.GetError();
    EXPECT_EQ( raised.GetValue(), v3 ) << "the raised skinned source is not the bytes the engine writes for it";

    const auto decoded = Assets::DecodeMeshSourceAsset(
         std::as_bytes( std::span( raised.GetValue().data(), raised.GetValue().size() ) ) );
    ASSERT_TRUE( decoded.IsSuccess() ) << decoded.GetError();
    const auto& skin = decoded.GetValue().Source.Skin;
    if ( !skin.has_value() )
        FAIL() << "the raised skinned source states no skin";
    EXPECT_EQ( skin->Skeleton, kSkeleton );
}

// No candidate, or two with the signature: refused naming the source (and every matching skeleton) - never the
// first of several, never a skeleton made up.
TEST( MeshSourceV3Migration, NoneOrTwoMatchingSkeletonsAreRefusedByPath )
{
    const std::string v2 = ToSourceV2( Encode( MakeQuad( true ) ), true );
    ASSERT_FALSE( v2.empty() );

    const std::vector<Animation::SkeletonCandidate> none = {
         { kOtherSkeleton, kOtherSignature, "Rigs/Other.skeleton" } };
    const auto missing = Migration::MigrateMeshSourceToV3( kPath, v2, none );
    ASSERT_FALSE( missing.IsSuccess() ) << "a skin whose signature no skeleton states was raised";
    EXPECT_NE( missing.GetError().find( kPath ), std::string::npos ) << missing.GetError();

    const std::vector<Animation::SkeletonCandidate> two       = { { kSkeleton, kSignature, "Rigs/Quad.skeleton" },
                                                                  { kOtherSkeleton, kSignature, "Rigs/Twin.skeleton" } };
    const auto                                      ambiguous = Migration::MigrateMeshSourceToV3( kPath, v2, two );
    ASSERT_FALSE( ambiguous.IsSuccess() ) << "an ambiguous signature was resolved to one of its skeletons";
    EXPECT_NE( ambiguous.GetError().find( "Rigs/Quad.skeleton" ), std::string::npos ) << ambiguous.GetError();
    EXPECT_NE( ambiguous.GetError().find( "Rigs/Twin.skeleton" ), std::string::npos ) << ambiguous.GetError();
}

// A source already at SRCE 3 is not this step's input: refused, naming the version it states.
TEST( MeshSourceV3Migration, ASourceNotAtVersionTwoIsRefused )
{
    const std::string v3     = Encode( MakeQuad( false ) );
    const auto        raised = Migration::MigrateMeshSourceToV3( kPath, v3, Candidates() );
    ASSERT_FALSE( raised.IsSuccess() ) << "an SRCE 3 source was raised again";
    EXPECT_NE( raised.GetError().find( "version 3" ), std::string::npos ) << raised.GetError();
}
