// FO-2: a mesh or a collection dropped on the foliage palette becomes `.defoliage` types WITHOUT duplicates.
// What must hold: a second drop of the same mesh finds the type the first one made (one file, same GUID); a
// type whose numbers were tuned is not the one a default drop reuses; a collection records each item's type
// in its manifest, and a second drop of the collection reuses exactly those files without asking for meshes;
// a record that no longer names its file refuses instead of silently minting a fresh type.

#include <Editor/Panels/Collections/CollectionFoliageTypes.hpp>

#include <Common/Utilities/FileSystem.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

using namespace Desert::Assets::Serialization;
using Desert::Assets::AssetGuidRef;
using Desert::Editor::CollectionManifest;
using Desert::Editor::CollectionManifestItem;

namespace
{
    const AssetGuidRef kGrass{ "0123456789abcdef0123456789abcdef", "Meshes/Grass.stmesh" };
    const AssetGuidRef kFern{ "fedcba9876543210fedcba9876543210", "Meshes/Fern.stmesh" };

    class FoliageTypeDnD : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_Root  = std::filesystem::temp_directory_path() / ( "desert_fo2_" + std::to_string( stamp ) );
            m_Types = m_Root / "Foliage";
            std::filesystem::create_directories( m_Types );
        }
        void TearDown() override
        {
            std::error_code ec;
            std::filesystem::remove_all( m_Root, ec );
        }

        [[nodiscard]] std::size_t TypeFiles() const
        {
            std::size_t n = 0;
            for ( const auto& e : std::filesystem::recursive_directory_iterator( m_Types ) )
                n += e.path().extension() == kFoliageTypeExtension ? 1 : 0;
            return n;
        }

        [[nodiscard]] static FoliageTypeData Defaults( const AssetGuidRef& mesh )
        {
            FoliageTypeData data;
            data.Mesh = mesh;
            return data;
        }

        // What the editor's mesh lookup answers, by the item's source path; counts the calls.
        Desert::Editor::CollectionMeshResolver Meshes()
        {
            return [this]( const CollectionManifestItem& item ) -> Common::ResultStr<AssetGuidRef>
            {
                ++m_MeshCalls;
                const std::map<std::string, AssetGuidRef> known{
                     { "Src/grass.fbx", kGrass }, { "Src/fern.fbx", kFern }, { "Src/grass_copy.fbx", kGrass } };
                const auto it = known.find( item.Mesh );
                if ( it == known.end() )
                    return Common::MakeError<AssetGuidRef>( "no such mesh" );
                return Common::MakeSuccess( it->second );
            };
        }

        static CollectionManifest Pack()
        {
            CollectionManifest m;
            m.Name  = "Meadow";
            m.Items = { { .Name        = "grass",
                          .Category    = std::nullopt,
                          .Mesh        = "Src/grass.fbx",
                          .Thumbnail   = std::nullopt,
                          .Material    = std::nullopt,
                          .FoliageType = std::nullopt },
                        { .Name        = "fern",
                          .Category    = std::nullopt,
                          .Mesh        = "Src/fern.fbx",
                          .Thumbnail   = std::nullopt,
                          .Material    = std::nullopt,
                          .FoliageType = std::nullopt } };
            return m;
        }

        std::filesystem::path m_Root;
        std::filesystem::path m_Types;
        int                   m_MeshCalls = 0;
    };
} // namespace

TEST_F( FoliageTypeDnD, TheSameMeshDroppedTwiceFindsTheFirstType )
{
    const auto first = FindOrCreateFoliageTypeFile( m_Types, Defaults( kGrass ), "grass" );
    ASSERT_TRUE( first ) << first.GetError();
    EXPECT_TRUE( first.GetValue().Created );
    EXPECT_EQ( first.GetValue().Guid.size(), 32u );

    const auto second = FindOrCreateFoliageTypeFile( m_Types, Defaults( kGrass ), "grass" );
    ASSERT_TRUE( second ) << second.GetError();
    EXPECT_FALSE( second.GetValue().Created ) << "the second drop minted a duplicate";
    EXPECT_EQ( second.GetValue().Path, first.GetValue().Path );
    EXPECT_EQ( second.GetValue().Guid, first.GetValue().Guid );
    EXPECT_EQ( TypeFiles(), 1u );
}

