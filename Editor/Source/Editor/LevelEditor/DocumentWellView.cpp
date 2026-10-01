#include "Common/Core/Logger.hpp"
#include "Editor/Core/DocumentWell.hpp"
#include "Editor/Core/EditorSubject.hpp"
#include "Editor/Core/IconsMaterialDesignIcons.hpp"
#include "Editor/Panels/IPanel.hpp"
#include <cstddef>
#include <functional>
#include <glm/ext/vector_float2.hpp>
#include <imgui.h>
#include <optional>
#include <Common/Core/Profiler.hpp>
#include "Editor/Core/PanelRequests.hpp"
#include "Editor/Core/EditorResources.hpp"
#include "Editor/Core/ThemeManager.hpp"
#include "Editor/Core/ImGuiUtilities.hpp"
#include <ImGui/imgui_internal.h>
#include <array>
#include "Editor/Panels/Clouds/CloudsPanel.hpp"
#include "Editor/Core/SubjectEditorRegistry.hpp"
#include "Editor/Core/SubjectOpenRequest.hpp"
#include <Editor/Core/DocumentPlacement.hpp>
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>
#include "Editor/LevelEditor/DocumentHost.hpp"
#include "Editor/LevelEditor/WindowTitles.hpp"
#include "Engine/Core/ViewBudget.hpp"

namespace Desert::Editor
{
    // THE DOCUMENT WELL'S OWN WINDOW: the index of open documents. It is not where they open — a document opens
    // as a tab beside the level viewport (Editor/Core/DocumentPlacement.hpp).
    static constexpr const char* kDocumentWellWindow =
         ICON_MDI_FILE_DOCUMENT_MULTIPLE_OUTLINE "  Documents###documentwell";

    const char* DocumentHost::WellWindowTitle()
    {
        return kDocumentWellWindow;
    }

    void DocumentHost::RegisterDocumentWellLayoutHandler()
    {
        // "[DocumentWell][Window]\nOpen=0|1" in imgui.ini and in every saved layout. ReadInit runs before
        // each load, so a layout without the entry (the default, or one saved earlier) means open.
        ImGuiSettingsHandler handler;
        handler.TypeName   = "DocumentWell";
        handler.TypeHash   = ImHashStr( handler.TypeName );
        handler.UserData   = this;
        handler.ReadInitFn = []( ImGuiContext*, ImGuiSettingsHandler* self )
        {
            auto* layer = static_cast<DocumentHost*>( self->UserData );
            layer->m_DocumentWell.ShowWindow();
            layer->m_DocumentWellOpenInLayout = true;
        };
        handler.ReadOpenFn = []( ImGuiContext*, ImGuiSettingsHandler* self, const char* ) -> void*
        { return self->UserData; };
        handler.ReadLineFn = []( ImGuiContext*, ImGuiSettingsHandler* self, void*, const char* line )
        {
            auto* layer = static_cast<DocumentHost*>( self->UserData );
            if ( std::string_view( line ).empty() )
                return;
            if ( !layer->m_DocumentWell.ReadLayoutLine( line ) )
                LOG_WARN( "[Editor] The layout's [DocumentWell] entry has an unknown line '{}'; the Documents "
                          "window keeps its current state ({}).",
                          line, layer->m_DocumentWell.LayoutLine() );
            layer->m_DocumentWellOpenInLayout = layer->m_DocumentWell.IsWindowOpen();
        };
        handler.WriteAllFn = []( ImGuiContext*, ImGuiSettingsHandler* self, ImGuiTextBuffer* out )
        {
            auto*                  layer = static_cast<DocumentHost*>( self->UserData );
            const std::string_view line  = layer->m_DocumentWell.LayoutLine();
            out->appendf( "[DocumentWell][Window]\n%.*s\n\n", static_cast<int>( line.size() ), line.data() );
            layer->m_DocumentWellOpenInLayout = layer->m_DocumentWell.IsWindowOpen();
        };
        ::ImGui::AddSettingsHandler( &handler );
    }

    namespace
    {
        // The per-kind key of a document's remembered placement: "<domain>:<facet>" — the Material Editor of
        // every material shares one, a mesh viewer another. The owner is left out on purpose.
        std::string DocumentKindKey( const SubjectId& subject )
        {
            return std::string( SubjectDomainName( subject.Domain ) ) + ':' + std::to_string( subject.Facet );
        }
    } // namespace

