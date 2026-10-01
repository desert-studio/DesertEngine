#include "Editor/LevelEditor/WindowTitles.hpp"
#include "Editor/Core/IconsMaterialDesignIcons.hpp"
#include <string>



namespace Desert::Editor
{
    // Icon shown before a panel's tab/title + its View-menu entry. Keyed by the panel's STABLE name
    // (GetName(), which is also the ImGui dock ID) so we never touch that ID.
    const char* PanelIcon( const std::string& name )
    {
        if ( name == "Scene###scene" )
            return ICON_MDI_MONITOR;
        if ( name == "Scene Outliner" )
            return ICON_MDI_FILE_TREE;
        if ( name == "Details" )
            return ICON_MDI_TUNE;
        if ( name == "Assets" )
            return ICON_MDI_FOLDER_OUTLINE;
        if ( name == "World Settings" )
            return ICON_MDI_COG;
        if ( name == "Scalability" )
            return ICON_MDI_TUNE;
        if ( name == "Logs" )
            return ICON_MDI_TEXT_BOX_OUTLINE;
        if ( name == "History" )
            return ICON_MDI_HISTORY;
        if ( name == "Collections" )
            return ICON_MDI_SHAPE_OUTLINE;
        if ( name == "Anim Layers" )
            return ICON_MDI_ANIMATION;
        if ( name == "Model from Photos" )
            return ICON_MDI_CUBE_SCAN;
        // "Anim Graph", "Node Graph", "Particle Editor", "UI Editor" and "Sequencer" were here. They are
        // DOCUMENTS now,
        // and a document's icon comes from its registration rather than from a table keyed on a panel name
        // — this table can only ever match a tool's constant name, and a document is named after the thing
        // it edits. See SubjectEditorRegistry::Registration::Icon.
        if ( name == "Lua Console" )
            return ICON_MDI_CONSOLE;
        if ( name == "Build Settings" )
            return ICON_MDI_HAMMER_WRENCH;
        if ( name == "Asset References" )
            return ICON_MDI_LINK_VARIANT;
        if ( name == "Scene Validation" )
            return ICON_MDI_CLIPBOARD_CHECK_OUTLINE;
        if ( name == "Shader Library" )
            return ICON_MDI_PALETTE;
        return ICON_MDI_VIEW_DASHBOARD; // sensible default for any future panel
    }

    // Composes "<icon>  <label>###<stable id>". The visible part gets the icon; the trailing ###<name>
    // keeps the ImGui window ID EXACTLY panel->GetName(), so saved dock layouts and every GetName()==...
    // lookup keep working unchanged.
    std::string PanelDisplayTitle( const std::string& name )
    {
        std::string label = name;
        if ( const auto pos = label.find( "###" ); pos != std::string::npos )
            label.erase( pos ); // visible part only (drop any existing ###id)
        return IconWindowTitle( PanelIcon( name ), label, name );
    }
} // namespace Desert::Editor
