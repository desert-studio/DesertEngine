// THM1l-b16: A SKINNED FILE THROUGH THE WHOLE IMPORT, on a mock the suite writes itself (no external asset in
// the repository; UE's import automation does the same on reference files).
//
// The mock is CesiumMan's shape in miniature: a non-bone node "Z_UP" turning the scene -90 degrees about X sits
// above a two-bone rig and the skinned mesh; the triangle's raw vertices stand along +Z and the inverse bind
// matrices do not state the turn, so the character drawn stands along +Y only because of the node above the
// rig. One clip turns the root bone, its keys stated in the root's own node frame (local to Z_UP).
//
// What is pinned:
//  - the stored rest shape is the drawn one: height along Y, nothing along Z (AssimpImporter.cpp BakeBindPose);
//  - the clip's frame 0 puts the root bone where its bind does (AssimpImporter.cpp FoldNonBoneAncestors): a
//    clip that drops Z_UP lays the character down the moment it plays;
//  - the rig and its clip are written before the mesh that names them (ImportManager.cpp
//    CreateAssetsFromImport, by the registry's write journal), and the record states Kind = SkinnedMesh.

#include <Editor/Import/ImportManager.hpp>
#include <Editor/Import/MaterialAdoption.hpp>
#include <Editor/Import/MaterialImportContract.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/ContentKinds.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>

#include "../../TestSupport/assets_sandbox.hpp"
#include "../../TestSupport/derived_data_sandbox.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace Desert;
namespace Ser = Assets::Serialization;

namespace
{
    template <typename T>
    void Put( std::vector<unsigned char>& bytes, const T& value )
    {
        const auto* p = reinterpret_cast<const unsigned char*>( &value );
        bytes.insert( bytes.end(), p, p + sizeof( T ) );
    }

    std::string Base64( const std::vector<unsigned char>& bytes )
    {
        static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string        out;
        for ( std::size_t i = 0; i < bytes.size(); i += 3 )
        {
            const uint32_t b0 = bytes[i];
            const uint32_t b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0;
            const uint32_t b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0;
            const uint32_t n  = ( b0 << 16 ) | ( b1 << 8 ) | b2;
            out += alphabet[( n >> 18 ) & 63];
            out += alphabet[( n >> 12 ) & 63];
            out += i + 1 < bytes.size() ? alphabet[( n >> 6 ) & 63] : '=';
            out += i + 2 < bytes.size() ? alphabet[n & 63] : '=';
        }
        return out;
    }

    // A 1x1 PNG: the mock's base colour is EMBEDDED in the file, as Fox.glb's and CesiumMan.glb's are.
    constexpr const char* OnePixelPng =
         "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";