    void DocumentHost::RegisterDocumentPlacementHandler()
    {
        // "[DocumentPlacement][Kinds]\n<kind>=<DocumentPlacement::Format>" in imgui.ini and every saved layout.
        // ReadInit clears the map: a layout without the section means every kind opens beside the level.
        ImGuiSettingsHandler handler;
        handler.TypeName   = "DocumentPlacement";
        handler.TypeHash   = ImHashStr( handler.TypeName );
        handler.UserData   = this;
        handler.ReadInitFn = []( ImGuiContext*, ImGuiSettingsHandler* self )
        { static_cast<DocumentHost*>( self->UserData )->m_RememberedPlacement.clear(); };
        handler.ReadOpenFn = []( ImGuiContext*, ImGuiSettingsHandler* self, const char* ) -> void*
        { return self->UserData; };
        handler.ReadLineFn = []( ImGuiContext*, ImGuiSettingsHandler* self, void*, const char* line )
        {
            auto*                  layer = static_cast<DocumentHost*>( self->UserData );
            const std::string_view text( line );
            if ( text.empty() )
                return;
            const std::size_t eq         = text.find( '=' );
            const auto        remembered = eq == std::string_view::npos
                                                ? std::nullopt
                                                : DocumentPlacement::Parse( std::string( text.substr( eq + 1 ) ) );
            if ( eq == 0 || !remembered )
            {
                LOG_WARN( "[Editor] The layout's [DocumentPlacement] entry has an unreadable line '{}'; that "
                          "document kind opens beside the level viewport.",
                          line );
                return;
            }
            layer->m_RememberedPlacement[std::string( text.substr( 0, eq ) )] = *remembered;
        };
        handler.WriteAllFn = []( ImGuiContext*, ImGuiSettingsHandler* self, ImGuiTextBuffer* out )
        {
            auto* layer = static_cast<DocumentHost*>( self->UserData );
            out->appendf( "[DocumentPlacement][Kinds]\n" );
            for ( const auto& [kind, remembered] : layer->m_RememberedPlacement )
                out->appendf( "%s=%s\n", kind.c_str(), DocumentPlacement::Format( remembered ).c_str() );
            out->appendf( "\n" );
        };
        ::ImGui::AddSettingsHandler( &handler );
    }

