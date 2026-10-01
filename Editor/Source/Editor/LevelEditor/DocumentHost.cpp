#include <Engine/Core/PlayerStart.hpp>
#include <Engine/Graphic/ViewBudgetGate.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include "Common/Core/Constants.hpp"
#include "Common/Core/Logger.hpp"
#include "DesertShared/ResultStr.hpp"
#include "Editor/Core/CommandPalette.hpp"
#include "Editor/Core/DocumentWell.hpp"
#include "Editor/Core/EditorSubject.hpp"
#include "Editor/Core/IconsMaterialDesignIcons.hpp"
#include "Editor/Core/OpenDocuments.hpp"
#include "Editor/Core/UnsavedClose.hpp"
#include "Editor/Panels/IPanel.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <imgui.h>
#include <memory>
#include <optional>
#include <Common/Core/Core.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Scripting/ScriptEngine.hpp>
#include <Engine/Core/WorldStreamer.hpp>
#include "Editor/Core/Commands/SceneCommands.hpp"
#include "Editor/Core/ImGuiUtilities.hpp"
#include <ImGui/imgui_internal.h>
#include <format>
#include <Editor/Panels/AnimationEditor/SkeletonReferenceSlots.hpp>
#include "Editor/Panels/FileExplorer/FileExplorerPanel.hpp"
#include "Editor/Panels/WorldPartition/WorldPartitionPanel.hpp"
#include "Editor/Panels/Foliage/FoliageCommands.hpp"
#include "Editor/Panels/Collections/CollectionsPanel.hpp"
#include "Editor/Panels/NodeGraph/NodeGraphPanel.hpp"
#include "Editor/Panels/MaterialEditor/MaterialEditorPanel.hpp"
#include "Editor/Core/AssetOpen.hpp"
#include "Editor/Panels/Animation/AnimGraphPanel.hpp"
#include "Editor/Panels/Photogrammetry/PhotogrammetryPanel.hpp"
#include "Editor/Panels/AssetReferences/AssetReferencesPanel.hpp"
#include "Editor/Panels/LuaConsole/LuaConsolePanel.hpp"
#include "Editor/Panels/Sequencer/SequencerPanel.hpp"
#include "Editor/Panels/Validation/SceneValidationPanel.hpp"
#include "Editor/Panels/Clouds/CloudModellingVolumePanel.hpp"
#include "Editor/Panels/Clouds/CloudLayoutPanel.hpp"
#include "Editor/Panels/Clouds/CloudNoiseVolumePanel.hpp"
#include "Editor/Panels/SkyboxViewer/SkyboxViewerDocument.hpp"
#include "Editor/Panels/AnimationEditor/AnimationEditorDocument.hpp"
#include "Editor/Panels/StaticMeshViewer/StaticMeshViewerDocument.hpp"
#include "Editor/Panels/TextureViewer/TextureViewerDocument.hpp"
#include "Editor/Panels/Clouds/CloudTypePanel.hpp"
#include "Editor/Panels/Clouds/CloudsPanel.hpp"
#include "Editor/Core/OpenableAssets.hpp"
#include "Editor/Core/SubjectEditorRegistry.hpp"
#include "Editor/Core/SubjectOpenRequest.hpp"
#include <Editor/Core/Rigging/RigBuilder.hpp>
#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "Editor/LevelEditor/DocumentHost.hpp"
#include "Editor/LevelEditor/SceneWorkspace.hpp"
#include "Editor/LevelEditor/WindowTitles.hpp"
#include "Engine/Assets/AssetMetadata.hpp"
#include "Engine/Assets/Common.hpp"
#include "Engine/Core/ViewBudget.hpp"
#include "Engine/Graphic/Renderer.hpp"

namespace Desert::Editor
{
    // WHAT A DOCUMENT NOBODY CLAIMS LOOKS LIKE. The registry has no opinion about an unregistered subject
    // type and must not invent one (SubjectEditorRegistry::Icon takes the fallback as an argument for
    // exactly that reason), so the editor's answer lives here, once, rather than at each of the four places
    // that draw a document's glyph.
    static constexpr const char* kUnknownDocumentIcon = ICON_MDI_FILE_DOCUMENT_OUTLINE;

    // THE ICON SWITCH USED TO BE HERE, keyed on Assets::AssetTypeID, and it is gone rather than extended.
    //
    // It was one of the two hand-written tables a new kind of document had to be entered in — the other
    // being AssetTypeName for the text — neither of which is where the document is registered. That is
    // three edits in three files for one new kind, and the two that are not the registration are the ones
    // that get forgotten: the table's `default:` then quietly gave the new kind the generic page glyph and
    // nothing anywhere said so. The icon is now part of the registration
    // (SubjectEditorRegistry::Registration::Icon), so a kind that exists HAS one, by construction. It also
    // had no answer at all for a component subject, whose facet is not an AssetTypeID.

    DocumentHost::DocumentHost( SceneWorkspace& workspace, std::shared_ptr<Assets::AssetManager>& assetManager,
                                std::string& focusWindow, ShowFolderFn showFolder )
         : m_Workspace( workspace ), m_AssetManager( assetManager ), m_ShowFolder( std::move( showFolder ) ),
           m_FocusWindow( focusWindow )
    {
    }

    std::string DocumentHost::DocumentIcon( const SubjectId& subject ) const
    {
        return m_SubjectEditors.Icon( subject, kUnknownDocumentIcon );
    }

