#include "AssetFieldOpen.hpp"

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>

#include <ImGui/imgui.h>

namespace Desert::Editor
{
    void DrawAssetFieldOpen( uint64_t handle )
    {
        if ( handle == 0 )
            return;

        // The field's rectangle, tested by hand rather than through IsItemHovered: after EndGroup the last
        // item is the GROUP, and the widget inside it that owns the hover (the slot button, the combo) would
        // make IsItemHovered answer "no" for exactly the pixels the user is pointing at.
        const bool hovered = ImGui::IsWindowHovered( ImGuiHoveredFlags_AllowWhenBlockedByActiveItem ) &&
                             ImGui::IsMouseHoveringRect( ImGui::GetItemRectMin(), ImGui::GetItemRectMax() );

        const Assets::AssetHandle asset( handle );
        if ( hovered && ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
            Core::AssetFieldRequests::Request( asset, Core::AssetFieldAction::Open );

        if ( hovered && ImGui::IsMouseReleased( ImGuiMouseButton_Right ) )
            ImGui::OpenPopup( "##asset_field_open" );
        if ( ImGui::BeginPopup( "##asset_field_open" ) )
        {
            if ( ImGui::MenuItem( ICON_MDI_OPEN_IN_NEW "  Open" ) )
                Core::AssetFieldRequests::Request( asset, Core::AssetFieldAction::Open );
            if ( ImGui::MenuItem( ICON_MDI_FOLDER_SEARCH_OUTLINE "  Show in browser" ) )
                Core::AssetFieldRequests::Request( asset, Core::AssetFieldAction::ShowInBrowser );
            ImGui::EndPopup();
        }
    }

    void DrawAssetFieldButtons( uint64_t handle )
    {
        const Assets::AssetHandle asset( handle );
        ImGui::BeginDisabled( handle == 0 );
        ImGui::SameLine();
        if ( ImGui::SmallButton( ICON_MDI_OPEN_IN_NEW "##asset_field_open_button" ) )
            Core::AssetFieldRequests::Request( asset, Core::AssetFieldAction::Open );
        if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            ImGui::SetTooltip( "Open in the asset editor" );
        ImGui::SameLine();
        if ( ImGui::SmallButton( ICON_MDI_FOLDER_SEARCH_OUTLINE "##asset_field_browse_button" ) )
            Core::AssetFieldRequests::Request( asset, Core::AssetFieldAction::ShowInBrowser );
        if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            ImGui::SetTooltip( "Show in the Content Browser" );
        ImGui::EndDisabled();
    }
} // namespace Desert::Editor