    void DocumentHost::DrawDocumentWell()
    {
        namespace ImGui = ::ImGui;

        // CLOSED BY THE USER, NEVER BY ITSELF. The x hides the window and its neighbours take the area; it
        // does not collapse when it merely empties, because a node that appears and disappears resizes the
        // level view under the user's cursor. The window's state is a layout line, so a change marks the
        // layout for saving.
        if ( m_DocumentWell.IsWindowOpen() != m_DocumentWellOpenInLayout )
            ImGui::MarkIniSettingsDirty();
        if ( !m_DocumentWell.IsWindowOpen() )
            return;

        ImGui::Begin( WellWindowTitle(), &m_DocumentWell.WindowOpenFlag(), ImGuiWindowFlags_NoCollapse );

        if ( m_OpenDocuments.Empty() )
        {
            // THE EMPTY STATE SAYS WHAT THE AREA IS FOR. A reserved column that is blank most of the time
            // is a column nobody learns the purpose of; this is the price B.1 pays for stable geometry and
            // it is paid in words rather than in pixels.
            const float avail = ImGui::GetContentRegionAvail().x;

            ImGui::Dummy( ImVec2( 0.0f, 24.0f ) );
            {
                // The DEFAULT font, not the bold one: the icon range is merged into the default face only,
                // so the same glyph drawn in bold comes out as the missing-glyph box. (Measured — the first
                // capture of this empty state had a "?" where the document icon belongs.)
                const char* icon = ICON_MDI_FILE_DOCUMENT_OUTLINE;
                ImGui::SetCursorPosX( ImGui::GetCursorPosX() + ( avail - ImGui::CalcTextSize( icon ).x ) * 0.5f );
                ImGui::TextDisabled( "%s", icon );
            }

            ImGui::Dummy( ImVec2( 0.0f, 8.0f ) );
            {
                const char* title = "No document open";
                ImGui::PushFont( EditorResources::GetBoldFont() );
                ImGui::SetCursorPosX( ImGui::GetCursorPosX() + ( avail - ImGui::CalcTextSize( title ).x ) * 0.5f );
                ImGui::TextUnformatted( title );
                ImGui::PopFont();
            }

            ImGui::Dummy( ImVec2( 0.0f, 6.0f ) );
            {
                // EVERY door named, because none is discoverable from an empty area. There were two while
                // every document was a FILE; the third arrived with the component documents (U7, U7-2) and
                // is not an asset slot at all — the anim graph, the emitter, the UI canvas and the two
                // timelines are opened by a button beside the component that holds them, and a user
                // reading this list would otherwise have gone looking in the Content Browser for a file
                // that does not exist.
                const char* body = "Double-click a material, a cloud type, a noise volume or a layout in the "
                                   "Content Browser \xe2\x80\x94 press the pencil on any asset slot in "
                                   "Details \xe2\x80\x94 or, for an anim graph, an emitter, a UI canvas or "
                                   "a timeline, the button beside that component in Details.";
                ImGui::PushTextWrapPos( ImGui::GetCursorPosX() + avail );
                ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
                ImGui::TextUnformatted( body );
                ImGui::PopStyleColor();
                ImGui::PopTextWrapPos();
            }

            ImGui::Dummy( ImVec2( 0.0f, 10.0f ) );
            {
                const char*  label = ICON_MDI_FOLDER_MULTIPLE_OUTLINE "  Browse assets";
                const ImVec2 size( ImGui::CalcTextSize( label ).x + ImGui::GetStyle().FramePadding.x * 2.0f,
                                   0.0f );
                ImGui::SetCursorPosX( ImGui::GetCursorPosX() + ( avail - size.x ) * 0.5f );
                if ( ImGui::Button( label, size ) )
                    Core::PanelRequests::Open( "Assets" );
            }

            // RECENTLY CLOSED: the one thing an area that stays can offer that a vanishing one cannot.
            // Reopening goes through the ordinary open request, so it is refused by the slot cap exactly
            // like any other open and cannot become a second way in.
            if ( !m_DocumentWell.RecentlyClosed().empty() )
            {
                ImGui::Dummy( ImVec2( 0.0f, 12.0f ) );
                ImGui::Separator();
                ImGui::TextDisabled( "RECENTLY CLOSED" );
                for ( const ClosedDocument& closed : m_DocumentWell.RecentlyClosed() )
                {
                    ImGui::PushID( static_cast<int>( std::hash<SubjectId>{}( closed.Subject ) & 0x7fffffff ) );
                    const std::string row =
                         DocumentIcon( closed.Subject ) + "  " +
                         closed.DisplayName;
                    if ( ImGui::Selectable( row.c_str() ) )
                        Core::SubjectOpenRequests::Request( closed.Subject );
                    if ( ImGui::IsItemHovered() )
                        ImGui::SetTooltip( "Reopen this %s document",
                                           m_SubjectEditors.TypeName( closed.Subject ).c_str() );
                    ImGui::PopID();
                }
            }

            ImGui::End();
            return;
        }

        // SOMETHING IS OPEN: the well becomes the INDEX of the area it names. Past about six documents the
        // tab strip has the one you want off its end, so a list is not a fallback here — it is the primary
        // way to switch, and it carries the two facts a tab cannot: which type each document is, and
        // whether it is holding a view.
        ImGui::TextDisabled( "OPEN DOCUMENTS \xe2\x80\x94 %zu", m_OpenDocuments.Count() );
        ImGui::Separator();

        // Most recently used first, the same order Ctrl+Tab walks — one order, read in two places, so the
        // list cannot teach a different sequence from the key.
        std::vector<SubjectId> closeRequests;
        for ( const SubjectId& subject : m_DocumentWell.MostRecentOrder() )
        {
            const ISubjectDocument* document = m_OpenDocuments.Find( subject );
            if ( !document )
                continue;

            ImGui::PushID( static_cast<int>( std::hash<SubjectId>{}( subject ) & 0x7fffffff ) );

            const std::string row =
                 DocumentIcon( document->Subject() ) + "  " +
                 DocumentDisplayName( document->GetName() );
            if ( ImGui::Selectable( row.c_str(), subject == m_FocusedDocument,
                                    ImGuiSelectableFlags_AllowItemOverlap ) )
                FocusDocument( subject );

            // The view column. "Cloud - no view" is not trivia: it is the answer to "I closed four windows
            // and it still will not open", because closing a CPU-drawn document frees nothing.
            std::string view = "no view";
            if ( document->HoldsView() )
                view = "1 view";
            else if ( document->ClaimsView() )
                view = "claiming ~" + Engine::ViewBudget::FormatMiB( document->ViewForecastBytes() );
            const std::string right  = m_SubjectEditors.TypeName( document->Subject() ) + " \xc2\xb7 " + view;
            const float       rightW = ImGui::CalcTextSize( right.c_str() ).x;
            ImGui::SameLine( ImGui::GetContentRegionMax().x - rightW - 28.0f );
            ImGui::TextDisabled( "%s", right.c_str() );

            ImGui::SameLine( ImGui::GetContentRegionMax().x - 18.0f );
            if ( ImGui::SmallButton( ICON_MDI_CLOSE ) )
                closeRequests.push_back( subject );

            ImGui::PopID();
        }

        ImGui::End();

        // Requested after the loop: RequestDocumentClose only queues, but collecting first keeps the rule
        // that nothing mutates a container while it is being walked.
        for ( const SubjectId& subject : closeRequests )
            AskDocumentClose( subject, "closed from the Documents index" );
    }