TEST_F( FoliageTypeDnD, AnotherMeshGetsItsOwnType )
{
    ASSERT_TRUE( FindOrCreateFoliageTypeFile( m_Types, Defaults( kGrass ), "grass" ) );
    const auto fern = FindOrCreateFoliageTypeFile( m_Types, Defaults( kFern ), "fern" );
    ASSERT_TRUE( fern ) << fern.GetError();
    EXPECT_TRUE( fern.GetValue().Created );
    EXPECT_EQ( fern.GetValue().Path.filename(), "fern.defoliage" );
    EXPECT_EQ( TypeFiles(), 2u );
}

TEST_F( FoliageTypeDnD, ATunedTypeIsNotTheOneADefaultDropReuses )
{
    FoliageTypeData tuned = Defaults( kGrass );
    tuned.Density         = 40.0f;
    ASSERT_TRUE( SaveFoliageTypeFile( m_Types / "grass.defoliage", tuned ) );

    const auto dropped = FindOrCreateFoliageTypeFile( m_Types, Defaults( kGrass ), "grass" );
    ASSERT_TRUE( dropped ) << dropped.GetError();
    EXPECT_TRUE( dropped.GetValue().Created );
    EXPECT_EQ( dropped.GetValue().Path.filename(), "grass_1.defoliage" ) << "the tuned file's name was reused";
    EXPECT_EQ( TypeFiles(), 2u );
}

TEST_F( FoliageTypeDnD, ATypeInASubfolderIsFound )
{
    ASSERT_TRUE( SaveFoliageTypeFile( m_Types / "Meadow" / "grass.defoliage", Defaults( kGrass ) ) );
    const auto dropped = FindOrCreateFoliageTypeFile( m_Types, Defaults( kGrass ), "grass" );
    ASSERT_TRUE( dropped ) << dropped.GetError();
    EXPECT_FALSE( dropped.GetValue().Created );
    EXPECT_EQ( TypeFiles(), 1u );
}

TEST_F( FoliageTypeDnD, AnUnreadableTypeRefusesTheLookupByName )
{
    std::ofstream( m_Types / "broken.defoliage" ) << "{ not json";
    const auto dropped = FindOrCreateFoliageTypeFile( m_Types, Defaults( kGrass ), "grass" );
    ASSERT_FALSE( dropped ) << "a lookup skipped a file it could not rule out";
    EXPECT_NE( dropped.GetError().find( "broken.defoliage" ), std::string::npos ) << dropped.GetError();
    EXPECT_EQ( TypeFiles(), 1u );
}

TEST_F( FoliageTypeDnD, ACollectionRecordsItsTypesAndASecondDropReusesThem )
{
    CollectionManifest m     = Pack();
    const auto         first = Desert::Editor::ResolveCollectionFoliageTypes( m, m_Types, m_Root, Meshes() );
    ASSERT_TRUE( first ) << first.GetError();
    ASSERT_EQ( first.GetValue().Types.size(), 2u );
    EXPECT_TRUE( first.GetValue().ManifestChanged );
    EXPECT_EQ( m_MeshCalls, 2 );
    EXPECT_EQ( TypeFiles(), 2u );
    for ( std::size_t i = 0; i < 2; ++i )
    {
        if ( !m.Items[i].FoliageType.has_value() )
            FAIL() << "item " << i << " has no record";
        EXPECT_EQ( m.Items[i].FoliageType->Guid, first.GetValue().Types[i].Guid ) << i;
        EXPECT_EQ( m_Root / m.Items[i].FoliageType->Path, first.GetValue().Types[i].Path ) << i;
    }
    EXPECT_EQ( m.Items[0].FoliageType->Path, "Foliage/grass.defoliage" );

    // The manifest goes to disk and comes back through its one reader, as the panel does.
    const auto file = m_Root / "collection.json";
    ASSERT_TRUE( Desert::Editor::SaveCollectionManifest( file, m ) );
    const auto text = Common::Utils::FileSystem::ReadFileContent( file );
    ASSERT_TRUE( text );
    auto reread = Desert::Editor::ReadCollectionManifest( text.GetValue() );
    ASSERT_TRUE( reread ) << reread.GetError();

    CollectionManifest again  = reread.GetValue();
    const auto         second = Desert::Editor::ResolveCollectionFoliageTypes( again, m_Types, m_Root, Meshes() );
    ASSERT_TRUE( second ) << second.GetError();
    EXPECT_FALSE( second.GetValue().ManifestChanged );
    EXPECT_EQ( m_MeshCalls, 2 ) << "a recorded item asked for its mesh again";
    EXPECT_EQ( TypeFiles(), 2u );
    for ( std::size_t i = 0; i < 2; ++i )
    {
        EXPECT_EQ( second.GetValue().Types[i].Guid, first.GetValue().Types[i].Guid ) << i;
        EXPECT_FALSE( second.GetValue().Types[i].Created ) << i;
    }
}

