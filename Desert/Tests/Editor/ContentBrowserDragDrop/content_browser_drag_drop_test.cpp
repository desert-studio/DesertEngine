// A drag onto a folder in the Content Browser moves the asset there (CB-DRAG-FOLDER). The defect this pins:
// the old tree target accepted a payload type no source emitted ("selectable") and moved into a folder path
// nothing ever set, so every drop was dead and, had it fired, would have moved into "".
#include <gtest/gtest.h>

#include <Editor/Panels/FileExplorer/ContentBrowserDragDrop.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace CB = Desert::Editor::ContentBrowserDragDrop;
using Desert::Editor::FileType;

namespace
{
    bool Accepted( const char* type )
    {
        return std::any_of( CB::MovablePayloads.begin(), CB::MovablePayloads.end(),
                            [type]( const char* t ) { return std::strcmp( t, type ) == 0; } );
    }
} // namespace

// Every payload a browser tile is dragged as is one a folder accepts: the source and the target read one table.
TEST( ContentBrowserDragDrop, EveryDragSourcePayloadIsAcceptedByAFolder )
{
    EXPECT_TRUE( Accepted( CB::PayloadTypeOf( false, FileType::Unknown ) ) );
    for ( int t = 0; t <= static_cast<int>( FileType::CookedWorld ); ++t )
        EXPECT_TRUE( Accepted( CB::PayloadTypeOf( true, static_cast<FileType>( t ) ) ) ) << "FileType " << t;
}

// The invariant: a drop moves into a NON-EMPTY folder; an empty target moves nothing.
TEST( ContentBrowserDragDrop, EmptyTargetMovesNothing )
{
    EXPECT_TRUE( CB::PlanFolderDrop( "Assets/A/rock.demat", {}, "" ).empty() );
    const auto moves = CB::PlanFolderDrop( "Assets/A/rock.demat", {}, "Assets/B" );
    ASSERT_EQ( moves.size(), 1u );
    EXPECT_EQ( moves[0], "Assets/A/rock.demat" );
}

TEST( ContentBrowserDragDrop, ASelectedTileCarriesTheSelection )
{
    const std::vector<std::string> sel = { "Assets/A/a.demat", "Assets/A/b.png" };
    EXPECT_EQ( CB::PlanFolderDrop( "Assets/A/b.png", sel, "Assets/B" ), sel );
    // A tile outside the selection drags alone (UE).
    EXPECT_EQ( CB::PlanFolderDrop( "Assets/A/c.png", sel, "Assets/B" ),
               std::vector<std::string>{ "Assets/A/c.png" } );
}

TEST( ContentBrowserDragDrop, RefusesNoOpAndSelfNestingMoves )
{
    EXPECT_TRUE( CB::PlanFolderDrop( "Assets/A/a.demat", {}, "Assets/A" ).empty() );  // already there
    EXPECT_TRUE( CB::PlanFolderDrop( "Assets/A", {}, "Assets/A" ).empty() );          // onto itself
    EXPECT_TRUE( CB::PlanFolderDrop( "Assets/A", {}, "Assets/A/Sub/Deep" ).empty() ); // into its own subtree
    EXPECT_EQ( CB::PlanFolderDrop( "Assets/A", {}, "Assets/AB" ).size(), 1u ); // a sibling prefix is no child
}