    void DocumentHost::DrawMajorTabStrip()
    {
        namespace ImGui = ::ImGui;

        const auto isOpen = [this]( const SubjectId& subject )
        {
            return std::any_of( m_OpenDocuments.begin(), m_OpenDocuments.end(),
                                [&]( const auto& open ) { return open->Subject() == subject; } );
        };
        if ( MajorTabActive() && !isOpen( m_ActiveMajorTab ) )
            m_ActiveMajorTab = SubjectId{};
        std::erase_if( m_SeenMajorTabs, [&]( const SubjectId& seen ) { return !isOpen( seen ); } );

        std::vector<ISubjectDocument*> majors;
        for ( const auto& document : m_OpenDocuments )
            if ( document->OpensAsMajorTab() )
                majors.push_back( document.get() );
        // No strip while only the level is open, as UE shows none without an asset editor.
        if ( majors.empty() || !ImGui::BeginTabBar( "##majortabs", ImGuiTabBarFlags_Reorderable ) )
            return;

        SubjectId              selected;
        std::vector<SubjectId> closeRequests;
        if ( ImGui::BeginTabItem( ICON_MDI_MONITOR " Scene###majorscene" ) )
            ImGui::EndTabItem();
        for ( ISubjectDocument* document : majors )
        {
            // A tab seen for the first time comes to the front: opening an asset editor shows it, as in UE.
            const bool              fresh = m_SeenMajorTabs.insert( document->Subject() ).second;
            const bool              asked = !m_FocusWindow.empty() && document->GetName() == m_FocusWindow;
            bool                    open  = true;
            const ImGuiTabItemFlags flags =
                 fresh || asked ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if ( ImGui::BeginTabItem( DocumentDisplayTitle( *document ).c_str(), &open, flags ) )
            {
                selected = document->Subject();
                ImGui::EndTabItem();
            }
            if ( !open )
                closeRequests.push_back( document->Subject() );
        }
        ImGui::EndTabBar();
        m_ActiveMajorTab = selected;
        for ( const SubjectId& subject : closeRequests )
            AskDocumentClose( subject, "you closed the tab" );
    }

