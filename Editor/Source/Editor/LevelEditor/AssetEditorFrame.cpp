#include "AssetEditorFrame.hpp"

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ThemeManager.hpp>
#include <Editor/LevelEditor/LevelToolbar.hpp>
#include <Editor/Panels/AssetEditorToolbar.hpp>
#include <Editor/Panels/IPanel.hpp>

#include <Engine/Assets/AssetMetadata.hpp>

#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <format>
#include <string>

namespace Desert::Editor::AssetEditorFrame
{
    namespace
    {
        // The level toolbar's strip (LevelToolbar::Draw): #161616, the same padding and frame metrics, so an asset
        // editor's bar and the level's bar are one look.
        constexpr ImVec4 kStripColour{ 0.086f, 0.086f, 0.086f, 1.0f };

        float StatusBarHeight()
        {
            return ImGui::GetTextLineHeight() + 8.0f;
        }

        void PushStrip()
        {
            ImGui::PushStyleColor( ImGuiCol_ChildBg, kStripColour );
            ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 8.0f, 4.0f ) );
            ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 2.0f, 0.0f ) );
            ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 8.0f, 5.0f ) );
            ImGui::PushStyleVar( ImGuiStyleVar_FrameRounding, 4.0f );
        }

        void PopStrip()
        {
            ImGui::PopStyleVar( 4 );
            ImGui::PopStyleColor();
        }

        void Separator()
        {
            ImGui::SameLine( 0.0f, 6.0f );
            LevelToolbar::ToolbarSeparatorAt( ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y );
            ImGui::Dummy( ImVec2( 1.0f, ImGui::GetFrameHeight() ) );
            ImGui::SameLine( 0.0f, 6.0f );
        }

        void DrawEntry( const AssetEditorToolbar::Entry& entry, const std::size_t index )
        {
            using Kind = AssetEditorToolbar::Entry::Kind;
            switch ( entry.Type )
            {
                case Kind::Separator:
                    Separator();
                    return;
                case Kind::Button:
                {
                    const bool checked = entry.Checked && entry.Checked();
                    const bool enabled = !entry.Enabled || entry.Enabled();
                    if ( LevelToolbar::ToolbarButton( entry.Icon.c_str(), entry.Label.c_str(), checked,
                                                      entry.Tooltip.empty() ? nullptr : entry.Tooltip.c_str(),
                                                      enabled ) &&
                         entry.Run )
                        entry.Run();
                    ImGui::SameLine();
                    return;
                }
                case Kind::Combo:
                {
                    // A drop-down with the button's own face: the current choice and a chevron; the list is a
                    // popup under it (UE's toolbar combo button).
                    const std::string face  = std::format( "{}  " ICON_MDI_CHEVRON_DOWN, entry.Current );
                    const std::string popup = std::format( "##assettoolbarcombo{}", index );
                    if ( LevelToolbar::ToolbarButton( entry.Icon.c_str(), face.c_str(), false,
                                                      entry.Tooltip.empty() ? nullptr : entry.Tooltip.c_str() ) )
                        ImGui::OpenPopup( popup.c_str() );
                    const ImVec2 under( ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y );
                    ImGui::SameLine();
                    ImGui::SetNextWindowPos( under );
                    if ( ImGui::BeginPopup( popup.c_str() ) )
                    {
                        for ( std::size_t i = 0; i < entry.Choices.size(); ++i )
                        {
                            if ( ImGui::Selectable( entry.Choices[i].Label.c_str(), i == entry.Chosen ) &&
                                 entry.Choices[i].Run )
                                entry.Choices[i].Run();
                        }
                        ImGui::EndPopup();
                    }
                    return;
                }
            }
        }

        const char* DiskStateWord( const ISubjectDocument::DiskState state )
        {
            switch ( state )
            {
                case ISubjectDocument::DiskState::Clean:
                    return "Saved";
                case ISubjectDocument::DiskState::Dirty:
                    return "Modified";
                case ISubjectDocument::DiskState::Untracked:
                    return "Not tracked"; // UNTRACKED IS NOT CLEAN (IPanel.hpp): the frame does not say "Saved"
            }
            return "";
        }
    } // namespace

    void DrawToolbar( ISubjectDocument& document, const Assets::AssetMetadata* asset )
    {
        PushStrip();
        ImGui::BeginChild( "##assettoolbar", ImVec2( 0.0f, ImGui::GetFrameHeight() + 8.0f ), false,
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse );

        // SAVE: offered unless the document says its file already matches. Untracked is offered too — the
        // document cannot tell, so the person is not told there is nothing to save.
        const ISubjectDocument::DiskState disk = document.GetDiskState();
        if ( LevelToolbar::ToolbarButton( ICON_MDI_CONTENT_SAVE, "Save", false, "Save this asset (Ctrl+S)",
                                          disk != ISubjectDocument::DiskState::Clean ) &&
             !document.SaveDocument() )
            LOG_WARN( "[Documents] '{}': Save wrote no file — this editor has nothing of its own to write.",
                      document.GetName() );
        ImGui::SameLine();

        // BROWSE: the Assets browser opens the asset's folder and selects it (UE: SyncBrowserToAssets). Queued
        // like a Details field's Browse — the browser is EditorLayer's, not this frame's.
        const bool known = asset != nullptr && !asset->Filepath.empty();
        if ( LevelToolbar::ToolbarButton( ICON_MDI_FOLDER_SEARCH_OUTLINE, "Browse", false,
                                          known ? "Find this asset in the Assets browser"
                                                : "This asset is not registered with the asset manager",
                                          known ) )
            Core::AssetFieldRequests::Request( Assets::AssetHandle( document.Subject().Owner ),
                                               Core::AssetFieldAction::BrowseTo );
        ImGui::SameLine();

        AssetEditorToolbar toolbar;
        document.ExtendToolbar( toolbar );
        if ( !toolbar.Entries().empty() )
            Separator();
        for ( std::size_t i = 0; i < toolbar.Entries().size(); ++i )
            DrawEntry( toolbar.Entries()[i], i );

        ImGui::EndChild();
        PopStrip();
    }

    bool BeginBody()
    {
        const float height =
             ImGui::GetContentRegionAvail().y - StatusBarHeight() - ImGui::GetStyle().ItemSpacing.y;
        return ImGui::BeginChild( "##assetbody", ImVec2( 0.0f, std::max( height, 1.0f ) ), false,
                                  ImGuiWindowFlags_None );
    }

    void EndBody()
    {
        ImGui::EndChild();
    }

    void DrawStatusBar( const ISubjectDocument& document, const Assets::AssetMetadata* asset )
    {
        PushStrip();
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 8.0f, 4.0f ) );
        ImGui::BeginChild( "##assetstatus", ImVec2( 0.0f, StatusBarHeight() ), false,
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse );

        const std::string path =
             asset != nullptr ? asset->Filepath.generic_string() : std::string( "(unregistered)" );
        ImGui::PushStyleColor( ImGuiCol_Text, ThemeManager::GetIconColor() );
        ImGui::TextUnformatted( ICON_MDI_FILE_OUTLINE );
        ImGui::PopStyleColor();
        ImGui::SameLine( 0.0f, 6.0f );
        ImGui::TextDisabled( "%s", path.c_str() );

        const ISubjectDocument::DiskState disk = document.GetDiskState();
        ImGui::SameLine( 0.0f, 12.0f );
        if ( disk == ISubjectDocument::DiskState::Dirty )
            ImGui::TextColored( ThemeManager::GetWarningColor(), ICON_MDI_CIRCLE_MEDIUM " %s",
                                DiskStateWord( disk ) );
        else
            ImGui::TextDisabled( "%s", DiskStateWord( disk ) );

        if ( const std::string status = document.StatusText(); !status.empty() )
        {
            const float width = ImGui::CalcTextSize( status.c_str() ).x;
            const float right = ImGui::GetWindowContentRegionMax().x - width;
            ImGui::SameLine( std::max( right, ImGui::GetCursorPosX() + 12.0f ) );
            ImGui::TextUnformatted( status.c_str() );
        }

        ImGui::EndChild();
        ImGui::PopStyleVar();
        PopStrip();
    }
} // namespace Desert::Editor::AssetEditorFrame
