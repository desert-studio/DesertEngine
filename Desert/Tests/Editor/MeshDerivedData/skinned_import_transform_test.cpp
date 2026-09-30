// THM1l-b: the import options Uniform Scale / Up Axis reach a skinned file - its vertices, its skeleton and its
// clips, consistently (UE applies FBX Import Options to skeletal meshes too), and LOD Generate simplifies a
// skinned section as it does a static one.
//
// The fixture stands in for what the importer hands over for a two-bone skinned file: a root bone lifted 1 unit
// up, a child 2 units further along +z, one vertex at the child, and one clip moving/turning the child.

#include <Editor/Import/MeshDeriver.hpp>
#include <Editor/Import/SourceToEngine.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <gtest/gtest.h>

#include <vector>

using namespace Desert;
namespace Ser = Assets::Serialization;

namespace
{
    struct SkinnedFile
    {
        Ser::MeshAssetData                   Mesh;
        Ser::SkeletonAssetData               Skeleton;
        std::vector<Ser::AnimationAssetData> Clips;
    };

    SkinnedFile MakeFile()
    {
        SkinnedFile file;
        file.Mesh.IsSkinned = true;
        Ser::SkinnedVertexData v{};
        v.Position    = { 0.0f, 1.0f, 2.0f };
        v.Normal      = { 0.0f, 0.0f, 1.0f };
        v.BoneIDs     = { 1, 0, 0, 0 };
        v.BoneWeights = { 1.0f, 0.0f, 0.0f, 0.0f };
        file.Mesh.SkinnedVertices.push_back( v );
        Ser::SubmeshData sub{};
        sub.VertexCount     = 1;
        sub.Transform       = glm::mat4( 1.0f );
        sub.BoundingBox.Min = { 0.0f, 1.0f, 2.0f };
        sub.BoundingBox.Max = { 0.0f, 1.0f, 2.0f };
        file.Mesh.Submeshes.push_back( sub );

        Animation::BoneInfo root;
        root.Name               = "root";
        root.LocalBindTransform = glm::translate( glm::mat4( 1.0f ), { 0.0f, 1.0f, 0.0f } );
        root.OffsetMatrix       = glm::inverse( root.LocalBindTransform );
        Animation::BoneInfo child;
        child.Name               = "child";
        child.ParentBoneID       = 0;
        child.LocalBindTransform = glm::translate( glm::mat4( 1.0f ), { 0.0f, 0.0f, 2.0f } );
        child.OffsetMatrix       = glm::inverse( root.LocalBindTransform * child.LocalBindTransform );
        file.Skeleton.Bones      = { root, child };

        Ser::AnimationAssetData clip;
        Ser::ChannelData        channel;
        channel.BoneName = "child";
        Ser::KeyPosition p;
        p.Value = { 0.0f, 0.0f, 5.0f };
        channel.Positions.push_back( p );
        Ser::KeyRotation r;
        r.Value = glm::angleAxis( glm::radians( 90.0f ), glm::vec3( 0.0f, 0.0f, 1.0f ) );
        channel.Rotations.push_back( r );
        Ser::KeyScale s;
        s.Value = { 1.0f, 2.0f, 3.0f };
        channel.Scales.push_back( s );
        clip.Channels.push_back( channel );
        file.Clips.push_back( clip );
        return file;
    }

    void Apply( SkinnedFile& file, const Assets::MeshImportSettings& settings )
    {
        Editor::ApplySourceToEngine( settings, &file.Mesh, &file.Skeleton, file.Clips );
    }

    void ExpectNear( const glm::vec3& a, const glm::vec3& b )
    {
        EXPECT_NEAR( a.x, b.x, 1e-5f );
        EXPECT_NEAR( a.y, b.y, 1e-5f );
        EXPECT_NEAR( a.z, b.z, 1e-5f );
    }

    glm::vec3 Translation( const glm::mat4& m )
    {
        return { m[3] };
    }

    // The skinned position of vertex 0 in bind pose: global(child) * offset(child) * v.
    glm::vec3 BindSkinned( const SkinnedFile& file )
    {
        const auto&     bones  = file.Skeleton.Bones;
        const glm::mat4 global = bones[0].LocalBindTransform * bones[1].LocalBindTransform;
        return { global * bones[1].OffsetMatrix * glm::vec4( file.Mesh.SkinnedVertices[0].Position, 1 ) };
    }
} // namespace

TEST( SkinnedImportTransform, DefaultsChangeNothing )
{
    SkinnedFile file = MakeFile();
    Apply( file, {} );
    ExpectNear( file.Mesh.SkinnedVertices[0].Position, { 0.0f, 1.0f, 2.0f } );
    ExpectNear( Translation( file.Skeleton.Bones[1].LocalBindTransform ), { 0.0f, 0.0f, 2.0f } );
    ExpectNear( file.Clips[0].Channels[0].Positions[0].Value, { 0.0f, 0.0f, 5.0f } );
}

