// WP17: a save of a partitioned world writes only the entities whose revision differs from their files'
// (Core/Serialize/EntityPackages.hpp), and undo back to the saved revision leaves nothing to write.
//
// SceneSerializer::SaveToFile cannot be compiled by a suite (it reaches the renderer), so the scene here is a
// model whose composer counts the records it is asked to serialize - the same callback SaveToFile hands
// SaveThroughPackages.

#include <Engine/Core/Serialize/EntityPackages.hpp>
#include <Engine/Core/Serialize/ExternalEntities.hpp>

#include <Common/Json/Carry.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <gtest/gtest.h>

#include <iterator>
#include <format>
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
                   std::format( "wp17_{}", std::chrono::steady_clock::now().time_since_epoch().count() );
            std::filesystem::create_directories( Root );
            Scene = Root / "World.desce";
            Tags  = { { 10, "A" }, { 20, "B" }, { 30, "C" } };
        }
        ~ModelWorld()
        {
            std::error_code ec;
            std::filesystem::remove_all( Root, ec );
        }

        [[nodiscard]] std::vector<LiveEntity> Live() const
        {
            std::vector<LiveEntity> live;
            std::uint32_t           index = 0;
            live.reserve( Tags.size() );
            for ( const auto& [id, tag] : Tags )
                live.push_back( LiveEntity{ UUID( id ), UUID( id ), UUID( 0 ), index++ } );
            return live;
        }

        Common::ResultStr<PackageSaveOutcome> TrySave( CleanCheck check )
        {
            const auto live = Live();
            return SaveThroughPackages(
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
                         std::format_to( std::back_inserter( records ),
                                         R"({}{{"id":{},"Tag":"{}","siblingIndex":{}}})",
                                         records.empty() ? "" : ",", id, tag, sibling );
                     }
                     return Common::Json::TextDocument::Parse(
                          std::format( R"({{"SceneName":"World","Entities":[{}],"WorldPartition":{{"Grids":[{{)"
                                       R"("CellSize":12800.0,"LoadingRange":25600.0}}]}}}})",
                                       records ) );
                 },
                 check );
        }

        PackageSaveOutcome Save( CleanCheck check = CleanCheck::Trust )
        {
            auto saved = TrySave( check );
            EXPECT_TRUE( saved ) << ( saved ? "" : saved.GetError() );
            return saved ? saved.GetValue() : PackageSaveOutcome{};
        }

        [[nodiscard]] std::string Joined() const
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

// UE's Modify() without a transaction: an edit outside the history marks its entity, and the delta save writes
// it. Undo of an earlier recorded edit cannot make it unmodified - nothing recorded the unrecorded change.
TEST( EntityPackages, AnUnrecordedEditMarkedModifiedIsWrittenAndSurvivesAnUndo )
{
    ModelWorld world;
    world.Save();

    const auto stamp = world.Packages.Touch( UUID( 30 ) );
    world.Packages.MarkModified( UUID( 30 ) );
    world.Tags[30] = "C2";
    world.Packages.Restore( stamp.Id, stamp.Before ); // undo of the recorded edit

    const auto delta = world.Save();
    EXPECT_FALSE( delta.Whole );
    EXPECT_EQ( delta.Files.Written, 1u );
    EXPECT_NE( world.Joined().find( "C2" ), std::string::npos );

    // The save took the baseline: the mark is spent.
    EXPECT_FALSE( world.Packages.IsDirty( UUID( 30 ) ) );
}