    // The buffer: positions (0), joints (36), weights (48), inverse binds (96), key times (224), key turns (232).
    std::string MockGltf()
    {
        std::vector<unsigned char> b;
        const float                positions[9] = { -0.5f, 0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f };
        for ( float f : positions )
            Put( b, f );
        const unsigned char joints[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0 };
        for ( unsigned char j : joints )
            Put( b, j );
        for ( int v = 0; v < 3; ++v )
            for ( float w : { 1.0f, 0.0f, 0.0f, 0.0f } )
                Put( b, w );
        // Column-major, as glTF states them: the root's bind is the identity, the tip's is 1 unit up +Z.
        for ( int bone = 0; bone < 2; ++bone )
            for ( int i = 0; i < 16; ++i )
                Put( b, i == 14 ? ( bone == 1 ? -1.0f : 0.0f ) : ( i % 5 == 0 ? 1.0f : 0.0f ) );
        for ( float t : { 0.0f, 1.0f } )
            Put( b, t );
        const float s = std::sin( glm::radians( 22.5f ) ), c = std::cos( glm::radians( 22.5f ) );
        for ( float q : { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, s, c } )
            Put( b, q );
        EXPECT_EQ( b.size(), 264u );

        const std::string uri = "data:application/octet-stream;base64," + Base64( b );
        return R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
"nodes":[
 {"name":"Z_UP","matrix":[1,0,0,0, 0,0,-1,0, 0,1,0,0, 0,0,0,1],"children":[1,3]},
 {"name":"Root","children":[2]},
 {"name":"Tip","translation":[0,0,1]},
 {"name":"Body","mesh":0,"skin":0}],
"skins":[{"joints":[1,2],"inverseBindMatrices":3,"skeleton":1}],
"meshes":[{"name":"Body","primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,"WEIGHTS_0":2},"material":0}]}],
"materials":[{"name":"Skin","pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],
"textures":[{"source":0}],
"images":[{"mimeType":"image/png","uri":"data:image/png;base64,)" +
               std::string( OnePixelPng ) + R"("}],
"animations":[{"name":"Turn","samplers":[{"input":4,"output":5,"interpolation":"LINEAR"}],
 "channels":[{"sampler":0,"target":{"node":1,"path":"rotation"}}]}],
"accessors":[
 {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-0.5,0,0],"max":[0.5,0,2]},
 {"bufferView":1,"componentType":5121,"count":3,"type":"VEC4"},
 {"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"},
 {"bufferView":3,"componentType":5126,"count":2,"type":"MAT4"},
 {"bufferView":4,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},
 {"bufferView":5,"componentType":5126,"count":2,"type":"VEC4"}],
"bufferViews":[
 {"buffer":0,"byteOffset":0,"byteLength":36},
 {"buffer":0,"byteOffset":36,"byteLength":12},
 {"buffer":0,"byteOffset":48,"byteLength":48},
 {"buffer":0,"byteOffset":96,"byteLength":128},
 {"buffer":0,"byteOffset":224,"byteLength":8},
 {"buffer":0,"byteOffset":232,"byteLength":32}],
"buffers":[{"byteLength":264,"uri":")" +
               uri + R"("}]})";
    }

    std::string Read( const std::filesystem::path& file )
    {
        const auto content = Common::Utils::FileSystem::ReadFileContent( file );
        EXPECT_TRUE( content.IsSuccess() ) << file.string();
        return content.IsSuccess() ? content.GetValue() : std::string{};
    }

    // The journal serial of the one row of @p kind the import wrote: the last serial after which it still reads
    // as written.
    uint64_t WrittenAt( Common::Content::ContentKind kind, uint64_t before, uint64_t after )
    {
        uint64_t at = 0;
        for ( uint64_t s = before; s < after; ++s )
            if ( !Assets::ContentRegistry::WrittenSince( kind, s ).Rows.empty() )
                at = s + 1;
        return at;
    }

    class SkinnedImport : public ::testing::Test
    {
    protected:
        // The mock's material needs a template to go to, as in the editor after its shaders load: the shipped
        // PBR and Unlit shaders, read by the importer's own reader and published through the same seam the
        // registry uses (run from the tree root, before the sandbox moves the process).
        static void SetUpTestSuite()
        {
            std::vector<Editor::ImportTemplate> shipped;
            for ( const char* file : { "Editor/Resources/Shaders/Programs/PBR/StaticMeshPBR.shader",
                                       "Editor/Resources/Shaders/Programs/Unlit/Unlit.shader" } )
            {
                const auto text = Common::Utils::FileSystem::ReadFileContent( file );
                ASSERT_TRUE( text.IsSuccess() ) << file << " (run from the tree root)";
                auto read = Editor::ReadImportTemplate( text.GetValue(), file );
                ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
                shipped.push_back( std::move( read.GetValue() ) );
            }
            ASSERT_EQ( Editor::ImportManager::PublishImportTemplates( std::move( shipped ) ), 2u );
        }

        void SetUp() override
        {
            Assets::ContentRegistry::ResetForTest();
            std::filesystem::create_directories( m_Source.parent_path() );
            std::ofstream( m_Source, std::ios::binary ) << MockGltf();

            m_Before  = Assets::ContentRegistry::WriteSerial();
            m_Outcome = ImportManager().ImportWithSettings( m_Source, Assets::SourceImportSettings{} );
            m_After   = Assets::ContentRegistry::WriteSerial();
            ASSERT_EQ( m_Outcome.Verdict, Editor::CookVerdict::Cooked );
            ASSERT_EQ( m_Outcome.WrittenMeshes.size(), 1u );
            ASSERT_EQ( m_Outcome.WrittenSkeletons.size(), 1u );
            ASSERT_EQ( m_Outcome.WrittenClips.size(), 1u );
        }
        void TearDown() override
        {
            Assets::ContentRegistry::ResetForTest();
        }

        using ImportManager = Editor::ImportManager;

        // The embedded base colour is cooked as a texture, and a texture cook stores its platform data in the
        // DDC: without a sandboxed cache root the texture import refuses and the whole cook is Incomplete.
        TestSupport::DerivedDataSandbox m_DerivedData{ "SkinnedImport" };
        TestSupport::AssetsSandbox m_Sandbox{ "SkinnedImport", {} };
        std::filesystem::path      m_Source = "Resources/Assets/Mock/Rig.gltf";
        Editor::ImportOutcome      m_Outcome;
        uint64_t                   m_Before = 0;
        uint64_t                   m_After  = 0;
    };
} // namespace

TEST_F( SkinnedImport, TheStoredRestShapeStandsAlongYAsTheNodeAboveTheRigTurnsIt )
{
    const std::string bytes = Read( m_Outcome.WrittenMeshes.front() );
    const auto        mesh  = Ser::ReadMeshAssetData( bytes, m_Outcome.WrittenMeshes.front().string() );
    ASSERT_TRUE( mesh.IsSuccess() ) << mesh.GetError();
    ASSERT_TRUE( mesh.GetValue().IsSkinned );
    ASSERT_FALSE( mesh.GetValue().Submeshes.empty() );

    const auto&     box  = mesh.GetValue().Submeshes.front().BoundingBox;
    const glm::vec3 size = box.Max - box.Min;
    ASSERT_GT( size.x, 0.0f );
    // Raw: 1 wide along X, 2 tall along Z. Drawn (and stored): 2 tall along Y, flat in Z.
    EXPECT_NEAR( size.y / size.x, 2.0f, 1e-3f ) << size.x << " " << size.y << " " << size.z;
    EXPECT_NEAR( size.z / size.x, 0.0f, 1e-3f ) << size.x << " " << size.y << " " << size.z;
    EXPECT_NEAR( box.Min.y, 0.0f, 1e-3f * size.x );
}

TEST_F( SkinnedImport, TheClipsFirstFramePutsTheRootBoneWhereItsBindDoes )
{
    const auto rig = Ser::ReadSkeletonJson( Read( m_Outcome.WrittenSkeletons.front() ) );
    ASSERT_TRUE( rig.IsSuccess() ) << rig.GetError();
    const auto clip = Ser::ReadAnimationJson( Read( m_Outcome.WrittenClips.front() ) );
    ASSERT_TRUE( clip.IsSuccess() ) << clip.GetError();

    const Animation::BoneInfo* root = nullptr;
    for ( const auto& bone : rig.GetValue().Bones )
        if ( bone.Name == "Root" )
            root = &bone;
    ASSERT_NE( root, nullptr );

    const Ser::ChannelData* channel = nullptr;
    for ( const auto& c : clip.GetValue().Channels )
        if ( c.BoneName == "Root" )
            channel = &c;
    ASSERT_NE( channel, nullptr );
    ASSERT_FALSE( channel->Rotations.empty() );

    // The bind states the Z_UP turn (folded into the root's local bind); frame 0 must state the same turn.
    const glm::quat bind  = glm::normalize( glm::quat_cast( glm::mat3( root->LocalBindTransform ) ) );
    const glm::quat first = glm::normalize( channel->Rotations.front().Value );
    EXPECT_NEAR( std::abs( glm::dot( bind, first ) ), 1.0f, 1e-4f )
         << "bind " << bind.w << " " << bind.x << " " << bind.y << " " << bind.z << " / frame 0 " << first.w << " "
         << first.x << " " << first.y << " " << first.z;
    const glm::quat zUp = glm::angleAxis( glm::radians( -90.0f ), glm::vec3( 1.0f, 0.0f, 0.0f ) );
    EXPECT_NEAR( std::abs( glm::dot( bind, zUp ) ), 1.0f, 1e-4f );
}

TEST_F( SkinnedImport, TheRigAndItsClipAreWrittenBeforeTheMeshAndTheRecordSaysSkinnedMesh )
{
    using Common::Content::ContentKind;
    const uint64_t skeleton = WrittenAt( ContentKind::Skeleton, m_Before, m_After );
    const uint64_t clip     = WrittenAt( ContentKind::Animation, m_Before, m_After );
    const uint64_t mesh     = WrittenAt( ContentKind::SkinnedMesh, m_Before, m_After );
    ASSERT_NE( skeleton, 0u );
    ASSERT_NE( clip, 0u );
    ASSERT_NE( mesh, 0u );
    EXPECT_LT( skeleton, mesh );
    EXPECT_LT( clip, mesh );

    const auto kind = Ser::ReadImportRecordKind( m_Source );
    ASSERT_TRUE( kind.IsSuccess() ) << kind.GetError();
    EXPECT_EQ( kind.GetValue(), ContentKind::SkinnedMesh );
}

// THM-FIXJ: Reimport of a clip re-imports its source (UE: UAnimSequence::AssetImportData). `<stem>_<clip>.anim`
// has no inverse, so the clip STATES the source it came from and the bytes' hash; live on Fox_Walk.anim "has no
// import source to reimport from". ImportOptions::ImportSourceOfAsset resolves this statement beside the clip.
TEST_F( SkinnedImport, TheClipNamesTheSourceItWasImportedFrom )
{
    ASSERT_FALSE( m_Outcome.WrittenClips.empty() );
    const auto clip = Ser::ReadAnimationJson( Read( m_Outcome.WrittenClips.front() ) );
    ASSERT_TRUE( clip.IsSuccess() ) << clip.GetError();
    ASSERT_TRUE( clip.GetValue().Import.has_value() );
    EXPECT_EQ( clip.GetValue().Import->Source, m_Source.filename().generic_string() );
    const auto hash = Assets::HashMeshSourceFile( m_Source );
    ASSERT_TRUE( hash.IsSuccess() ) << hash.GetError();
    EXPECT_EQ( clip.GetValue().Import->SourceHash, hash.GetValue() );
}

// THM1l-b19: the texture EMBEDDED in the file ("*0" to assimp, a data-URI image here, a bufferView image in a
// .glb) is written out beside the source as a texture of its own and the material names it. Live on Fox.glb:
// "texture '*0' (type 1) NOT FOUND" and the fox drew white.
TEST_F( SkinnedImport, TheEmbeddedBaseColourIsWrittenBesideTheSource )
{
    const std::filesystem::path extracted = m_Source.parent_path() / ( m_Source.stem().string() + "_0.png" );
    EXPECT_TRUE( std::filesystem::exists( extracted ) ) << extracted.string();
}

// THM1l-b21: THE SKINNED MESH NAMES ITS MATERIAL AS THE STATIC ONE DOES: every .skmesh submesh states the GUID of
// the .demat the same import wrote (the entity takes it through MeshECSSystem::AdoptMeshMaterialSlots). Live on
// Fox.glb the fox drew the grey default although fox_material.demat and Fox_0.detex were written.
TEST_F( SkinnedImport, TheSkinnedMeshNamesTheMaterialTheImportWrote )
{
    const auto mesh = Ser::ReadMeshAssetData( Read( m_Outcome.WrittenMeshes.front() ),
                                              m_Outcome.WrittenMeshes.front().string() );
    ASSERT_TRUE( mesh.IsSuccess() ) << mesh.GetError();
    const std::filesystem::path demat = Editor::MaterialAdoption::MaterialAssetPath( m_Source, "Skin" );
    const auto                  header =
         Common::Content::ReadAssetHeader( demat, Common::Content::AssetHeaderReadContext{ {}, true } );
    ASSERT_TRUE( header.IsSuccess() ) << demat.string() << ": " << header.GetError();
    ASSERT_FALSE( mesh.GetValue().Submeshes.empty() );
    for ( const auto& submesh : mesh.GetValue().Submeshes )
    {
        EXPECT_FALSE( submesh.MaterialGuid.IsNull() ) << "a skinned submesh names no material";
        EXPECT_EQ( submesh.MaterialGuid, header.GetValue().Guid )
             << "the submesh names another material than " << demat.string();
    }
}

namespace
{
    struct Cooked
    {
        Ser::MeshAssetData      Mesh;
        Ser::SkeletonAssetData  Rig;
        Ser::AnimationAssetData Clip;
    };