    void DocumentHost::DrawDocuments()
    {
        namespace ImGui = ::ImGui;

        std::vector<SubjectId> closeRequests;
        SubjectId              focused;

        // The level viewport's node, READ BACK from its window every frame rather than remembered from the
        // frame the layout was built: a layout loaded from imgui.ini never passes through DockBuilder, and a
        // captured id would be 0 for the whole of every such session.
        ImGuiID mainDockId = 0;
        if ( const ::ImGuiWindow* scene = ImGui::FindWindowByName( PanelDisplayTitle( "Scene###scene" ).c_str() ) )
            mainDockId = scene->DockId;
        const ImGuiViewport* work      = ImGui::GetMainViewport();
        const bool           sceneLive = mainDockId != 0 && ImGui::DockBuilderGetNode( mainDockId ) != nullptr;

        // A document that closed gives its "placed" mark back, so its next opening is placed again.
        std::erase_if( m_PlacedDocuments,
                       [this]( const SubjectId& placed )
                       {
                           return std::none_of( m_OpenDocuments.begin(), m_OpenDocuments.end(),
                                                [&]( const auto& open ) { return open->Subject() == placed; } );
                       } );

        for ( const auto& document : m_OpenDocuments )
        {
            const SubjectId   subject = document->Subject();
            const std::string kind    = DocumentKindKey( subject );
            // A major-tab document is drawn only while its tab is in front, and then it is the only one: the
            // level's documents live in the level's dockspace, which is hidden behind it.
            const bool major = document->OpensAsMajorTab();
            if ( MajorTabActive() ? subject != m_ActiveMajorTab : major )
                continue;
            if ( major )
            {
                ImGui::SetNextWindowPos( ImVec2( m_MajorTabOrigin.x, m_MajorTabOrigin.y ), ImGuiCond_Always );
                ImGui::SetNextWindowSize( ImVec2( m_MajorTabSize.x, m_MajorTabSize.y ), ImGuiCond_Always );
                m_PlacedDocuments.insert( subject );
            }

            // PLACED ONCE, ON THE FRAME THE WINDOW FIRST EXISTS, the way Unreal opens an asset editor: where
            // this kind was last put, else as a tab beside the level viewport (Editor/Core/DocumentPlacement.hpp).
            // ImGuiCond_Always for that one frame, because the window's own imgui.ini entry would otherwise
            // put it back wherever an older layout left it (the small floating window ME1c removed).
            const bool placing = !major && !m_PlacedDocuments.contains( subject );
            if ( placing )
            {
                const auto  it             = m_RememberedPlacement.find( kind );
                const auto* remembered     = it != m_RememberedPlacement.end() ? &it->second : nullptr;
                const bool  rememberedLive = remembered != nullptr && remembered->DockId != 0 &&
                                            ImGui::DockBuilderGetNode( remembered->DockId ) != nullptr;
                const glm::vec2 workPos( work->WorkPos.x, work->WorkPos.y );
                const glm::vec2 workSize( work->WorkSize.x, work->WorkSize.y );
                // The drawer is wherever the Assets browser is docked today, read back like the scene's node.
                ImGuiID drawerDockId = 0;
                if ( const ::ImGuiWindow* assets =
                          ImGui::FindWindowByName( PanelDisplayTitle( "Assets" ).c_str() ) )
                    drawerDockId = assets->DockId;
                const bool drawerLive = drawerDockId != 0 && ImGui::DockBuilderGetNode( drawerDockId ) != nullptr;
                const DocumentPlacement::Resolution resolved =
                     document->IsLevelTimeline()
                          ? DocumentPlacement::ResolveTimeline( m_SubjectEditors.TypeName( subject ), remembered,
                                                                drawerDockId, drawerLive, mainDockId, sceneLive,
                                                                rememberedLive, workPos, workSize )
                          : DocumentPlacement::Resolve( m_SubjectEditors.TypeName( subject ), remembered,
                                                        mainDockId, sceneLive, rememberedLive, workPos, workSize );
                if ( !resolved.Report.empty() )
                    LOG_WARN( "[Documents] {}: {}", document->GetName(), resolved.Report );
                ImGui::SetNextWindowDockID( resolved.Place.DockId, ImGuiCond_Always );
                if ( !resolved.Place.Docked() )
                {
                    ImGui::SetNextWindowPos( ImVec2( resolved.Place.Pos.x, resolved.Place.Pos.y ),
                                             ImGuiCond_Always );
                    ImGui::SetNextWindowSize( ImVec2( resolved.Place.Size.x, resolved.Place.Size.y ),
                                              ImGuiCond_Always );
                }
                m_PlacedDocuments.insert( subject );
            }

            if ( !m_FocusWindow.empty() && document->GetName() == m_FocusWindow )
            {
                ImGui::SetNextWindowFocus();
                m_FocusWindow.clear();
            }

            // THE CLOSE BOX WRITES TO A FRAME-LOCAL BOOL, NOT TO THE PANEL'S VISIBILITY.
            //
            // This one line is the defect, fixed. While documents lived in the panel list they were drawn
            // with `&panel->GetVisibility()` like every tool, so one bool meant "hidden" for a tool and
            // "destroy me" for a document — and the View menu, which wrote that same bool, could therefore
            // destroy a document with a tick and had no way to bring it back. A document has no visibility:
            // it is open, or it does not exist.
            bool            open       = true;
            const glm::vec2 docPadding = document->GetWindowPadding();
            ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( docPadding.x, docPadding.y ) );
            // BEGIN'S RETURN VALUE IS "IS THIS DOCUMENT ON SCREEN", and it was being thrown away. It is
            // false for a window that is collapsed and for one whose dock tab is not the active one — so
            // four documents in one dock node were all drawing their contents every frame while one of
            // them was visible, and the three that were not were also holding renderer slots for it. The
            // content is skipped, which is ImGui's own idiom, and the frames off screen are counted so the
            // slot can go back (ReleaseSlotsOfHiddenDocuments).
            // A major tab's close box is its tab in the strip (DrawMajorTabStrip); the window is the area.
            constexpr ImGuiWindowFlags kMajorFlags =
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
            const bool visible = ImGui::Begin( DocumentDisplayTitle( *document ).c_str(), major ? nullptr : &open,
                                               major ? kMajorFlags : ImGuiWindowFlags_None );
            ImGui::PopStyleVar();
            // OBSERVED from the second frame on: wherever the person has put the window is what the next
            // opening of this kind gets. The first frame is skipped because the placement is still landing.
            if ( !placing && !major )
            {
                const ImVec2                        pos  = ImGui::GetWindowPos();
                const ImVec2                        size = ImGui::GetWindowSize();
                const DocumentPlacement::Remembered seen =
                     DocumentPlacement::Observe( ImGui::GetWindowDockID(), mainDockId, glm::vec2( pos.x, pos.y ),
                                                 glm::vec2( size.x, size.y ) );
                auto [it, inserted] = m_RememberedPlacement.try_emplace( kind, seen );
                if ( inserted || !( it->second == seen ) )
                {
                    it->second = seen;
                    ImGui::MarkIniSettingsDirty();
                }
            }
            if ( visible )
            {
                // DockHierarchy: a document with its own DockSpace (the Animation Editor's Persona layout) has
                // its panels as docked windows, which RootAndChildWindows alone does not count as its own.
                if ( ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows |
                                             ImGuiFocusedFlags_DockHierarchy ) )
                    focused = subject;
                {
                    DESERT_PROFILE_SCOPE_DYNAMIC( document->GetName().c_str() );
                    document->OnUIRender();
                }
                // REPORTED, NOT DECIDED HERE. This view says only "I drew it"; whether NOBODY drew it is a
                // question about all the views at once and is settled by OpenDocuments::EndFrame after
                // every one of them has run — see the note there. Written as a report rather than as an
                // erase because the Clouds window draws the same documents and the two answers must not
                // race on the order the views happen to run in.
                m_OpenDocuments.NoteDrawn( subject );
            }
            ImGui::End();