// The Debug safety net: an edit that marked nothing is a refusal naming the entity, and nothing is written.
TEST( EntityPackages, AnUnmarkedEditIsRefusedByTheCheckAgainstFiles )
{
    ModelWorld world;
    world.Save();

    world.Packages.Touch( UUID( 10 ) ); // a recorded edit elsewhere keeps the save a delta
    world.Tags[10] = "A2";
    world.Tags[20] = "Bypassed"; // changed without Touch or MarkModified

    const auto refused = world.TrySave( CleanCheck::AgainstFiles );
    ASSERT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( "Bypassed" ), std::string::npos ) << refused.GetError();
    EXPECT_NE( refused.GetError().find( "20" ), std::string::npos ) << refused.GetError();
    EXPECT_EQ( world.Joined().find( "A2" ), std::string::npos ); // the refusal wrote nothing

    // Marked, the same edit passes the check and lands.
    world.Packages.MarkModified( UUID( 20 ) );
    const auto delta = world.Save( CleanCheck::AgainstFiles );
    EXPECT_FALSE( delta.Whole );
    EXPECT_EQ( delta.Files.Written, 2u );
    EXPECT_NE( world.Joined().find( "Bypassed" ), std::string::npos );
}

// WP19: an editor region left entity 30 on disk. A save writes the loaded entities only, keeps 30's file and its
// place in the header, and is never taken for a delete of it.
TEST( EntityPackages, ARecordAnEditorRegionLeftOnDiskIsKeptByTheSave )
{
    ModelWorld world;
    world.Save();
    world.Packages.Baseline( world.Scene, world.Live() );
    const auto thirtyRead = Common::Utils::FileSystem::ReadFileContent( EE::FileOf( world.Scene, UUID( 30 ) ) );
    ASSERT_TRUE( thirtyRead ) << thirtyRead.GetError();
    const std::string& thirtyBefore = thirtyRead.GetValue();

    world.Tags.erase( 30 ); // unloaded: the scene no longer holds it
    const std::vector<UUID> notLoaded{ UUID( 30 ) };
    world.Packages.AdoptRegion( world.Live(), notLoaded );
    EXPECT_FALSE( world.Packages.IsLoaded( UUID( 30 ) ) );

    world.Packages.Touch( UUID( 10 ) );
    world.Tags[10]   = "A2";
    world.Composed   = 0;
    const auto saved = world.Save( CleanCheck::AgainstFiles );
    EXPECT_FALSE( saved.Whole );
    EXPECT_EQ( saved.Files.Removed, 0u );
    const auto listed = EE::ListedEntities( world.Scene );
    ASSERT_TRUE( listed ) << listed.GetError();
    EXPECT_EQ( listed.GetValue(), ( std::vector<UUID>{ UUID( 10 ), UUID( 20 ), UUID( 30 ) } ) );
    const auto thirtyAfter = Common::Utils::FileSystem::ReadFileContent( EE::FileOf( world.Scene, UUID( 30 ) ) );
    ASSERT_TRUE( thirtyAfter ) << thirtyAfter.GetError();
    EXPECT_EQ( thirtyAfter.GetValue(), thirtyBefore );
    EXPECT_NE( world.Joined().find( "\"A2\"" ), std::string::npos );

    // An edit that names no entity is still a delta of the loaded ones: 30 is not written, not removed.
    world.Packages.TouchAll();
    const auto all = world.Save();
    EXPECT_FALSE( all.Whole );
    EXPECT_EQ( all.Files.Removed, 0u );
    EXPECT_TRUE( std::filesystem::exists( EE::FileOf( world.Scene, UUID( 30 ) ) ) );
}

TEST( EntityPackages, ASaveElsewhereOfAWorldHeldInPartIsRefused )
{
    ModelWorld world;
    world.Save();
    world.Packages.Baseline( world.Scene, world.Live() );
    world.Tags.erase( 30 );
    const std::vector<UUID> notLoaded{ UUID( 30 ) };
    world.Packages.AdoptRegion( world.Live(), notLoaded );
    world.Scene      = world.Root / "Elsewhere.desce";
    const auto saved = world.TrySave( CleanCheck::Trust );
    ASSERT_FALSE( saved );
    EXPECT_NE( saved.GetError().find( "not loaded" ), std::string::npos ) << saved.GetError();
    EXPECT_FALSE( std::filesystem::exists( world.Scene ) );
}