    // The window title a document is drawn with: its type's icon, its subject's name, and the "###doc<...>"
    // identity DocumentTitle already baked into GetName(). NOT PanelDisplayTitle, which would look the icon
    // up by a name that is an asset's and give every document the same fallback.
    std::string DocumentHost::DocumentDisplayTitle( const ISubjectDocument& document ) const
    {
        return IconWindowTitle( DocumentIcon( document.Subject() ), DocumentDisplayName( document.GetName() ),
                                document.GetName() );
    }

    std::vector<DocumentHost::ViewConsumer> DocumentHost::ViewCensus() const
    {
        std::vector<ViewConsumer> census;
        census.push_back( { "main viewport", true } ); // the primary scene's view exists for the session

        for ( const auto& doc : m_Workspace.Documents() )
            census.push_back( { "scene view '" + doc->Name + "'", true } );

        // A second ANGLE holds a view exactly as a second document does — leaving it out of the census
        // would make the refusal's own list disagree with SceneRenderer::LiveHoldings().
        for ( const auto& view : m_Workspace.Viewports() )
            census.push_back( { "viewport '" + view->Name + "'", view->Renderer != nullptr } );

        // NO DETAILS ROW. Details holds no view (THM-FIXF): its asset rows are pictures from the thumbnail pool,
        // as UE's Details slots are, and a live view belongs to an asset window and dies with it.

        // The documents are asked of their own owner rather than sifted out of the panel list with a
        // dynamic_cast. That cast was the seam an earlier task closed: it only existed because the two
        // kinds shared a container, and every place that had to write it was a place that could forget to.
        for ( const auto& document : m_OpenDocuments )
        {
            // The VISIBLE half of the name. The census tells a user what to close, and they close a window
            // titled "MP_GreenTint", not one titled "MP_GreenTint###docasset:2:3333333333333333333".
            census.push_back(
                 { m_SubjectEditors.TypeName( document->Subject() ) + " document '" +
                        DocumentDisplayName( document->GetName() ) + "'",
                   document->HoldsView(), document->ClaimsView(),
                   document->ClaimsView() && !document->HoldsView() ? document->ViewForecastBytes() : 0,
                   document->Subject() } );
        }

        return census;
    }

    std::string DocumentHost::SubjectEntityName( const SubjectId& subject, const char* what ) const
    {
        // WHAT THE WINDOW IS CALLED, not what it is. The subject is the identity; this is the label beside
        // it, and it is resolved ONCE, here, at the moment the document is built — the document itself must
        // not need a scene to know its own name, and an entity renamed afterwards does not become a second
        // window (an asset document behaves the same way; see Control::DocumentSnapshot::Subject).
        std::string name = "Entity";
        if ( m_Workspace.ActiveScene() )
        {
            if ( const auto entOpt = m_Workspace.ActiveScene()->FindEntityByID( subject.Owner ) )
                name = entOpt->get().GetComponent<ECS::TagComponent>().Tag;
        }
        return name + " \xc2\xb7 " + what;
    }

