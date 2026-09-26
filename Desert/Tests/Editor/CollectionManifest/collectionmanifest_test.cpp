// collection.json: what FbxMeshSplitter writes, the Collections panel must accept.
//
// The splitter used to build the manifest by string concatenation with no escaping, and the panel reads it
// strictly. A collection folder, texture stem or path holding a quote or a backslash therefore produced a file
// the panel refused, and the refusal only reached the log: the collection silently vanished from the panel.
// Both ends now share Editor/Panels/Collections/CollectionManifest.hpp. These tests write a manifest through
// that writer into a temporary directory, read it back the way the panel does (file bytes -> strict reader),
// and pin the two refusals the strict reader owes: a member of the wrong type and a key nobody declared.
#include <gtest/gtest.h>

#include <Editor/Panels/Collections/CollectionManifest.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    using Desert::Editor::CollectionManifest;
    using Desert::Editor::CollectionManifestItem;
    using Desert::Editor::CollectionManifestMaterial;

    class CollectionManifestFile : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_Dir            = std::filesystem::temp_directory_path() /
                    ( "desert_collection_manifest_" + std::to_string( stamp ) );
            std::filesystem::create_directories( m_Dir );
        }
        void TearDown() override
        {
            std::error_code ec;
            std::filesystem::remove_all( m_Dir, ec );
        }

        // The panel's path: the file's bytes, then the strict reader.
        [[nodiscard]] std::filesystem::path Save( const std::string& json ) const
        {
            const auto    path = m_Dir / "collection.json";
            std::ofstream out( path, std::ios::binary );
            out << json;
            return path;
        }
        [[nodiscard]] static Common::ResultStr<CollectionManifest> Load( const std::filesystem::path& path )
        {
            std::ifstream in( path, std::ios::binary );
            if ( !in )
                return Common::MakeError<CollectionManifest>( "cannot open " + path.string() );
            const std::string bytes( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
            return Desert::Editor::ReadCollectionManifest( bytes );
        }

        std::filesystem::path m_Dir;
    };

    // What the splitter builds for one pack, with the characters the hand-built writer never escaped.
    CollectionManifest SplitterShaped()
    {
        CollectionManifest manifest;
        manifest.Name      = R"(Oak "Grove" \ Pack)";
        manifest.Author    = "FbxMeshSplitter";
        // Every field named, absent ones as nullopt: a designated initializer that skips a field is what
        // -Wmissing-field-initializers reports, and naming them also shows which paths the writer must omit.
        manifest.Materials = std::vector<CollectionManifestMaterial>{
             { .Name        = "bark",
               .Albedo      = R"(Resources/Collections/Oak "Grove"/bark_albedo.png)",
               .Opacity     = std::nullopt,
               .Normal      = "Resources/Collections/Oak/bark_normal.png",
               .Roughness   = std::nullopt,
               .Metallic    = std::nullopt,
               .AO          = std::nullopt,
               .AlphaCutoff = 0.0f,
               .TwoSided    = false },
             { .Name        = "leaf",
               .Albedo      = "Resources/Collections/Oak/leaf_albedo.png",
               .Opacity     = "Resources/Collections/Oak/leaf_opacity.png",
               .Normal      = std::nullopt,
               .Roughness   = std::nullopt,
               .Metallic    = std::nullopt,
               .AO          = std::nullopt,
               .AlphaCutoff = 0.5f,
               .TwoSided    = true } };
        manifest.Items = { { .Name      = "trunk",
                             .Category  = R"(Oak "Grove")",
                             .Mesh      = "Resources/Mesh/Oak/trunk.obj",
                             .Thumbnail = std::nullopt,
                             .Material  = 0 },
                           { .Name      = "leaves",
                             .Category  = "Oak",
                             .Mesh      = R"(Resources/Mesh/Oak\leaves.obj)",
                             .Thumbnail = std::nullopt,
                             .Material  = 1 },
                           { .Name      = "stump",
                             .Category  = std::nullopt,
                             .Mesh      = "Resources/Mesh/Oak/stump.obj",
                             .Thumbnail = std::nullopt,
                             .Material  = std::nullopt } };
        return manifest;
    }

    // A manifest whose item carries Material as a string: the shape a hand edit or another producer can reach.
    struct WrongTypeItem
    {
        std::string Name;
        std::string Mesh;
        std::string Material;
    };
    struct WrongTypeManifest
    {
        std::string                Name;
        std::vector<WrongTypeItem> Items;
    };

    // A manifest carrying a key the shape does not declare (a producer on a newer schema, or a typo).
    struct ExtraKeyItem
    {
        std::string Name;
        std::string Mesh;
        int         Rating = 0;
    };
    struct ExtraKeyManifest
    {
        std::string               Name;
        std::vector<ExtraKeyItem> Items;
    };
} // namespace