TEST_F( FoliageTypeDnD, ARecordKeepsTheTuningADefaultDropWouldLose )
{
    CollectionManifest m = Pack();
    ASSERT_TRUE( Desert::Editor::ResolveCollectionFoliageTypes( m, m_Types, m_Root, Meshes() ) );
    FoliageTypeData tuned = Defaults( kGrass );
    tuned.Density         = 40.0f;
    const auto grassFile  = m_Root / m.Items[0].FoliageType->Path;
    {
        const auto text = Common::Utils::FileSystem::ReadFileContent( grassFile );
        ASSERT_TRUE( text );
        auto held = ParseFoliageType( text.GetValue() );
        ASSERT_TRUE( held );
        tuned.Header = held.GetValue().Header; // keep the GUID, as the Details edit does
    }
    ASSERT_TRUE( SaveFoliageTypeFile( grassFile, tuned ) );

    const auto second = Desert::Editor::ResolveCollectionFoliageTypes( m, m_Types, m_Root, Meshes() );
    ASSERT_TRUE( second ) << second.GetError();
    EXPECT_EQ( second.GetValue().Types[0].Path, grassFile );
    EXPECT_EQ( TypeFiles(), 2u ) << "the tuned type was replaced by a fresh default one";
}

TEST_F( FoliageTypeDnD, ACollectionReusesTheTypeASingleMeshDropMade )
{
    const auto single = FindOrCreateFoliageTypeFile( m_Types, Defaults( kGrass ), "grass" );
    ASSERT_TRUE( single );
    CollectionManifest m = Pack();
    m.Items.push_back( { .Name        = "grass again",
                         .Category    = std::nullopt,
                         .Mesh        = "Src/grass_copy.fbx",
                         .Thumbnail   = std::nullopt,
                         .Material    = std::nullopt,
                         .FoliageType = std::nullopt } );
    const auto dropped = Desert::Editor::ResolveCollectionFoliageTypes( m, m_Types, m_Root, Meshes() );
    ASSERT_TRUE( dropped ) << dropped.GetError();
    EXPECT_EQ( dropped.GetValue().Types[0].Guid, single.GetValue().Guid );
    EXPECT_EQ( dropped.GetValue().Types[2].Guid, single.GetValue().Guid ) << "two items, one mesh, two types";
    EXPECT_EQ( TypeFiles(), 2u );
}

TEST_F( FoliageTypeDnD, ARecordThatNoLongerNamesItsFileRefuses )
{
    CollectionManifest m = Pack();
    ASSERT_TRUE( Desert::Editor::ResolveCollectionFoliageTypes( m, m_Types, m_Root, Meshes() ) );
    m.Items[1].FoliageType->Guid = "00000000000000000000000000000001";
    const auto stale             = Desert::Editor::ResolveCollectionFoliageTypes( m, m_Types, m_Root, Meshes() );
    ASSERT_FALSE( stale ) << "a record naming another GUID was accepted";
    EXPECT_NE( stale.GetError().find( "fern" ), std::string::npos ) << stale.GetError();
    EXPECT_NE( stale.GetError().find( "00000000000000000000000000000001" ), std::string::npos )
         << stale.GetError();

    std::filesystem::remove( m_Root / m.Items[0].FoliageType->Path );
    m.Items[1].FoliageType.reset();
    const auto gone = Desert::Editor::ResolveCollectionFoliageTypes( m, m_Types, m_Root, Meshes() );
    ASSERT_FALSE( gone ) << "a record whose file is gone was answered with a fresh type";
    EXPECT_NE( gone.GetError().find( "grass" ), std::string::npos ) << gone.GetError();
}

TEST_F( FoliageTypeDnD, AnItemWhoseMeshDoesNotResolveRefusesNamingIt )
{
    CollectionManifest m = Pack();
    m.Items[1].Mesh      = "Src/missing.fbx";
    const auto dropped   = Desert::Editor::ResolveCollectionFoliageTypes( m, m_Types, m_Root, Meshes() );
    ASSERT_FALSE( dropped );
    EXPECT_NE( dropped.GetError().find( "Src/missing.fbx" ), std::string::npos ) << dropped.GetError();
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