    void DocumentHost::ServiceSubjectOpenRequests()
    {
        // ASSET FIELDS FIRST: an "Open" from Details becomes a subject request below, in this same frame.
        for ( const Core::AssetFieldRequest& request : Core::AssetFieldRequests::Drain() )
        {
            const Assets::AssetMetadata* found =
                 m_AssetManager ? m_AssetManager->FindMetadataByHandle( request.Handle ) : nullptr;
            if ( request.Action == Core::AssetFieldAction::Open )
            {
                if ( auto opened = Core::RequestOpenAsset( found, request.Handle, m_SubjectEditors );
                     !opened.IsSuccess() )
                    LOG_WARN( "[Editor] Open: {} — no window opened.", opened.GetError() );
                continue;
            }
            auto folder = Core::AssetFolderFor( found, request.Handle );
            auto shown  = folder.IsSuccess() ? m_ShowFolder( folder.GetValue().generic_string() )
                                             : Common::MakeFormattedError<bool>( "{}", folder.GetError() );
            if ( !shown.IsSuccess() )
                LOG_WARN( "[Editor] Show in browser: {}", shown.GetError() );
        }

        for ( const SubjectId& subject : Core::SubjectOpenRequests::Drain() )
        {
            // OPEN-OR-FOCUS, keyed by the subject, asked of the one owner of open documents. It does NOT
            // set a visibility flag any more: a document that is open is open, and "focus" is the only
            // thing a second request for the same subject can mean.
            if ( ISubjectDocument* open = m_OpenDocuments.Find( subject ) )
            {
                FocusDocument( open->Subject() );
                continue;
            }

            // Checked BEFORE the budget below, so a kind with no editor is reported as the missing editor it
            // is rather than as a memory shortage it had nothing to do with.
            if ( !m_SubjectEditors.HasEditorFor( subject.Type() ) )
            {
                LOG_WARN( "[Editor] Nothing edits subject '{}' — no window opened.", subject.ToString() );
                continue;
            }

            // BUILT FIRST, JUDGED SECOND. The admission asks the document what its view will cost
            // (ISubjectDocument::ViewForecastBytes), and only the document knows: a Material Editor forecasts
            // a preview view, a cloud document forecasts nothing. Building one allocates no GPU memory — a
            // document builds its view on its first DRAW — so a refused document is dropped here having cost
            // nothing but the object.
            auto document = m_SubjectEditors.Create( subject );
            if ( !document )
                continue; // the registry already said why

            // ASKED THE MOMENT IT IS BUILT, and not left to the sweep a frame later. A document whose
            // subject was already gone would otherwise appear for one frame and vanish, which reads as a
            // window that failed rather than as a thing that is not there. The factory has just resolved
            // the subject, so this costs one more resolution and answers before anything is on screen.
            if ( !document->IsSubjectAlive() )
            {
                LOG_WARN( "[Editor] Refusing to open a document for subject '{}': the {} it names does not "
                          "exist (deleted, or in a scene that is no longer open).",
                          subject.ToString(), m_SubjectEditors.TypeName( subject ) );
                continue;
            }

            // A DOCUMENT WHOSE VIEW DOES NOT FIT IS REFUSED, OUT LOUD, IN BYTES. Admitted past the budget it
            // would not fail here — its view would fail to allocate on the first frame it draws, far from the
            // click that asked for it. So the device-local budget is asked now, with every number printed and
            // the open views named, because a bare "out of memory" leaves the user with nothing to close.
            //
            // Pending bytes are counted separately and it is not pedantry: a document that is open but has
            // not drawn yet holds no memory the device reports and has an allocation coming, so the usage
            // alone would admit a document there is no room for and discover it a frame later.
            //
            // The rule itself lives in SubjectEditorRegistry.hpp (AdmitDocumentView over
            // Engine::ViewBudget::MayCreate), not here: this file is compiled by no suite, and a rule written
            // in it is a rule nothing can assert.
            const uint64_t                    pending = PendingViewBytes( m_OpenDocuments.Documents() );
            const Engine::ViewBudget::Reading reading = Graphic::ReadViewBudget();
            const Engine::ViewBudget::Verdict verdict = AdmitDocumentView( *document, pending, reading );
            if ( !verdict.Ok )
            {
                std::vector<Engine::ViewBudget::HeldView> views = Graphic::SceneRenderer::LiveHoldings();
                std::vector<ViewConsumer>                 rows  = ViewCensus();
                std::string                               census;
                for ( const ViewConsumer& consumer : rows )
                {
                    std::string state =
                         "holds no view and never will (drawn on the CPU) — closing it frees nothing";
                    if ( consumer.HoldsView )
                        state = "holds a view";
                    else if ( consumer.ClaimsView )
                        state = std::format( "no view yet, will allocate ~{} when it draws",
                                             Engine::ViewBudget::FormatMiB( consumer.ForecastBytes ) );
                    census += "\n    " + consumer.Name + " — " + state;
                }
                LOG_ERROR( "[Editor] Refusing to open a document for subject '{}': {} (of which {} is spoken for "
                           "by open documents that have not drawn yet). Close one of these first:{}",
                           subject.ToString(),
                           Engine::ViewBudget::DescribeRefusal( document->GetName(), verdict, reading, views ),
                           Engine::ViewBudget::FormatMiB( pending ), census );

                // AND THE SAME THING WHERE THE USER IS. The census above went to a log the user was not
                // reading, so a double-click on a document that did not fit did nothing at all as far as the
                // screen was concerned. The dialog carries the identical numbers and rows and, for the ones
                // that are documents, a button that acts on them.
                //
                // The subject is named by its FILE NAME or its ENTITY NAME where one is known: "handle
                // 3333333333333333333" is the log's identifier, not the user's.
                m_OpenRefusal        = OpenRefusal{ RefusedSubjectName( subject ),
                                             m_SubjectEditors.TypeName( subject ),
                                             verdict,
                                             reading,
                                             pending,
                                             std::move( views ),
                                             std::move( rows ) };
                m_OpenRefusalPending = true;
                continue;
            }

            const std::string name = document->GetName();
            // Asked BEFORE the move: what this document adds to the memory spoken for. A document that will
            // never build a view adds nothing. See ISubjectDocument::ClaimsView.
            const uint64_t committed = pending + ( document->ClaimsView() ? document->ViewForecastBytes() : 0 );

            // THROUGH THE OWNER'S OWN DOOR, which is what refuses a duplicate rather than appending one.
            // The open-or-focus above already answered for the route this function serves; the refusal
            // here is for the routes that do not exist yet, and it is the container's rule rather than a
            // habit every future call site has to inherit (Editor/Core/OpenDocuments.hpp).
            const DocumentOpenResult opened = m_OpenDocuments.Open( std::move( document ) );
            if ( opened.Outcome == DocumentOpenOutcome::Refused )
            {
                LOG_ERROR( "[Editor] The '{}' editor built a document that names nothing for subject '{}' — "
                           "no window was opened.",
                           m_SubjectEditors.TypeName( subject ), subject.ToString() );
                continue;
            }
            if ( opened.Outcome == DocumentOpenOutcome::AlreadyOpen )
            {
                // Cannot normally happen — the open-or-focus at the top of this loop catches it. Said out
                // loud rather than swallowed, because reaching this line means two requests for one subject
                // survived that check, and the container's refusal is what stops it becoming two documents.
                LOG_WARN( "[Editor] A second document for subject '{}' was built and discarded; the one "
                          "already open was focused instead.",
                          subject.ToString() );
            }

            m_DocumentWell.Opened( subject ); // also brings a closed well back
            m_FocusWindow     = name;         // brings the new window forward in the document well
            m_FocusedDocument = subject;
            LOG_INFO( "[Editor] Opened a '{}' document '{}' ({} open; {} spoken for by documents that have not "
                      "drawn yet; {}, in use {}).",
                      m_SubjectEditors.TypeName( subject ), name, m_OpenDocuments.Count(),
                      Engine::ViewBudget::FormatMiB( committed ), Engine::ViewBudget::DescribeCeiling( reading ),
                      Engine::ViewBudget::FormatMiB( reading.UsageBytes ) );
        }
    }