TEST( SkinnedImportTransform, UniformScaleScalesVerticesBonesAndClips )
{
    SkinnedFile file = MakeFile();
    Apply( file, { .UniformScale = 2.0f } );
    ExpectNear( file.Mesh.SkinnedVertices[0].Position, { 0.0f, 2.0f, 4.0f } );
    ExpectNear( file.Mesh.SkinnedVertices[0].Normal, { 0.0f, 0.0f, 1.0f } );
    ExpectNear( file.Mesh.Submeshes[0].BoundingBox.Max, { 0.0f, 2.0f, 4.0f } );
    // The scale lands in the bones' translations, not in their linear part.
    ExpectNear( Translation( file.Skeleton.Bones[0].LocalBindTransform ), { 0.0f, 2.0f, 0.0f } );
    ExpectNear( Translation( file.Skeleton.Bones[1].LocalBindTransform ), { 0.0f, 0.0f, 4.0f } );
    ExpectNear( glm::vec3( file.Skeleton.Bones[1].LocalBindTransform[0] ), { 1.0f, 0.0f, 0.0f } );
    ExpectNear( Translation( file.Skeleton.Bones[1].OffsetMatrix ), { 0.0f, -2.0f, -4.0f } );
    ExpectNear( file.Clips[0].Channels[0].Positions[0].Value, { 0.0f, 0.0f, 10.0f } );
    ExpectNear( file.Clips[0].Channels[0].Scales[0].Value, { 1.0f, 2.0f, 3.0f } );
    // Bind pose still reproduces the (scaled) vertex: mesh and skeleton moved together.
    ExpectNear( BindSkinned( file ), { 0.0f, 2.0f, 4.0f } );
}

TEST( SkinnedImportTransform, ZUpBecomesYUpForVerticesBonesAndClips )
{
    SkinnedFile file = MakeFile();
    Apply( file, { .UpAxis = Assets::MeshSourceUpAxis::Z } );
    // (x, y, z) -> (x, z, -y)
    ExpectNear( file.Mesh.SkinnedVertices[0].Position, { 0.0f, 2.0f, -1.0f } );
    ExpectNear( file.Mesh.SkinnedVertices[0].Normal, { 0.0f, 1.0f, 0.0f } );
    ExpectNear( Translation( file.Skeleton.Bones[1].LocalBindTransform ), { 0.0f, 2.0f, 0.0f } );
    const auto& channel = file.Clips[0].Channels[0];
    ExpectNear( channel.Positions[0].Value, { 0.0f, 5.0f, 0.0f } );
    // A turn about the file's up (+z) is a turn about the engine's up (+y).
    ExpectNear( channel.Rotations[0].Value * glm::vec3( 1.0f, 0.0f, 0.0f ), { 0.0f, 0.0f, -1.0f } );
    ExpectNear( channel.Scales[0].Value, { 1.0f, 3.0f, 2.0f } );
    ExpectNear( BindSkinned( file ), { 0.0f, 2.0f, -1.0f } );
}

TEST( SkinnedImportTransform, LodGenerateSimplifiesSkinnedSections )
{
    // A 16 x 16 skinned grid: enough triangles for meshopt to make a chain.
    Ser::MeshAssetData mesh;
    mesh.IsSkinned  = true;
    constexpr int n = 16;
    for ( int y = 0; y <= n; ++y )
        for ( int x = 0; x <= n; ++x )
        {
            Ser::SkinnedVertexData v{};
            v.Position    = { static_cast<float>( x ), static_cast<float>( y ), 0.0f };
            v.BoneWeights = { 1.0f, 0.0f, 0.0f, 0.0f };
            mesh.SkinnedVertices.push_back( v );
        }
    for ( int y = 0; y < n; ++y )
        for ( int x = 0; x < n; ++x )
        {
            const uint32_t a = y * ( n + 1 ) + x;
            mesh.Indices.push_back( { a, a + 1, a + n + 2 } );
            mesh.Indices.push_back( { a, a + n + 2, a + n + 1 } );
        }
    Ser::SubmeshData sub{};
    sub.VertexCount = static_cast<uint32_t>( mesh.SkinnedVertices.size() );
    sub.IndexCount  = static_cast<uint32_t>( mesh.Indices.size() * 3 );
    sub.Transform   = glm::mat4( 1.0f );
    mesh.Submeshes.push_back( sub );

    Editor::BakeMeshLODs( mesh );
    ASSERT_GT( mesh.Submeshes[0].LODs.size(), 1u );
    EXPECT_LT( mesh.Submeshes[0].LODs.back().size(), mesh.Indices.size() );
}
