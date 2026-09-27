// WHERE A PREFAB MAY BE PUT, AND WHAT IT SAYS WHEN IT MAY NOT.
//
// The question Ю19 had to answer was "is a UI prefab the same prefab?", and the measured answer is yes
// everywhere except one place: a UI element is drawn only by the walk that starts at a UICanvas
// (UICanvasLayout::EnumerateCanvas), so a UI prefab instantiated the way every prefab was instantiated
// before — at the scene root — produced entities that were present, selectable, correctly serialized,
// and covered NO PIXELS, with nothing logged. One argument (the parent) and one rule (this file) is the
// whole of the difference.
//
// THE WORDING IS PART OF THE DELIVERABLE and is asserted, not just the boolean. The behaviour being
// replaced was silence; a refusal that does not say which file, where it was going and what to do
// instead is the same silence with an extra step.

#include <gtest/gtest.h>

#include <Common/Json/Json.hpp>
#include <Engine/Assets/Prefab/PrefabPlacement.hpp>

#include <string>

using Desert::Assets::CheckPrefabPlacement;
using Desert::Assets::ClassifyPrefabRoot;
using Desert::Assets::EntityData;
using Desert::Assets::PrefabRootKind;

namespace
{
    EntityData RecordWith( const std::vector<std::string>& componentKeys )
    {
        EntityData data;
        for ( const std::string& key : componentKeys )
        {
            data.Components[key] = Common::Json::Value( Common::Json::Object{} );
        }
        return data;
    }

    constexpr bool kUnderCanvas    = true;
    constexpr bool kNotUnderCanvas = false;
} // namespace

// --- Classification ---------------------------------------------------------------------------------

TEST( PrefabPlacement, TheRootIsClassifiedByTheKeysTheRegistryWrote )
{
    EXPECT_EQ( ClassifyPrefabRoot( RecordWith( { "StaticMesh" } ) ), PrefabRootKind::World );
    EXPECT_EQ( ClassifyPrefabRoot( RecordWith( { "UILayout", "UIPanel" } ) ), PrefabRootKind::UIElement );
    EXPECT_EQ( ClassifyPrefabRoot( RecordWith( { "UICanvas" } ) ), PrefabRootKind::UICanvas );

    // A canvas entity carries a UILayout too in most authored trees. Canvas wins, because a canvas is a
    // UI tree's ROOT and the two kinds have opposite placement rules — reading the layout first would
    // give a canvas prefab the element's verdict and let one be nested inside another.
    EXPECT_EQ( ClassifyPrefabRoot( RecordWith( { "UILayout", "UICanvas" } ) ), PrefabRootKind::UICanvas );

    // An entity with no component payloads at all is a world entity: a bare transform is legal and has
    // no canvas requirement.
    EXPECT_EQ( ClassifyPrefabRoot( EntityData{} ), PrefabRootKind::World );
}

// --- The verdict ------------------------------------------------------------------------------------

TEST( PrefabPlacement, AUIElementOutsideACanvasIsRefused )
{
    const auto verdict = CheckPrefabPlacement( PrefabRootKind::UIElement, kNotUnderCanvas,
                                               "Prefabs/Button.deprefab", "the scene root" );
    ASSERT_FALSE( verdict.Allowed );

    // The four things a reader must have to act without opening any source: which file, what it is,
    // where it was going, and what to do instead.
    EXPECT_NE( verdict.Refusal.find( "Prefabs/Button.deprefab" ), std::string::npos ) << verdict.Refusal;
    EXPECT_NE( verdict.Refusal.find( "the scene root" ), std::string::npos ) << verdict.Refusal;
    EXPECT_NE( verdict.Refusal.find( "NO PIXELS" ), std::string::npos ) << verdict.Refusal;
    EXPECT_NE( verdict.Refusal.find( "NOTHING WAS CREATED" ), std::string::npos ) << verdict.Refusal;
    EXPECT_NE( verdict.Refusal.find( "Canvas" ), std::string::npos ) << verdict.Refusal;
}

TEST( PrefabPlacement, AUIElementInsideACanvasIsAllowedAndSaysNothing )
{
    const auto verdict =
         CheckPrefabPlacement( PrefabRootKind::UIElement, kUnderCanvas, "Prefabs/Button.deprefab", "'HUD'" );
    EXPECT_TRUE( verdict.Allowed );
    EXPECT_TRUE( verdict.Refusal.empty() ) << "an allowed placement that carries a message is a warning "
                                              "nobody asked for, and warnings nobody asked for are how "
                                              "real ones stop being read";
}

TEST( PrefabPlacement, ACanvasInsideACanvasIsRefused )
{
    const auto verdict =
         CheckPrefabPlacement( PrefabRootKind::UICanvas, kUnderCanvas, "Prefabs/PauseMenu.deprefab", "'HUD'" );
    ASSERT_FALSE( verdict.Allowed );
    // The mechanism, not just the prohibition: the outer canvas's walk descends into it AND
    // CanvasesInDrawOrder returns it as a root of its own, so every element in it is drawn and
    // hit-tested twice.
    EXPECT_NE( verdict.Refusal.find( "twice" ), std::string::npos ) << verdict.Refusal;
    EXPECT_NE( verdict.Refusal.find( "NOTHING WAS CREATED" ), std::string::npos ) << verdict.Refusal;
}

TEST( PrefabPlacement, ACanvasAtTheSceneRootIsAllowed )
{
    EXPECT_TRUE( CheckPrefabPlacement( PrefabRootKind::UICanvas, kNotUnderCanvas, "Prefabs/PauseMenu.deprefab",
                                       "the scene root" )
                      .Allowed );
}

TEST( PrefabPlacement, AWorldPrefabIsAllowedOnBothSides )
{
    // This is a DECISION rather than an omission. A canvas is an ordinary entity with children, and a
    // mesh parented into one keeps its own transform and renders through the 3D path exactly as it
    // would anywhere else. Refusing it would forbid a legal arrangement in order to guess at an intent.
    EXPECT_TRUE(
         CheckPrefabPlacement( PrefabRootKind::World, kNotUnderCanvas, "Prefabs/Crate.deprefab", "the scene root" )
              .Allowed );
    EXPECT_TRUE(
         CheckPrefabPlacement( PrefabRootKind::World, kUnderCanvas, "Prefabs/Crate.deprefab", "'HUD'" ).Allowed );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