    std::string DocumentHost::RefusedSubjectName( const SubjectId& subject ) const
    {
        if ( subject.Domain == SubjectDomain::Asset && m_AssetManager )
        {
            // The UNTYPED metadata lookup, deliberately: the refusal happens before any editor for this
            // type is consulted, so all that is known about the subject is that it is an asset — and a
            // typed lookup would have to guess which class to ask for. Metadata carries no cast, so there
            // is nothing here that could answer with a stranger.
            if ( const auto* metadata =
                      m_AssetManager->FindMetadataByHandle( Assets::AssetHandle( subject.Owner ) ) )
                return metadata->Filepath.stem().string();
        }
        if ( subject.Domain == SubjectDomain::EntityComponent && m_Workspace.ActiveScene() )
        {
            if ( const auto entOpt = m_Workspace.ActiveScene()->FindEntityByID( subject.Owner ) )
                return entOpt->get().GetComponent<ECS::TagComponent>().Tag;
        }
        return "this subject";
    }

    void DocumentHost::RequestDocumentClose( const SubjectId& subject, std::string reason )
    {
        if ( !m_OpenDocuments.Find( subject ) )
            return; // already gone, or never open — a second x on one window in one frame is not an error

        const auto queued =
             std::find_if( m_DocumentsToClose.begin(), m_DocumentsToClose.end(),
                           [&subject]( const PendingDocumentClose& p ) { return p.Subject == subject; } );
        if ( queued == m_DocumentsToClose.end() )
            m_DocumentsToClose.push_back( PendingDocumentClose{ subject, std::move( reason ) } );
    }

    void DocumentHost::AskDocumentClose( const SubjectId& subject, std::string reason )
    {
        const ISubjectDocument* document = m_OpenDocuments.Find( subject );
        if ( document == nullptr )
            return;

        const bool dirty = document->GetDiskState() == ISubjectDocument::DiskState::Dirty;
        if ( !CloseAsksFirst( dirty, /*closedByThePerson=*/true ) )
        {
            RequestDocumentClose( subject, std::move( reason ) );
            return;
        }

        const auto asked =
             std::find_if( m_CloseQuestions.begin(), m_CloseQuestions.end(),
                           [&subject]( const PendingDocumentClose& p ) { return p.Subject == subject; } );
        if ( asked == m_CloseQuestions.end() )
            m_CloseQuestions.push_back( PendingDocumentClose{ subject, std::move( reason ) } );
    }

    namespace
    {
        constexpr std::string_view kEditorExitReason = "the editor is closing";

        [[nodiscard]] std::string_view CloseChoiceName( const UnsavedCloseChoice choice )
        {
            switch ( choice )
            {
                case UnsavedCloseChoice::Save:
                    return "saved first";
                case UnsavedCloseChoice::Discard:
                    return "changes discarded";
                case UnsavedCloseChoice::Cancel:
                    return "cancelled";
            }
            return "?";
        }
    } // namespace

    bool DocumentHost::AnswerCloseQuestion( const SubjectId& subject, const UnsavedCloseChoice choice )
    {
        const auto asked =
             std::find_if( m_CloseQuestions.begin(), m_CloseQuestions.end(),
                           [&subject]( const PendingDocumentClose& p ) { return p.Subject == subject; } );
        if ( asked == m_CloseQuestions.end() )
            return false;
        const PendingDocumentClose question = std::move( *asked );
        m_CloseQuestions.erase( asked );

        ISubjectDocument* document = m_OpenDocuments.Find( subject );
        if ( document == nullptr )
            return false; // closed some other way while the question was up: nothing left to answer for

        bool saved = false;
        if ( choice == UnsavedCloseChoice::Save )
            saved = document->SaveDocument();
        else if ( choice == UnsavedCloseChoice::Discard && !document->DiscardEdits() )
        {
            // Closing drops what memory holds anyway; the line only says the document had no discard step.
            LOG_INFO( "'{}': closing without saving, the document has no edits of its own to discard",
                      document->GetName() );
        }

        const bool closes = CloseAfterAnswer( choice, saved );
        if ( closes )
        {
            RequestDocumentClose( subject,
                                  question.Reason + " (" + std::string( CloseChoiceName( choice ) ) + ")" );
        }
        else
        {
            if ( choice == UnsavedCloseChoice::Save )
            {
                LOG_WARN( "'{}' stays open: Save wrote no file, so closing would lose its edits",
                          document->GetName() );
            }
            // A kept document keeps the editor open too, and the rest of the exit's questions go with it.
            if ( m_AfterCloseQuestions )
            {
                m_AfterCloseQuestions = nullptr;
                std::erase_if( m_CloseQuestions,
                               []( const PendingDocumentClose& p ) { return p.Reason == kEditorExitReason; } );
            }
        }

        if ( m_AfterCloseQuestions && m_CloseQuestions.empty() )
            std::exchange( m_AfterCloseQuestions, nullptr )();
        return closes;
    }

    void DocumentHost::AskCloseAll( std::function<void()> onAllAnswered )
    {
        for ( const auto& document : m_OpenDocuments )
        {
            if ( !CloseAsksFirst( document->GetDiskState() == ISubjectDocument::DiskState::Dirty,
                                  /*closedByThePerson=*/true ) )
                continue;
            const SubjectId subject = document->Subject();
            const bool      asked =
                 std::any_of( m_CloseQuestions.begin(), m_CloseQuestions.end(),
                              [&subject]( const PendingDocumentClose& p ) { return p.Subject == subject; } );
            if ( !asked )
                m_CloseQuestions.push_back( PendingDocumentClose{ subject, std::string( kEditorExitReason ) } );
        }

        if ( m_CloseQuestions.empty() )
        {
            onAllAnswered();
            return;
        }
        m_AfterCloseQuestions = std::move( onAllAnswered );
    }

