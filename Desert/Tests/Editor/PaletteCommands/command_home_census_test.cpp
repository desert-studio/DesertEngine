// EVERY ACTION A MOUSE REACHES IS A COMMAND A SCRIPT REACHES (UE: FUICommandInfo + FUICommandList).
//
// The 09-30 live checks could not Play the animation editor, run the Content Browser's right-click actions,
// clear its selection, drop the scene selection or frame the selected entity: those existed only as a button,
// a menu row or a key. They are now commands (Editor/Core/UICommandInfo.hpp): the menu row, the button, the
// key and the palette / control channel all call one executor. This suite pins both halves — the command
// tables (compiled in) and the census that the surfaces call the commands rather than a second copy of the
// body (read from the sources, as LandscapePaletteCensus does).

#include <Editor/Panels/FileExplorer/ContentBrowserCommands.hpp>
#include <Editor/Panels/ViewportPanel/ViewportCommands.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

using namespace Desert::Editor;

namespace
{
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            const std::ifstream probe( std::filesystem::path( prefix ) / "Editor/Source/EditorLayer.cpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const std::string& relative )
    {
        const std::ifstream in( RepoRoot() + relative );
        if ( !in )
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // The text of one function: from its definition line to the next member definition of the same class.
    std::string FunctionBody( const std::string& source, const std::string& signature, const std::string& nextOf )
    {
        const auto begin = source.find( signature );
        if ( begin == std::string::npos )
            return {};
        const auto end = source.find( nextOf, begin + signature.size() );
        return source.substr( begin, end == std::string::npos ? std::string::npos : end - begin );
    }

    constexpr const char* kFileExplorer = "Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.cpp";
    constexpr const char* kViewport     = "Editor/Source/Editor/Panels/ViewportPanel/ViewportPanel.cpp";
    constexpr const char* kAnimDocument =
         "Editor/Source/Editor/Panels/AnimationEditor/AnimationEditorDocument.cpp";
    constexpr const char* kLayer = "Editor/Source/EditorLayer.cpp";
} // namespace

TEST( CommandHome, EveryCommandHasALabelUniqueInItsContext )
{
    std::set<std::string_view> labels;
    for ( const ContentBrowserCommand command : kContentBrowserCommandOrder )
    {
        EXPECT_EQ( CommandInfo( command ).Context, kContentBrowserContext );
        EXPECT_FALSE( CommandInfo( command ).Label.empty() );
        EXPECT_TRUE( labels.insert( CommandInfo( command ).Label ).second ) << CommandInfo( command ).Label;
    }
    labels.clear();
    for ( const ViewportCommand command : kViewportCommandOrder )
    {
        EXPECT_EQ( CommandInfo( command ).Context, kViewportContext );
        EXPECT_FALSE( CommandInfo( command ).Label.empty() );
        EXPECT_TRUE( labels.insert( CommandInfo( command ).Label ).second ) << CommandInfo( command ).Label;
    }
}

TEST( CommandHome, TheSourcesAreFound )
{
    ASSERT_FALSE( RepoRoot().empty() ) << "run from inside the repository";
    for ( const char* file : { kFileExplorer, kViewport, kAnimDocument, kLayer } )
        EXPECT_FALSE( ReadFile( file ).empty() ) << file;
}

// The context menu's action rows are the commands — never a MenuItem with the command's words and a second
// body under it.
TEST( CommandHome, TheContentBrowserMenuRowsAreCommands )
{
    const std::string menu = FunctionBody( ReadFile( kFileExplorer ),
                                           "void FileExplorerPanel::DrawItemContextMenu(", "FileExplorerPanel::" );
    ASSERT_FALSE( menu.empty() );
    for ( const ContentBrowserCommand command : kContentBrowserCommandOrder )
    {
        const std::string label( CommandInfo( command ).Label );
        EXPECT_EQ( menu.find( "MenuItem( \"" + label + "\"" ), std::string::npos )
             << "'" << label << "' is drawn as its own MenuItem, not as the command";
    }
    for ( const char* name : { "Open", "ShowInExplorer", "OpenContainingFolder", "Reimport", "ReimportWithNewFile",
                               "CaptureThumbnail", "EditThumbnail" } )
        EXPECT_NE( menu.find( std::string( "CommandMenuItem( ContentBrowserCommand::" ) + name ),
                   std::string::npos )
             << name << " is not a row of the context menu";
}

TEST( CommandHome, ThePaletteOffersEveryTable )
{
    const std::string layer = ReadFile( kLayer );
    EXPECT_NE( layer.find( "Editor::kContentBrowserCommandOrder" ), std::string::npos );
    EXPECT_NE( layer.find( "&FileExplorerPanel::RunCommand" ), std::string::npos );
    EXPECT_NE( layer.find( "Editor::kViewportCommandOrder" ), std::string::npos );
    EXPECT_NE( layer.find( "&Editor::ViewportPanel::RequestCommand" ), std::string::npos );
    // The document's palette actions are the transport commands (and so reach the channel as
    // "Document / <name>: Play/Pause").
    const std::string actions = FunctionBody( ReadFile( kAnimDocument ), "AnimationEditorDocument::Actions()",
                                              "AnimationEditorDocument::" );
    EXPECT_NE( actions.find( "kTransportCommandOrder" ), std::string::npos );
}

TEST( CommandHome, TheTransportButtonsRunTheCommands )
{
    const std::string transport = FunctionBody(
         ReadFile( kAnimDocument ), "void AnimationEditorDocument::DrawTransport()", "AnimationEditorDocument::" );
    ASSERT_FALSE( transport.empty() );
    for ( const char* body : { "t.TogglePlay()", "t.StepFrames(", "t.ToStart()", "t.ToEnd()" } )
        EXPECT_EQ( transport.find( body ), std::string::npos )
             << body << " is a second home of a transport command";
    for ( const char* name : { "ToStart", "PreviousFrame", "PlayPause", "NextFrame", "ToEnd" } )
        EXPECT_NE( transport.find( std::string( "transportButton( TransportCommand::" ) + name ),
                   std::string::npos )
             << name << " has no button";
}

TEST( CommandHome, TheViewportKeysRunTheCommands )
{
    const std::string keys =
         FunctionBody( ReadFile( kViewport ), "bool ViewportPanel::OnKeyPressedEvent(", "ViewportPanel::" );
    ASSERT_FALSE( keys.empty() );
    EXPECT_NE( keys.find( "RunCommand( ViewportCommand::FocusSelected )" ), std::string::npos );
    EXPECT_NE( keys.find( "RunCommand( ViewportCommand::SelectNone )" ), std::string::npos );
    EXPECT_EQ( keys.find( "->Focus(" ), std::string::npos ) << "F frames the selection itself again";
    EXPECT_EQ( keys.find( "SelectionManager::ClearSelection" ), std::string::npos );
}

// MCP-CMD2: UE's SyncBrowserToFolders / SyncBrowserToAssets. "Select asset" reached only the open folder, so a
// client could not reach Materials/Fox/fox_material.demat. The labels carry the path; the palette offers one per
// folder / file under the root; Sync opens the folder and selects there, through the click's own executors.
TEST( ContentBrowserNavigation, GoToFolderAndSyncToAssetCarryThePathAndCallTheBrowser )
{
    EXPECT_EQ( ContentBrowserPathLabel( kGoToFolderLabel, "Assets/Materials/Fox" ),
               "Go to Folder Assets/Materials/Fox" );
    EXPECT_EQ( ContentBrowserPathLabel( kSyncToAssetLabel, "Assets/Materials/Fox/fox_material.demat" ),
               "Sync to Asset Assets/Materials/Fox/fox_material.demat" );

    const std::string layer = ReadFile( kLayer );
    EXPECT_NE( layer.find( "&FileExplorerPanel::GoToFolder" ), std::string::npos );
    EXPECT_NE( layer.find( "&FileExplorerPanel::SyncToAsset" ), std::string::npos );
    EXPECT_NE( layer.find( "->ContentFiles()" ), std::string::npos ) << "every file, not the open folder's";

    const std::string browser = ReadFile( kFileExplorer );
    const std::string sync    = FunctionBody( browser, "FileExplorerPanel::SyncToAsset(", "FileExplorerPanel::" );
    EXPECT_NE( sync.find( "NavigateToPath(" ), std::string::npos );
    EXPECT_NE( sync.find( "SelectEntry(" ), std::string::npos );
    const std::string go = FunctionBody( browser, "FileExplorerPanel::GoToFolder(", "FileExplorerPanel::" );
    EXPECT_NE( go.find( "NavigateToPath(" ), std::string::npos );

    // Edit Thumbnail asks the kind table, not a model-or-material list (Fox.skmesh was refused).
    const std::string run = FunctionBody( browser, "FileExplorerPanel::RunCommand(", "FileExplorerPanel::" );
    EXPECT_NE( run.find( "HasThumbnailOrbit" ), std::string::npos );
    EXPECT_EQ( run.find( "only a model or a material has a thumbnail orbit" ), std::string::npos );
}