            if ( !open )
                closeRequests.push_back( subject );
        }

        // The focus is only MOVED by a document that actually has it. A frame in which the keyboard is on a
        // tool leaves the last focused document standing, so Ctrl+Tab resumes from where the user was
        // editing rather than from nothing.
        m_DocumentHasFocus = !focused.IsNull();
        if ( !focused.IsNull() )
        {
            // CLICKING A DOCUMENT COMMITS THE RING, cycling to one does not. Both are "focus", so without
            // this distinction one of the two rules would be wrong: either a mouse click would leave
            // Ctrl+Tab walking an order the user has since abandoned, or the second Ctrl+Tab would return to
            // where the first one started. The cycling flag is cleared when Ctrl comes up, and the ring is
            // committed there — see the shortcut block in OnUIRender.
            if ( focused != m_FocusedDocument && !m_CyclingDocuments )
                m_DocumentWell.Touch( focused );

            m_FocusedDocument = focused;
        }

        for ( const SubjectId& subject : closeRequests )
            AskDocumentClose( subject, "you closed the window" );
    }

    void DocumentHost::DrawOpenRefusedPopup()
    {
        namespace ImGui = ::ImGui;

        constexpr const char* kTitle = "Cannot open this document";

        if ( m_OpenRefusalPending )
        {
            ImGui::OpenPopup( kTitle );
            m_OpenRefusalPending = false;
        }

        ImGui::SetNextWindowPos( ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                 ImVec2( 0.5f, 0.5f ) );

        if ( !ImGui::BeginPopupModal( kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
            return;

        if ( !m_OpenRefusal )
        {
            // Cannot normally happen; the modal is only ever opened with a refusal in hand. Closing rather
            // than drawing an empty dialog, because an empty dialog with no way out is worse than none.
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        ImGui::PushStyleColor( ImGuiCol_Text, ThemeManager::GetErrorColor() );
        ImGui::TextUnformatted( ICON_MDI_ALERT_CIRCLE_OUTLINE );
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushFont( EditorResources::GetBoldFont() );
        ImGui::Text( "Cannot open %s", m_OpenRefusal->AssetName.c_str() );
        ImGui::PopFont();

        // EVERY NUMBER THAT DECIDED IT, the same ones the log line carries: what the document needs (its own
        // forecast plus what open documents have spoken for), the ceiling and where it came from, what is in
        // use and what is left. "Not enough memory" without them cannot be acted on.
        const Engine::ViewBudget::Verdict& verdict = m_OpenRefusal->Verdict;
        ImGui::TextDisabled( "Needs %s (%s already spoken for by documents that have not drawn yet).",
                             Engine::ViewBudget::FormatMiB( verdict.RequestBytes ).c_str(),
                             Engine::ViewBudget::FormatMiB( m_OpenRefusal->PendingBytes ).c_str() );
        ImGui::TextDisabled( "%s.", Engine::ViewBudget::DescribeCeiling( m_OpenRefusal->Reading ).c_str() );
        ImGui::TextDisabled( "In use %s%s, free %s.", Engine::ViewBudget::FormatMiB( verdict.UsageBytes ).c_str(),
                             m_OpenRefusal->Reading.UsageKnown ? "" : " (counted from open views only)",
                             Engine::ViewBudget::FormatMiB( verdict.FreeBytes ).c_str() );
        ImGui::Separator();

        // WHERE THE MEMORY WENT, by view, largest first — what each open view actually holds.
        std::vector<Engine::ViewBudget::HeldView> views = m_OpenRefusal->Views;
        std::sort( views.begin(), views.end(),
                   []( const Engine::ViewBudget::HeldView& a, const Engine::ViewBudget::HeldView& b )
                   { return a.Bytes > b.Bytes; } );
        ImGui::TextUnformatted( "Open views" );
        ImGui::Indent( 18.0f );
        for ( const Engine::ViewBudget::HeldView& view : views )
            ImGui::TextDisabled( "%s \xe2\x80\x94 %s", view.Name.c_str(),
                                 Engine::ViewBudget::FormatMiB( view.Bytes ).c_str() );
        ImGui::Unindent( 18.0f );
        ImGui::Separator();
        ImGui::TextUnformatted( "Close one of these to make room:" );

        std::vector<SubjectId> closeRequests;
        for ( const ViewConsumer& consumer : m_OpenRefusal->Census )
        {
            ImGui::TextUnformatted( consumer.Name.c_str() );

            // A row the user can act on gets a button; the main viewport and the Details preview do not,
            // because neither is a window a person closes to make room. Saying nothing on those rows is
            // the honest version: they are named because they explain where the memory went.
            if ( consumer.Document && m_OpenDocuments.Find( *consumer.Document ) )
            {
                ImGui::SameLine( ImGui::GetContentRegionMax().x - 64.0f );
                ImGui::PushID( static_cast<int>( std::hash<SubjectId>{}( *consumer.Document ) & 0x7fffffff ) );
                if ( ImGui::SmallButton( "Close" ) )
                    closeRequests.push_back( *consumer.Document );
                ImGui::PopID();
            }

            // A document that has not drawn yet holds nothing the device reports, but it WILL — say how
            // much, because that is memory the refusal counted against the new document.
            if ( !consumer.HoldsView && consumer.ClaimsView )
            {
                ImGui::Indent( 18.0f );
                ImGui::TextDisabled( "will allocate ~%s when it draws",
                                     Engine::ViewBudget::FormatMiB( consumer.ForecastBytes ).c_str() );
                ImGui::Unindent( 18.0f );
            }

            // The CPU-drawn documents say so, for the reason the log line already did: closing one frees
            // nothing, and a census that let the user close four of them and still be refused would be a
            // longer way of saying nothing.
            if ( !consumer.HoldsView && !consumer.ClaimsView )
            {
                ImGui::Indent( 18.0f );
                ImGui::TextDisabled( "drawn on the CPU \xe2\x80\x94 closing it frees nothing" );
                ImGui::Unindent( 18.0f );
            }
        }

        ImGui::Separator();
        if ( ImGui::Button( "Close this message", ImVec2( 180.0f, 0.0f ) ) )
        {
            m_OpenRefusal.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::TextDisabled( "The same census is in the log." );

        ImGui::EndPopup();

        for ( const SubjectId& subject : closeRequests )
            RequestDocumentClose( subject, "closed to free view memory" );
    }
} // namespace Desert::Editor