    Cooked ReadCooked( const Editor::ImportOutcome& outcome )
    {
        Cooked     out;
        const auto mesh = Ser::ReadMeshAssetData( Read( outcome.WrittenMeshes.front() ),
                                                  outcome.WrittenMeshes.front().string() );
        EXPECT_TRUE( mesh.IsSuccess() ) << mesh.GetError();
        const auto rig = Ser::ReadSkeletonJson( Read( outcome.WrittenSkeletons.front() ) );
        EXPECT_TRUE( rig.IsSuccess() ) << rig.GetError();
        const auto clip = Ser::ReadAnimationJson( Read( outcome.WrittenClips.front() ) );
        EXPECT_TRUE( clip.IsSuccess() ) << clip.GetError();
        if ( mesh.IsSuccess() )
            out.Mesh = mesh.GetValue();
        if ( rig.IsSuccess() )
            out.Rig = rig.GetValue();
        if ( clip.IsSuccess() )
            out.Clip = clip.GetValue();
        return out;
    }

    // Every bone's model-space bind, from the local binds down the parents.
    std::vector<glm::mat4> GlobalBinds( const Ser::SkeletonAssetData& rig )
    {
        std::vector<glm::mat4> global( rig.Bones.size(), glm::mat4( 1.0f ) );
        std::vector<bool>      done( rig.Bones.size(), false );
        for ( std::size_t pass = 0; pass < rig.Bones.size(); ++pass )
            for ( std::size_t i = 0; i < rig.Bones.size(); ++i )
            {
                const auto& bone = rig.Bones[i];
                if ( done[i] || ( bone.ParentBoneID && !done[*bone.ParentBoneID] ) )
                    continue;
                global[i] = bone.ParentBoneID ? global[*bone.ParentBoneID] * bone.LocalBindTransform
                                              : bone.LocalBindTransform;
                done[i]   = true;
            }
        return global;
    }
} // namespace

// THM1l-b19: REIMPORT WITH ANOTHER UNIFORM SCALE rescales the mesh, the rig's bind AND inverse bind, and the clips
// by ONE transform (UE's reimport of a skeletal mesh), so the mesh skinned in its bind pose is the mesh unskinned.
// Live on Fox.glb: Scale 10 -> Reimport drew a torn star.
TEST_F( SkinnedImport, AReimportAtTenTimesTheScaleScalesMeshRigAndClipTogether )
{
    const Cooked before = ReadCooked( m_Outcome );

    Assets::SourceImportSettings tenfold;
    tenfold.Mesh.UniformScale         = 10.0f;
    const Editor::ImportOutcome again = ImportManager().ImportWithSettings( m_Source, tenfold );
    ASSERT_EQ( again.Verdict, Editor::CookVerdict::Cooked );
    ASSERT_EQ( again.WrittenMeshes.size(), 1u );
    ASSERT_EQ( again.WrittenSkeletons.size(), 1u );
    ASSERT_EQ( again.WrittenClips.size(), 1u );
    const Cooked after = ReadCooked( again );

    ASSERT_EQ( after.Mesh.SkinnedVertices.size(), before.Mesh.SkinnedVertices.size() );
    ASSERT_EQ( after.Rig.Bones.size(), before.Rig.Bones.size() );
    for ( std::size_t v = 0; v < after.Mesh.SkinnedVertices.size(); ++v )
    {
        const glm::vec3 was = before.Mesh.SkinnedVertices[v].Position;
        const glm::vec3 now = after.Mesh.SkinnedVertices[v].Position;
        EXPECT_NEAR( glm::length( now - 10.0f * was ), 0.0f, 1e-3f ) << "vertex " << v;
    }
    const auto& boxWas = before.Mesh.Submeshes.front().BoundingBox;
    const auto& boxNow = after.Mesh.Submeshes.front().BoundingBox;
    // The scale the reimport REALLY applied, printed (live, the log's "geometry scaled by 100" is the file's unit,
    // glTF metres -> cm, and says nothing of the record's Uniform Scale, which ApplySourceToEngine bakes after).
    const float applied = ( boxNow.Max.z - boxNow.Min.z ) / ( boxWas.Max.z - boxWas.Min.z );
    std::printf( "[SkinnedImport] Uniform Scale 1 -> 10: the mesh's box grew x%g\n",
                 static_cast<double>( applied ) );
    EXPECT_NEAR( applied, 10.0f, 1e-3f );
    EXPECT_NEAR( glm::length( ( boxNow.Max - boxNow.Min ) - 10.0f * ( boxWas.Max - boxWas.Min ) ), 0.0f, 1e-3f );

    // The rig: bind translations x10, and the skin in the bind pose is the identity on every vertex.
    for ( std::size_t b = 0; b < after.Rig.Bones.size(); ++b )
        EXPECT_NEAR( glm::length( glm::vec3( after.Rig.Bones[b].LocalBindTransform[3] ) -
                                  10.0f * glm::vec3( before.Rig.Bones[b].LocalBindTransform[3] ) ),
                     0.0f, 1e-3f )
             << after.Rig.Bones[b].Name;
    const std::vector<glm::mat4> global = GlobalBinds( after.Rig );
    for ( const auto& vertex : after.Mesh.SkinnedVertices )
    {
        glm::vec4 skinned( 0.0f );
        for ( std::size_t k = 0; k < 4; ++k )
            if ( vertex.BoneWeights[k] > 0.0f )
                skinned += vertex.BoneWeights[k] *
                           ( global[vertex.BoneIDs[k]] * after.Rig.Bones[vertex.BoneIDs[k]].OffsetMatrix *
                             glm::vec4( vertex.Position, 1.0f ) );
        EXPECT_NEAR( glm::length( glm::vec3( skinned ) - vertex.Position ), 0.0f, 1e-3f )
             << vertex.Position.x << " " << vertex.Position.y << " " << vertex.Position.z;
    }

    // The clip: rotations are scale-free, translations x10.
    ASSERT_EQ( after.Clip.Channels.size(), before.Clip.Channels.size() );
    for ( std::size_t c = 0; c < after.Clip.Channels.size(); ++c )
    {
        const auto& was = before.Clip.Channels[c];
        const auto& now = after.Clip.Channels[c];
        ASSERT_EQ( now.Rotations.size(), was.Rotations.size() );
        for ( std::size_t k = 0; k < now.Rotations.size(); ++k )
            EXPECT_NEAR( std::abs( glm::dot( now.Rotations[k].Value, was.Rotations[k].Value ) ), 1.0f, 1e-4f );
        ASSERT_EQ( now.Positions.size(), was.Positions.size() );
        for ( std::size_t k = 0; k < now.Positions.size(); ++k )
            EXPECT_NEAR( glm::length( now.Positions[k].Value - 10.0f * was.Positions[k].Value ), 0.0f, 1e-3f );
    }
}

// THM1l-b22: the reimport refreshes the loaded mesh under the number the ENTITIES hold - the Skinned Mesh picker's
// row handle, the GUID's fold - and a reimport keeps it. It used the path's hash, which names no `.skmesh`, so Fox
// at Uniform Scale 10 kept its old build: Details read the old Approx Size and the skin tore.
TEST_F( SkinnedImport, AReimportNamesTheSkinnedMeshByTheHandleItsEntitiesHold )
{
    const auto known = Editor::LoadedHandleOf( m_Outcome.WrittenMeshes.front() );
    ASSERT_TRUE( known.IsSuccess() ) << known.GetError();
    const auto rows = Assets::ContentRegistry::MeshRows( true );
    ASSERT_EQ( rows.size(), 1u );
    EXPECT_EQ( static_cast<uint64_t>( known.GetValue() ), static_cast<uint64_t>( rows.front().Handle ) );

    Assets::SourceImportSettings tenfold;
    tenfold.Mesh.UniformScale         = 10.0f;
    const Editor::ImportOutcome again = ImportManager().ImportWithSettings( m_Source, tenfold );
    ASSERT_EQ( again.WrittenMeshes.size(), 1u );
    const auto kept = Editor::LoadedHandleOf( again.WrittenMeshes.front() );
    ASSERT_TRUE( kept.IsSuccess() ) << kept.GetError();
    EXPECT_EQ( static_cast<uint64_t>( kept.GetValue() ), static_cast<uint64_t>( known.GetValue() ) );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