    void DocumentHost::DrawCloseQuestionPopup()
    {
        namespace ImGui = ::ImGui;

        if ( m_CloseQuestions.empty() )
            return;
        const SubjectId         subject  = m_CloseQuestions.front().Subject;
        const ISubjectDocument* document = m_OpenDocuments.Find( subject );
        if ( document == nullptr )
        {
            m_CloseQuestions.erase( m_CloseQuestions.begin() );
            return;
        }

        constexpr const char* kTitle = "Save Changes?###UnsavedClose";
        if ( !ImGui::IsPopupOpen( kTitle ) )
            ImGui::OpenPopup( kTitle );
        ImGui::SetNextWindowPos( ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                 ImVec2( 0.5f, 0.5f ) );
        if ( !ImGui::BeginPopupModal( kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
            return;

        const std::string name = DocumentDisplayName( document->GetName() );
        ImGui::Text( "Save changes to %s before closing?", name.c_str() );
        ImGui::TextDisabled( "Don't Save puts the file's content back; nothing is written." );
        ImGui::Spacing();

        std::optional<UnsavedCloseChoice> choice;
        if ( ImGui::Button( "Save", ImVec2( 110.0f, 0.0f ) ) )
            choice = UnsavedCloseChoice::Save;
        ImGui::SameLine();
        if ( ImGui::Button( "Don't Save", ImVec2( 110.0f, 0.0f ) ) )
            choice = UnsavedCloseChoice::Discard;
        ImGui::SameLine();
        if ( ImGui::Button( "Cancel", ImVec2( 110.0f, 0.0f ) ) || ImGui::IsKeyPressed( ImGuiKey_Escape ) )
            choice = UnsavedCloseChoice::Cancel;

        if ( choice )
        {
            ImGui::CloseCurrentPopup();
            (void)AnswerCloseQuestion( subject, *choice );
        }
        ImGui::EndPopup();
    }

    void DocumentHost::RequestCloseAllDocuments()
    {
        // Collected first and requested after, rather than requested while iterating: RequestDocumentClose
        // reads the well, and a range-for over a container something else is being asked about is the kind
        // of thing that survives review and then does not survive a refactor.
        std::vector<SubjectId> subjects;
        subjects.reserve( m_OpenDocuments.Count() );
        for ( const auto& document : m_OpenDocuments )
            subjects.push_back( document->Subject() );

        for ( const SubjectId& subject : subjects )
            AskDocumentClose( subject, "Close All Documents" );
    }

    void DocumentHost::CloseDocumentsWhoseSubjectIsGone()
    {
        // ── A DOCUMENT CLOSES WITH ITS SUBJECT ────────────────────────────────────────────────────────
        //
        // The owner's decision, and the counterpart to the one he refused: a document is NOT closed when
        // it loses the focus, because a layout that rearranges itself reads as an editor that lost your
        // panel. It IS closed when the thing it edits stops existing — delete the entity, remove the
        // component, delete the asset, close the scene — because the alternative is a window editing
        // nothing, and every one of its controls then writes into a resolution that returns null.
        //
        // WITH A NAMED REASON. Three different causes now queue a close, and a user whose window vanished
        // is owed which one it was; ServiceDocumentCloses prints it.
        //
        // ASKED EVERY FRAME, and it has to be. There are FOUR ways a subject dies and no single event
        // covers them: an asset leaves the manager, an entity is destroyed, a COMPONENT is removed from an
        // entity that survives, or the scene a document was opened over is closed. A subscription to one
        // of the four would be worse than none, because the other three would then look handled. The cost
        // is one resolution per open document per frame — the same resolution each document already
        // performs to draw itself, and there are rarely more than six of them.
        std::vector<SubjectId> dead;
        for ( const auto& document : m_OpenDocuments )
            if ( !document->IsSubjectAlive() )
                dead.push_back( document->Subject() );

        for ( const SubjectId& subject : dead )
        {
            RequestDocumentClose( subject, "the " + m_SubjectEditors.TypeName( subject ) +
                                                " it was editing no longer exists" );
        }
    }

    void DocumentHost::ReleaseSlotsOfHiddenDocuments()
    {
        // ── THE SLOT GOES WHEN NOBODY IS LOOKING; THE WINDOW STAYS ────────────────────────────────────
        //
        // Four documents docked as tabs in one node show one tab. The other three were rendering previews
        // nobody could see and holding three of the six renderer slots while they did it, so the fifth
        // document the user opened was refused over resources being spent on hidden windows.
        //
        // EVERY VIEW REPORTS INTO ONE COUNT, and the count lives on the owner (OpenDocuments::NoteDrawn /
        // EndFrame) rather than in this file. It used to be a map written only by the document well's draw
        // loop, which was the whole truth while the well was the only thing that could draw a document —
        // the Clouds window is a second one, and a material shown only in THAT window would otherwise have
        // been counted hidden and had its preview renderer taken away under a pane somebody was using.
        //
        // Called from ServiceDocumentCloses so it runs behind the SAME device-idle wait a close uses —
        // releasing a PreviewViewport destroys a Scene and a SceneRenderer, and the last submitted frame
        // may still be executing against them.
        for ( const auto& document : m_OpenDocuments )
        {
            const uint32_t undrawn = m_OpenDocuments.FramesUndrawn( document->Subject() );
            if ( undrawn < kFramesHiddenBeforeSlotRelease )
                continue;
            if ( !document->HoldsView() )
                continue;

            document->ReleaseView();

            // VERIFIED, NOT ASSUMED. ReleaseView's contract is that HoldsView answers false
            // afterwards; a document that inherited the empty default while genuinely holding a slot would
            // otherwise keep it for ever and the census would go on blaming a window the user cannot fix.
            if ( document->HoldsView() )
            {
                LOG_ERROR( "[Editor] '{}' was asked to release its renderer slot after {} hidden frames and "
                           "still holds one. ReleaseView must make HoldsView false — see "
                           "ISubjectDocument.",
                           DocumentDisplayName( document->GetName() ), undrawn );
                continue;
            }

            LOG_INFO( "[Editor] '{}' released its view after {} frames off screen (views: {}). "
                      "It is rebuilt on the first frame the window is drawn again.",
                      DocumentDisplayName( document->GetName() ), undrawn,
                      Graphic::SceneRenderer::DescribeLiveViews() );
        }
    }

    void DocumentHost::ServiceDocumentCloses()
    {
        // The hidden-document sweep shares this function's device-idle wait, so the wait is taken when
        // either has work. Two waits in one frame would be two full pipeline drains for one frame's worth
        // of teardown.
        bool releasePending = false;
        for ( const auto& document : m_OpenDocuments )
        {
            if ( m_OpenDocuments.FramesUndrawn( document->Subject() ) >= kFramesHiddenBeforeSlotRelease &&
                 document->HoldsView() )
            {
                releasePending = true;
                break;
            }
        }

        if ( m_DocumentsToClose.empty() && !releasePending )
            return;

        // ONE device-idle wait for the whole batch. Destroying a document destroys its PreviewViewport, and
        // with it the scene, the renderer and the renderer slot; the last submitted frame may still be
        // executing against that renderer's pipelines, framebuffers and descriptor pools. The ordering is
        // the one ~PreviewViewport and CloseSceneView both established, not a precaution invented here.
        Graphic::Renderer::GetInstance().WaitDeviceIdle();

        ReleaseSlotsOfHiddenDocuments();

        for ( const PendingDocumentClose& pending : m_DocumentsToClose )
        {
            // Released, then destroyed HERE. The owner hands ownership back rather than dropping the object
            // itself, because it is this function that knows the device is idle — see OpenDocuments.
            std::unique_ptr<ISubjectDocument> closed = m_OpenDocuments.Release( pending.Subject );
            if ( !closed )
                continue;

            // THE VIEWS ARE TOLD, and told BEFORE the object dies: NoteClosed reads the display name off
            // it. A view cannot discover a departure without keeping a second copy of the open set, which
            // is the duplicated state the ownership split removes.
            m_DocumentWell.NoteClosed( *closed );

            const std::string name = closed->GetName();
            m_OpenDocuments.ForgetDrawHistory( pending.Subject );
            if ( m_FocusedDocument == pending.Subject )
                m_FocusedDocument = SubjectId{};

            closed.reset();

            // The REASON is printed, and it is why this line takes one. "Closed document 'Hero'" leaves a
            // user who did not close it with nothing to go on; "because the AnimationComponent it was
            // editing no longer exists" is the whole answer.
            //
            // The slot count is printed rather than derived for a different reason: a document that failed
            // to return its slot produces no error at all, and this line beside the one in
            // ServiceSubjectOpenRequests is what makes the leak readable.
            LOG_INFO( "[Editor] Closed document '{}' — {} ({} open; views after release: {}).",
                      DocumentDisplayName( name ), pending.Reason, m_OpenDocuments.Count(),
                      Graphic::SceneRenderer::DescribeLiveViews() );
        }

        m_DocumentsToClose.clear();
    }

    void DocumentHost::FocusDocument( const SubjectId& subject )
    {
        ISubjectDocument* document = m_OpenDocuments.Find( subject );
        if ( !document )
            return;

        m_DocumentWell.Opened( subject ); // asked for by name: a closed well comes back to show it
        m_FocusedDocument = subject;
        m_FocusWindow     = document->GetName(); // brings it forward in whatever dock it lives
    }

    void DocumentHost::UpdateCycleShortcut( const ImGuiIO& io )
    {
        if ( io.KeyCtrl && !io.WantTextInput && ::ImGui::IsKeyPressed( ImGuiKey_Tab, false ) )
        {
            const SubjectId before = m_FocusedDocument;
            CycleDocuments();
            if ( m_FocusedDocument != before )
                ::ImGui::GetCurrentContext()->NavWindowingTarget = nullptr;
        }

        // The ring is committed when Ctrl comes back up, not on each press: see CycleDocuments.
        if ( m_CyclingDocuments && !io.KeyCtrl )
        {
            m_CyclingDocuments = false;
            m_DocumentWell.Touch( m_FocusedDocument );
        }
    }

    void DocumentHost::CycleDocuments()
    {
        const auto next = m_DocumentWell.NextMostRecent( m_FocusedDocument );
        if ( !next )
            return;

        ISubjectDocument* document = m_OpenDocuments.Find( *next );
        if ( !document )
            return;

        // Focus WITHOUT touching the ring. Committing the new order on every press would make the second
        // Ctrl+Tab return to where the first started, so the order is committed when Ctrl is released —
        // see m_CyclingDocuments in OnUIRender.
        m_FocusedDocument  = *next;
        m_FocusWindow      = document->GetName();
        m_CyclingDocuments = true;
    }

    // ONE OF A DOCUMENT'S OWN ACTIONS, RUN BY NAME. A named function rather than the lambda body it was:
    // a parameter-less multi-line lambda is the shape `bugprone-exception-escape` fires on in this tree
    // (ScenePropertiesPanel.cpp:92 records the same finding), and it is also the shape clang-format 18 and
    // 22 disagree about. The palette entry is now one line and this is where the work is.
    //
    // THE DOCUMENT IS RE-RESOLVED FROM THE SUBJECT, not captured: a window can be closed between the
    // moment this dictionary was built and the moment an entry runs, and every other document command here
    // re-resolves for that reason. A label that no longer exists is a REFUSAL — a view mode's label changes
    // with the mode it is in, so "show the curves" is gone the moment the curves are showing.
    Common::BoolResultStr DocumentHost::RunDocumentAction( const SubjectId& subject, const std::string& label )
    {
        for ( const auto& open : m_OpenDocuments )
        {
            if ( !( open->Subject() == subject ) )
            {
                continue;
            }
            for ( auto& current : open->Actions() )
            {
                if ( current.Label != label )
                {
                    continue;
                }
                // AN ACTION WITH NO CLOSURE IS NOT A NO-OP: calling an empty std::function throws, and a
                // document that published a label with nothing behind it has a defect worth naming.
                if ( !current.Run )
                {
                    return Common::MakeFormattedError<bool>(
                         "the document offers '{}' with nothing behind it — the label was published without "
                         "an action",
                         label );
                }
                current.Run();
                return PaletteCommandDone();
            }
        }
        return Common::MakeFormattedError<bool>(
             "the document that offered '{}' is gone, or no longer offers it (a view mode's label changes "
             "with the mode it is in)",
             label );
    }

    void DocumentHost::AppendCloseAllCommand( std::vector<PaletteCommand>& commands )
    {
        commands.push_back( { "Action", "Close All Documents", [this]
                              {
                                  RequestCloseAllDocuments();
                                  return PaletteCommandDone();
                              } } );
    }

    void DocumentHost::AppendDocumentCommands( std::vector<PaletteCommand>& commands )
    {
        // Documents — FOCUS an open one. A separate category because the verb is different and the
        // difference is the point of this task: a tool is opened, a document is switched to. Nothing here
        // creates or destroys a window, so a mistyped search cannot cost the user one.
        for ( const auto& document : m_OpenDocuments )
        {
            const SubjectId subject = document->Subject();
            commands.push_back( { "Document", "Go to " + DocumentDisplayName( document->GetName() ),
                                  [this, subject]
                                  {
                                      FocusDocument( subject );
                                      return PaletteCommandDone();
                                  } } );
        }

        // AND WHAT AN OPEN DOCUMENT CAN DO, which until now was nothing the palette knew about. A view mode
        // inside a window lives on a button, and a button is the gesture an unattended run cannot make —
        // so "the Sequencer can show its keys as curves" was a claim with no way to photograph it.
        //
        // The document's own name prefixes the label, because two Sequencers over two rigs would otherwise
        // offer two identical entries and the palette matches on the label exactly.
        for ( const auto& document : m_OpenDocuments )
        {
            const SubjectId   subject = document->Subject();
            const std::string name    = DocumentDisplayName( document->GetName() );
            for ( auto& action : document->Actions() )
            {
                // The LABEL is captured, not the action: a document can be destroyed between building this
                // list and running an entry, and re-resolving by subject is what every other document
                // command here does for the same reason.
                const std::string label = action.Label;
                // NOT A LAMBDA, and that is a finding rather than a style: `bugprone-exception-escape`
                // fires on a parameter-less lambda in this tree (ScenePropertiesPanel.cpp:92 records the
                // same one), and `PaletteCommand::Run` takes no parameters, so there is no version of a
                // lambda here that the check accepts. `bind_front` binds the member function directly and
                // there is nothing for it to analyse.
                commands.push_back(
                     { "Document", name + ": " + label,
                       std::bind_front( &DocumentHost::RunDocumentAction, this, subject, label ) } );
            }
        }

        // Closing one, by name. Never offered before, because a person closes a window with the x on it —
        // which is exactly the gesture no unattended run can make, and therefore the reason "close a
        // document and show what the well offers back" was a claim nobody could photograph. It goes
        // through RequestDocumentClose like the x does, so the destruction still happens between frames
        // behind the device-idle wait.
        for ( const auto& document : m_OpenDocuments )
        {
            const SubjectId subject = document->Subject();
            commands.push_back( { "Document", "Close " + DocumentDisplayName( document->GetName() ),
                                  [this, subject]
                                  {
                                      AskDocumentClose( subject, "closed from the command "
                                                                 "palette" );
                                      return PaletteCommandDone();
                                  } } );
            // The Don't Save answer as one command, for runs that cannot click a modal: raises the question
            // if nothing has yet, then answers it through the same path the button takes.
            commands.push_back(
                 { "Document", DocumentDisplayName( document->GetName() ) + ": Close Discard", [this, subject]
                   {
                       if ( m_OpenDocuments.Find( subject ) == nullptr )
                           return Common::MakeError<bool>( "the document to close is no longer "
                                                           "open." );
                       const bool asked = std::any_of( m_CloseQuestions.begin(), m_CloseQuestions.end(),
                                                       [&subject]( const PendingDocumentClose& p )
                                                       { return p.Subject == subject; } );
                       if ( !asked )
                           m_CloseQuestions.push_back( PendingDocumentClose{
                                subject, "closed from the command palette without saving" } );
                       (void)AnswerCloseQuestion( subject, UnsavedCloseChoice::Discard );
                       return PaletteCommandDone();
                   } } );
        }

        // Ctrl+Tab, as a command. The key is bound in OnUIRender and a key is not available to a
        // client either; this is the same CycleDocuments the keystroke calls, so the ring the two walk
        // cannot differ.
        if ( m_OpenDocuments.Count() > 1 )
        {
            commands.push_back( { "Document", "Cycle to the next most recently used", [this]
                                  {
                                      CycleDocuments();
                                      return PaletteCommandDone();
                                  } } );
        }

        // The well's window, reachable without a mouse: Window > Documents and its x.
        commands.push_back( { "Document", "Show the Documents window", [this]
                              {
                                  m_DocumentWell.ShowWindow();
                                  return PaletteCommandDone();
                              } } );
        commands.push_back( { "Document", "Close the Documents window", [this]
                              {
                                  m_DocumentWell.CloseWindow();
                                  return PaletteCommandDone();
                              } } );

        // Reopening one that was closed. The list the empty well shows, reachable without a mouse — and
        // it is the same Core::SubjectOpenRequests the Selectable there uses, so a reopen is refused by the
        // six-slot cap exactly like any other open rather than becoming a second way in.
        for ( const ClosedDocument& closed : m_DocumentWell.RecentlyClosed() )
        {
            const SubjectId subject = closed.Subject;
            commands.push_back( { "Document", "Reopen " + closed.DisplayName, [subject]
                                  {
                                      Core::SubjectOpenRequests::Request( subject );
                                      return PaletteCommandDone();
                                  } } );
        }
    }

    void DocumentHost::AppendOpenCommands( std::vector<PaletteCommand>&              commands,
                                           const std::vector<std::filesystem::path>& assetFiles )
    {
        // OPENABLE ASSETS. This is where `--open-panel <path-to-asset>` went — the half of that flag that
        // opened a DOCUMENT rather than a tool, and the only way a document has ever been put on screen
        // unattended, since a document does not exist until something opens its asset and therefore has
        // no name to be reached by.
        //
        // ENUMERATED FROM THE PROJECT'S FILES, NOT FROM THE ASSET MANAGER'S CACHE. This loop used to walk
        // `RegisteredAssets()` — whatever the startup preloader had got round to registering — which is a
        // container whose contents are derived from the same source as the question being asked of it.
        // Measured through the control channel, once per frame: the group goes 0 -> 106 -> 130 entries,
        // because FIVE separate startup stages fill that cache, so for 3.3 s of every boot the palette
        // successfully offered every material in this project and none of its twenty-four cloud assets.
        //
        // The entity half above never had that problem, and the reason is the shape: it walks the SCENE,
        // which is what says which entities exist. The equivalent for files is the content enumeration —
        // ListFilesRecursive, which is also what the preloader walks to build the cache in the first
        // place, and which covers a mounted .dpak as well as loose files. Reading it one step earlier
        // removes the window rather than shortening it.
        //
        // Nothing is loaded to build this list, which is the other half of the argument: a project with
        // ten thousand materials costs one directory walk here, and the file is parsed by the OPENER, on
        // the frame somebody actually asks for it.
        //
        // See Editor/Core/OpenableAssets.hpp for the labelling rule and the three `model.demat` that
        // motivated it.
        for ( const OpenableAsset& asset : CollectOpenableAssets( assetFiles, m_SubjectEditors.ClaimedExtensions(),
                                                                  Common::Constants::Path::ASSETS_PATH ) )
        {
            const std::string path = asset.Path;
            commands.push_back( { "Open", asset.Label, [this, path]
                                  {
                                      // THROUGH THE PATH OPENERS, the same route the asset browser's
                                      // double-click takes. Resolving a path to a subject here would be a
                                      // second copy of the find-or-create-and-load chain — the exact
                                      // duplication SubjectEditorRegistry::RegisterPathOpener was
                                      // introduced to delete, when the browser and EditorLayer each
                                      // carried one.
                                      //
                                      // AND THE OUTCOME IS ANSWERED, WHICH IS A6-2 POINT 1. `(void)` stood
                                      // here: a `.demat` that would not resolve logged its reason and came
                                      // back over the channel as `{"ok":true}`, so a script opened nothing
                                      // and carried on. The opener has already said WHY in the log, with
                                      // the path — this refuses without repeating a guess at the cause,
                                      // exactly as PathOpenOutcome::Failed is documented to mean.
                                      switch ( m_SubjectEditors.OpenPath( path ) )
                                      {
                                          case SubjectEditorRegistry::PathOpenOutcome::Requested:
                                              return PaletteCommandDone();
                                          case SubjectEditorRegistry::PathOpenOutcome::Failed:
                                              return Common::MakeFormattedError<bool>(
                                                   "'{}' is a document this editor opens and it would not "
                                                   "resolve; the log line above names the reason.",
                                                   path );
                                          case SubjectEditorRegistry::PathOpenOutcome::NotMine:
                                              // The palette offered it, so an opener claimed its
                                              // extension — NotMine here means the file has GONE since
                                              // the dictionary was built, which is a fact and not a
                                              // no-op.
                                              return Common::MakeFormattedError<bool>(
                                                   "'{}' is no longer there — no registered opener claims "
                                                   "it now. It existed when this list was built.",
                                                   path );
                                      }
                                      return Common::MakeFormattedError<bool>(
                                           "opening '{}' produced an outcome this build does not handle; "
                                           "that is a defect in the palette, not in the request.",
                                           path );
                                  } } );
        }
    }
} // namespace Desert::Editor
