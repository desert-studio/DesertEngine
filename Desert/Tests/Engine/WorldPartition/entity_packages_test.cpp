// WP17: a save of a partitioned world writes only the entities whose revision differs from their files'
// (Core/Serialize/EntityPackages.hpp), and undo back to the saved revision leaves nothing to write.
//
// SceneSerializer::SaveToFile cannot be compiled by a suite (it reaches the renderer), so the scene here is a
// model whose composer counts the records it is asked to serialize - the same callback SaveToFile hands
// SaveThroughPackages.

#include <Engine/Core/Serialize/EntityPackages.hpp>
#include <Engine/Core/Serialize/ExternalEntities.hpp>

#include <Common/Json/Carry.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace
{
    using Common::UUID;
    using namespace Desert::Core;
    namespace EE = Desert::Core::ExternalEntities;

    struct ModelWorld
    {
        std::filesystem::path                Root;
        std::filesystem::path                Scene;
        std::map<std::uint64_t, std::string> Tags; // id -> tag; every entity a root
        EntityPackages                       Packages;
        std::size_t                          Composed = 0;

        ModelWorld()
        {
            Root = std::filesystem::temp_directory_path() /
                   ( "wp17_" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
            std::filesystem::create_directories( Root );
            Scene = Root / "World.desce";
            Tags  = { { 10, "A" }, { 20, "B" }, { 30, "C" } };
        }
        ~ModelWorld()
        {
            std::error_code ec;
            std::filesystem::remove_all( Root, ec );
        }

        std::vector<LiveEntity> Live() const
        {
            std::vector<LiveEntity> live;
            std::uint32_t           index = 0;
            for ( const auto& [id, tag] : Tags )
                live.push_back( LiveEntity{ UUID( id ), UUID( id ), UUID( 0 ), index++ } );
            return live;
        }

        PackageSaveOutcome Save()
        {
            const auto live  = Live();
            auto       saved = SaveThroughPackages(
                 Scene, Packages, live, true,
                 [&]( const std::unordered_set<std::uint64_t>* only )
                      -> Common::ResultStr<Common::Json::TextDocument>
                 {
                     std::string   records;
                     std::uint32_t index = 0;
                     for ( const auto& [id, tag] : Tags )
                     {
                         const std::uint32_t sibling = index++;
                         if ( only != nullptr && !only->contains( id ) )
                             continue;
                         ++Composed;
                         records += ( records.empty() ? "" : "," ) + std::string( R"({"id":)" ) +
                                    std::to_string( id ) + R"(,"Tag":")" + tag + R"(","siblingIndex":)" +
                                    std::to_string( sibling ) + "}";
                     }
                     return Common::Json::TextDocument::Parse(
                          R"({"SceneName":"World","Entities":[)" + records +
                          R"(],"WorldPartition":{"Grids":[{"CellSize":12800.0,"LoadingRange":25600.0}]}})" );
                 } );
            EXPECT_TRUE( saved ) << ( saved ? "" : saved.GetError() );
            return saved ? saved.GetValue() : PackageSaveOutcome{};
        }

        std::string Joined() const
        {
            const auto text = EE::ReadSceneFileText( Scene );
            EXPECT_TRUE( text ) << ( text ? "" : text.GetError() );
            return text ? text.GetValue() : std::string();
        }
    };
} // namespace

TEST( EntityPackages, TheFirstSaveWithoutABaselineIsWhole )
{
    ModelWorld world;
    const auto first = world.Save();
    EXPECT_TRUE( first.Whole );
    EXPECT_EQ( first.Serialized, 3u );
    EXPECT_EQ( first.Files.Written, 4u ); // three records + the header
}

TEST( EntityPackages, AnEditToOneEntityWritesOneFileAndSerializesOneRecord )
{
    ModelWorld world;
    world.Save();
    world.Composed = 0;

    world.Packages.Touch( UUID( 20 ) );
    world.Tags[20]   = "B2";
    const auto delta = world.Save();
    EXPECT_FALSE( delta.Whole );
    EXPECT_EQ( world.Composed, 1u );
    EXPECT_EQ( delta.Serialized, 1u );
    EXPECT_EQ( delta.Files.Written, 1u );   // the entity's file
    EXPECT_EQ( delta.Files.Unchanged, 1u ); // the header: the list did not change
    EXPECT_NE( world.Joined().find( "B2" ), std::string::npos );
    EXPECT_NE( world.Joined().find( "\"A\"" ), std::string::npos );
}

TEST( EntityPackages, UndoBackToTheSavedRevisionLeavesNothingToWrite )
{
    ModelWorld world;
    world.Save();
    world.Composed = 0;

    const auto stamp = world.Packages.Touch( UUID( 20 ) );
    world.Tags[20]   = "B2";
    world.Packages.Restore( UUID( 20 ), stamp.Before ); // undo
    world.Tags[20] = "B";
    EXPECT_FALSE( world.Packages.IsDirty( UUID( 20 ) ) );

    const auto save = world.Save();
    EXPECT_FALSE( save.Whole );
    EXPECT_EQ( world.Composed, 0u );
    EXPECT_EQ( save.Files.Written, 0u );
    EXPECT_EQ( save.Files.Removed, 0u );
}

TEST( EntityPackages, EditSaveUndoIsDirtyAgain )
{
    ModelWorld world;
    world.Save();
    const auto stamp = world.Packages.Touch( UUID( 20 ) );
    world.Tags[20]   = "B2";
    world.Save();
    EXPECT_FALSE( world.Packages.IsDirty( UUID( 20 ) ) );

    world.Packages.Restore( UUID( 20 ), stamp.Before ); // undo after the save: the file holds B2
    world.Tags[20] = "B";
    EXPECT_TRUE( world.Packages.IsDirty( UUID( 20 ) ) );
    world.Composed  = 0;
    const auto save = world.Save();
    EXPECT_EQ( world.Composed, 1u );
    EXPECT_EQ( save.Files.Written, 1u );
}

TEST( EntityPackages, ADeleteRemovesTheFileAndRewritesTheHeaderAndShiftedSiblings )
{
    ModelWorld world;
    world.Save();
    world.Composed = 0;
    world.Tags.erase( 10 ); // 20 and 30 move up one place among the roots: their records change
    const auto save = world.Save();
    EXPECT_FALSE( save.Whole );
    EXPECT_EQ( save.Files.Removed, 1u );
    EXPECT_EQ( world.Composed, 2u );
    EXPECT_FALSE( std::filesystem::exists( EE::FileOf( world.Scene, UUID( 10 ) ) ) );
    EXPECT_EQ( world.Joined().find( "\"A\"" ), std::string::npos );
}

TEST( EntityPackages, AnEditThatNamesNoEntityMakesTheNextSaveWhole )
{
    ModelWorld world;
    world.Save();
    world.Packages.TouchAll();
    world.Composed  = 0;
    const auto save = world.Save();
    EXPECT_TRUE( save.Whole );
    EXPECT_EQ( world.Composed, 3u );
    EXPECT_EQ( save.Files.Written, 0u ); // whole, yet nothing differs
    EXPECT_FALSE( world.Save().Whole );  // the save took the baseline
}

TEST( EntityPackages, ASaveToAnotherFileIsWhole )
{
    ModelWorld world;
    world.Save();
    const PackageSavePlan plan = world.Packages.Plan( world.Root / "Other.desce", world.Live(), true );
    EXPECT_TRUE( plan.Whole );
    EXPECT_EQ( plan.Changed.size(), 3u );
}