TEST_F( CollectionManifestFile, WhatTheSplitterWritesThePanelReadsBackUnchanged )
{
    const CollectionManifest written = SplitterShaped();
    const auto               read    = Load( Save( Desert::Editor::WriteCollectionManifest( written ) ) );
    ASSERT_TRUE( read ) << read.GetError();
    const CollectionManifest& back = read.GetValue();

    EXPECT_EQ( back.Name, written.Name );
    EXPECT_EQ( back.Author, written.Author );
    // An explicit early return rather than ASSERT_TRUE: clang-tidy's optional-access analysis does not see
    // through gtest's macro, and FAIL() is a plain return it does see.
    if ( !written.Materials.has_value() || !back.Materials.has_value() )
        FAIL() << "a manifest with materials read back without them";
    const auto& writtenMaterials = *written.Materials;
    const auto& backMaterials    = *back.Materials;
    ASSERT_EQ( backMaterials.size(), 2u );
    for ( std::size_t i = 0; i < 2; ++i )
    {
        const auto& a = writtenMaterials[i];
        const auto& b = backMaterials[i];
        EXPECT_EQ( b.Name, a.Name ) << i;
        EXPECT_EQ( b.Albedo, a.Albedo ) << i;
        EXPECT_EQ( b.Opacity, a.Opacity ) << i;
        EXPECT_EQ( b.Normal, a.Normal ) << i;
        EXPECT_EQ( b.AlphaCutoff, a.AlphaCutoff ) << i;
        EXPECT_EQ( b.TwoSided, a.TwoSided ) << i;
    }
    ASSERT_EQ( back.Items.size(), written.Items.size() );
    for ( std::size_t i = 0; i < written.Items.size(); ++i )
    {
        EXPECT_EQ( back.Items[i].Name, written.Items[i].Name ) << i;
        EXPECT_EQ( back.Items[i].Category, written.Items[i].Category ) << i;
        EXPECT_EQ( back.Items[i].Mesh, written.Items[i].Mesh ) << i;
        EXPECT_EQ( back.Items[i].Material, written.Items[i].Material ) << i;
    }
}

TEST_F( CollectionManifestFile, AMemberOfTheWrongTypeIsRefusedAndNamed )
{
    const WrongTypeManifest wrong{ .Name  = "Oak",
                                   .Items = { { .Name = "trunk", .Mesh = "m.obj", .Material = "bark" } } };
    const auto              read = Load( Save( Common::Json::Write( wrong ) ) );
    ASSERT_FALSE( read ) << "Material as a string was accepted";
    EXPECT_NE( read.GetError().find( "Items" ), std::string::npos ) << read.GetError();
    EXPECT_NE( read.GetError().find( "Material" ), std::string::npos ) << read.GetError();
}

TEST_F( CollectionManifestFile, AnUndeclaredKeyIsRefusedAndNamed )
{
    const ExtraKeyManifest extra{ .Name = "Oak", .Items = { { .Name = "trunk", .Mesh = "m.obj", .Rating = 5 } } };
    const auto             read = Load( Save( Common::Json::Write( extra ) ) );
    ASSERT_FALSE( read ) << "an undeclared key was accepted";
    EXPECT_NE( read.GetError().find( "Rating" ), std::string::npos ) << read.GetError();
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
