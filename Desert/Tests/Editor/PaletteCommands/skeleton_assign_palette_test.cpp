// "Assets | Assign Skeleton…" (SKEL-cmd): the census of entries the palette offers for the Assets selection,
// and that each entry hands its own (subject, skeleton) pair to the assignment and passes its refusal through.
#include <Editor/Core/SkeletonAssignPalette.hpp>

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace
{
    using Desert::Editor::SkeletonAssignPaletteCommands;

    const Desert::Editor::SkeletonAssignFn kNeverCalled = []( const std::string&, const std::string& )
    { return Desert::Common::MakeError( "the assignment must not run while the census is taken" ); };

    std::vector<std::string> Labels( const std::vector<std::string>& selected,
                                     const std::vector<std::string>& skeletons )
    {
        std::vector<std::string> labels;
        for ( const auto& command : SkeletonAssignPaletteCommands( selected, skeletons, kNeverCalled ) )
            labels.push_back( command.Group + "/" + command.Label );
        return labels;
    }
} // namespace

TEST( SkeletonAssignPalette, EverySelectedMeshAndClipMeetsEveryRegisteredSkeleton )
{
    EXPECT_EQ(
         Labels( { "Assets/Cesium/CesiumMan.skmesh", "Assets/Cesium/Walk.anim", "Assets/Cesium/CesiumMan.png" },
                 { "Assets/Fox/Fox.skeleton", "Assets/Cesium/CesiumMan.skeleton" } ),
         ( std::vector<std::string>{ "Assets/Assign Skeleton… CesiumMan.skmesh <- Fox.skeleton",
                                     "Assets/Assign Skeleton… CesiumMan.skmesh <- CesiumMan.skeleton",
                                     "Assets/Assign Skeleton… Walk.anim <- Fox.skeleton",
                                     "Assets/Assign Skeleton… Walk.anim <- CesiumMan.skeleton" } ) );
}

TEST( SkeletonAssignPalette, WithNothingToAssignTheOneEntryStaysAndSaysWhy )
{
    const auto noSubject = SkeletonAssignPaletteCommands( { "Assets/Fox/Fox.skeleton" },
                                                          { "Assets/Fox/Fox.skeleton" }, kNeverCalled );
    ASSERT_EQ( noSubject.size(), 1u );
    EXPECT_EQ( noSubject[0].Label, "Assign Skeleton…" );
    const auto refused = noSubject[0].Run();
    ASSERT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( "select a .skmesh or .anim" ), std::string::npos );

    const auto noSkeleton =
         SkeletonAssignPaletteCommands( { "Assets/Cesium/CesiumMan.skmesh" }, {}, kNeverCalled );
    ASSERT_EQ( noSkeleton.size(), 1u );
    const auto refusedToo = noSkeleton[0].Run();
    ASSERT_FALSE( refusedToo );
    EXPECT_NE( refusedToo.GetError().find( "no .skeleton is registered" ), std::string::npos );
}

TEST( SkeletonAssignPalette, AnEntryAssignsItsOwnPairAndCarriesTheRefusal )
{
    std::vector<std::pair<std::string, std::string>> calls;
    const Desert::Editor::SkeletonAssignFn assign = [&]( const std::string& subject, const std::string& skeleton )
    {
        calls.emplace_back( subject, skeleton );
        return Desert::Common::MakeError( "'CesiumMan.skmesh': missing bones: Skeleton_torso_joint_1" );
    };
    const auto commands = SkeletonAssignPaletteCommands( { "A/CesiumMan.skmesh" },
                                                         { "B/Fox.skeleton", "C/Other.skeleton" }, assign );
    ASSERT_EQ( commands.size(), 2u );
    const auto refused = commands[1].Run();
    ASSERT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( "missing bones" ), std::string::npos );
    EXPECT_EQ( calls, ( std::vector<std::pair<std::string, std::string>>{
                           { "A/CesiumMan.skmesh", "C/Other.skeleton" } } ) );
}
