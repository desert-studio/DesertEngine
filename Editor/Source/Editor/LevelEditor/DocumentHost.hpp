#pragma once

#include "Editor/Core/CommandPalette.hpp"
#include "Editor/Core/DocumentPlacement.hpp"
#include "Editor/Core/DocumentWell.hpp"
#include "Editor/Core/OpenDocuments.hpp"
#include "Editor/Core/SubjectEditorRegistry.hpp"
#include "Editor/Core/UnsavedClose.hpp"
#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/ViewBudgetGate.hpp>
#include <ImGui/imgui.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    class SceneWorkspace;

    // THE OPEN ASSET DOCUMENTS AND EVERYTHING ABOUT THEIR LIFE: opening (and refusing past the view budget),
    // focus and the Ctrl+Tab ring, closing (and the Save / Don't Save / Cancel question first), the document
    // well and the major tabs. UE: UAssetEditorSubsystem (open/close/focus) + the document half of
    // FGlobalTabmanager. The tool panels are not here — they are PanelRegistry's (EditorLayer::m_Panels).
    //
    // Collaborators arrive by reference; no pointer back to EditorLayer. The one EditorLayer service it needs
    // ("show this folder in the Assets browser", for a Details field's Show in browser) comes as a function.
    class DocumentHost
    {
    public:
        using ShowFolderFn = std::function<Common::BoolResultStr( const std::string& folder )>;

        DocumentHost( SceneWorkspace& workspace, std::shared_ptr<Assets::AssetManager>& assetManager,
                      std::string& focusWindow, ShowFolderFn showFolder );
        DocumentHost( const DocumentHost& )            = delete;
        DocumentHost& operator=( const DocumentHost& ) = delete;

        // The registry the asset-editor registrations fill (RegisterAssetEditors, AssetEditorRegistrations.hpp)
        // and every opener reads.
        [[nodiscard]] SubjectEditorRegistry& SubjectEditors()
        {
            return m_SubjectEditors;
        }
        [[nodiscard]] const SubjectEditorRegistry& SubjectEditors() const
        {
            return m_SubjectEditors;
        }
        [[nodiscard]] OpenDocuments& Documents()
        {
            return m_OpenDocuments;
        }
        [[nodiscard]] const OpenDocuments& Documents() const
        {
            return m_OpenDocuments;
        }
        [[nodiscard]] DocumentWell& Well()
        {
            return m_DocumentWell;
        }
        [[nodiscard]] const DocumentWell& Well() const
        {
            return m_DocumentWell;
        }
        [[nodiscard]] const SubjectId& FocusedDocument() const
        {
            return m_FocusedDocument;
        }
        [[nodiscard]] bool DocumentHasFocus() const
        {
            return m_DocumentHasFocus;
        }
        // Outstanding work the control channel's quiescence gate waits on.
        [[nodiscard]] bool HasPendingCloses() const
        {
            return !m_DocumentsToClose.empty();
        }
        [[nodiscard]] bool HasPendingRefusal() const
        {
            return m_OpenRefusalPending;
        }
        // A document's glyph: its registration's icon, or the editor's one answer for an unclaimed type.
        [[nodiscard]] std::string DocumentIcon( const SubjectId& subject ) const;
        // The well's own ImGui window name (the default layout docks it).
        [[nodiscard]] static const char* WellWindowTitle();

        // imgui.ini handlers: the well's open state and the remembered placement per document kind.
        void RegisterDocumentWellLayoutHandler();
        void RegisterDocumentPlacementHandler();

        // The rect the major tab's document fills: the level dockspace's, measured by the dock host each frame.
        void SetMajorTabArea( const glm::vec2& origin, const glm::vec2& size )
        {
            m_MajorTabOrigin = origin;
            m_MajorTabSize   = size;
        }

        // Ctrl+Tab over the documents and the ring commit on Ctrl's release (see CycleDocuments).
        void UpdateCycleShortcut( const ImGuiIO& io );

        // Leaving the editor: every dirty document asks first; @p onAllAnswered runs once the last question
        // is answered without a Cancel (at once when nothing is dirty). Cancel on any of them drops it.
        void AskCloseAll( std::function<void()> onAllAnswered );

        // Palette providers (CommandRegistry, groups "Documents" and "Open"); @p assetFiles is the census the
        // palette build took (EditorLayer::m_PaletteAssetFiles).
        void AppendDocumentCommands( std::vector<PaletteCommand>& commands );
        // "Action / Close All Documents" (each dirty one asks first).
        void AppendCloseAllCommand( std::vector<PaletteCommand>& commands );
        void AppendOpenCommands( std::vector<PaletteCommand>&              commands,
                                 const std::vector<std::filesystem::path>& assetFiles );

        // Runs one action a document published (ISubjectDocument::Actions), addressed by subject + label.
        // Named rather than a lambda in the list above — see the definition for both reasons.
        [[nodiscard]] Common::BoolResultStr RunDocumentAction( const SubjectId&   subject,
                                                               const std::string& label );

        // ===== Asset documents (one window per asset, opened from the browser) =====
        // Drains Core::SubjectOpenRequests and, per request, focuses the document already open on that subject
        // or builds a new one through m_AssetEditors. Runs from OnUpdate (between frames) because it adds to
        // m_OpenDocuments, and REFUSES past the six renderer slots with the census printed by name — a seventh
        // consumer would otherwise be handed slot 0 to share, which fails silently and days later.
        void ServiceSubjectOpenRequests();
        // Destroys every document the user asked to close, behind ONE device-idle wait. This is what returns
        // the document's Scene, SceneRenderer and renderer slot.
        //
        // The request comes from m_DocumentsToClose, filled by the x on the window, the x in the Documents
        // menu or Close All — never from a visibility flag. That is the point of the split: a tool's
        // visibility is a setting the user keeps, and while documents shared the panel list they shared that
        // bool too, so unticking one in the View menu DESTROYED it and re-ticking could not bring it back.
        void ServiceDocumentCloses();
        // Asks for a document to be closed. Queued, never immediate: closing destroys GPU resources, which
        // is not legal from inside the ImGui pass that is drawing them.
        // @p reason is why, in the user's words, and it is REQUIRED. A document now closes for three
        // different causes — the user dismissed it, Close All, or its subject stopped existing — and a log
        // line that could not tell them apart would make "my window vanished" unanswerable.
        void RequestDocumentClose( const SubjectId& subject, std::string reason );
        // A close THE PERSON asked for. A dirty document is not queued: it gets the Save / Don't Save / Cancel
        // question (Editor/Core/UnsavedClose.hpp) and closes only on the answer. Every user gesture that
        // closes a document goes through here; RequestDocumentClose stays for closes the editor makes on its
        // own (the subject is gone, view memory is short), which have nobody to ask.
        void AskDocumentClose( const SubjectId& subject, std::string reason );
        // The answer to a pending close question for @p subject. Returns whether the document is now queued
        // for closing; false for Cancel, for a Save that wrote nothing, and when no question was pending.
        bool AnswerCloseQuestion( const SubjectId& subject, UnsavedCloseChoice choice );
        void DrawCloseQuestionPopup();
        // Every open document, queued for closing. One implementation behind Window ▸ Close All Documents
        // and behind the palette entry of the same name — the menu item used to carry the loop itself, and
        // a second copy of it in the palette would be two answers to "what does Close All close".
        void RequestCloseAllDocuments();

        // ── A DOCUMENT CLOSES WITH ITS SUBJECT ────────────────────────────────────────────────────────
        //
        // Queues a close for every open document whose IsSubjectAlive() has gone false — the entity was
        // deleted, the component removed, the asset dropped from the manager, the scene closed. The owner
        // ruled out closing on focus loss (a layout that moves itself reads as a lost panel) and ruled IN
        // closing with the subject, because the alternative is a window editing nothing.
        //
        // A SWEEP AND NOT A SUBSCRIPTION: there is no single event that covers all four ways a subject can
        // die, and a subscription to one of them would make the other three look handled.
        void CloseDocumentsWhoseSubjectIsGone();

        // Hands the renderer slot back for every document whose window has been undrawn for
        // kFramesHiddenBeforeSlotRelease frames. Called from ServiceDocumentCloses so it shares that
        // function's device-idle wait — see ISubjectDocument::ReleaseView.
        void ReleaseSlotsOfHiddenDocuments();

        // The label a component-subject document is named with: the entity's tag plus what the window is
        // about ("Hero · Anim Graph"). Resolved ONCE, when the document is built — a document must not
        // need a scene to know its own name, and renaming the entity must not open a second window.
        [[nodiscard]] std::string SubjectEntityName( const SubjectId& subject, const char* what ) const;

        // The name to put in a REFUSAL dialog for a subject no document was built for: the asset's file
        // stem, or the entity's tag. "this subject" when neither resolves — the refusal happens before any
        // editor is consulted, so this is all that is knowable about it.
        [[nodiscard]] std::string RefusedSubjectName( const SubjectId& subject ) const;
        // Brings @p subject's window to the front and makes it the most recently used document.
        void FocusDocument( const SubjectId& subject );
        // Ctrl+Tab: move to the next document in most-recently-used order. See DocumentWell::NextMostRecent.
        void CycleDocuments();

        // ===== The document well (layout option B.1) =====
        // The "Documents" window: the tab the documents dock beside, the index of what is open, and — when
        // nothing is open — the empty state that says what the area is for plus the list of recently closed
        // documents. It closes with its x like a tool (its neighbours take the area), comes back from
        // Window > Documents or by itself when a document is opened, and its open/closed state is a line of
        // the editor layout (DocumentWell::LayoutLine), so it survives a restart.
        // Closing the window closes no document: they stay open and the well shows the same tabs on return.
        // The window title one document is drawn with — its type's icon (from the registration), its
        // subject's name, and the identity DocumentTitle baked into GetName(). A member rather than a free
        // function because the icon comes from m_SubjectEditors.
        [[nodiscard]] std::string DocumentDisplayTitle( const ISubjectDocument& document ) const;

        void DrawDocumentWell();
        // Every open document, drawn into the well's dock node. Separate from the tool loop because the two
        // have separate owners and separate close semantics — a tool passes &GetVisibility() to Begin, a
        // document passes a frame-local bool whose false is a CLOSE REQUEST, not a hidden window.
        void DrawDocuments();
        // UE's major tabs: "Scene" plus one tab per open document that OpensAsMajorTab(); the one in front
        // owns the whole dock area and the level's panels are not drawn.
        void               DrawMajorTabStrip();
        [[nodiscard]] bool MajorTabActive() const
        {
            return !m_ActiveMajorTab.IsNull();
        }
        // The refusal, on screen. A seventh renderer consumer is refused; before this the refusal was a
        // line in the log and the click simply looked dead. The census text already existed — it had
        // nowhere to be shown.
        void DrawOpenRefusedPopup();

        // Who is holding a view right now, by name. Shown when an open is refused — "out of memory"
        // without the list leaves the user with nothing to close. The main viewport and every extra scene
        // view hold one for as long as they exist; the Details preview and each asset document are
        // demand-driven and may be open while holding nothing.
        struct ViewConsumer
        {
            std::string Name;
            bool        HoldsView = false;
            // Whether this consumer will ever build a view. A CPU-only asset document (the four cloud
            // editors) holds none and is not waiting for one, and the census has to say so — "will allocate
            // when it draws" would name it as something to close to free memory it was never going to take.
            // See ISubjectDocument::ClaimsView.
            bool ClaimsView = true;
            // What a document that claims a view but has not built it yet will allocate on its first frame
            // (ISubjectDocument::ViewForecastBytes). Zero for everything else: a view that exists is counted
            // by what it HOLDS (SceneRenderer::LiveHoldings), not by a forecast.
            uint64_t ForecastBytes = 0;
            // Set for a consumer the user can close FROM THE REFUSAL ITSELF: an open document. The main
            // viewport and the Details preview carry no handle — neither is a window a person closes to make
            // room. Last in the struct so the shorter aggregate initialisations below keep meaning what they say.
            std::optional<SubjectId> Document = {};
        };
        [[nodiscard]] std::vector<ViewConsumer> ViewCensus() const;

    private:
        SceneWorkspace&                        m_Workspace;
        std::shared_ptr<Assets::AssetManager>& m_AssetManager;
        ShowFolderFn                           m_ShowFolder;

        // AssetTypeID -> the editor that opens it. Holds factories only; the documents it builds are owned by
        // m_OpenDocuments below.
        SubjectEditorRegistry m_SubjectEditors;

        // THE OPEN DOCUMENTS, owned separately from the tools. See Editor/Core/OpenDocuments.hpp for the
        // whole argument; the short version is that a tool's visibility is a setting and a document's
        // existence is not, so one bool cannot serve both — and while they shared m_Panels it had to.
        //
        // DECLARED BEFORE THE VIEWS THAT READ IT, and the order is load-bearing rather than tidy: every view
        // below is constructed with a reference to this member.
        OpenDocuments m_OpenDocuments;
        // ONE VIEW OF THEM — the tabbed well, its Ctrl+Tab ring and its recently-closed list. The Clouds
        // window is a second view of the same container and neither knows the other exists.
        DocumentWell m_DocumentWell{ m_OpenDocuments };
        // Close requests, drained between frames by ServiceDocumentCloses. Filled by the x on a document
        // window, the x in Window ▸ Documents, Close All, and the refusal dialog's own Close buttons.
        struct PendingDocumentClose
        {
            SubjectId   Subject;
            std::string Reason;
        };
        std::vector<PendingDocumentClose> m_DocumentsToClose;
        // Close questions waiting for an answer, the front one shown as a modal. Same shape as the queue
        // above because an answer that closes moves the entry there unchanged.
        std::vector<PendingDocumentClose> m_CloseQuestions;

        // How long a document must go UNDRAWN BY EVERY VIEW before it gives its renderer slot back. A
        // document behind another one's tab is open and invisible, and it was holding one of the six
        // renderer slots for as long as the user left it there — see ISubjectDocument::ReleaseView.
        //
        // COUNTED RATHER THAN ACTED ON AT ONCE. Dragging a dock tab, collapsing a node and switching layouts
        // all hide a window for a frame or two, and tearing a Scene and a SceneRenderer down and building
        // them back for that would turn a flick of the mouse into a hitch. The threshold is the smallest
        // number of frames that is unambiguously "the user left it there" rather than "the layout moved".
        //
        // THE COUNT ITSELF LIVES ON m_OpenDocuments, not here, and the move is the point: with the Clouds
        // window there are two views that can draw a document, so "nobody drew it" is a fact about all of
        // them and cannot be maintained by either one. See OpenDocuments::NoteDrawn / EndFrame.
        static constexpr uint32_t kFramesHiddenBeforeSlotRelease = 30;
        // Which document window has the keyboard focus, as of the last frame. Drives the radio in
        // Window ▸ Documents and is where Ctrl+Tab starts from.
        SubjectId m_FocusedDocument;
        // Whether a document window has the keyboard NOW. m_FocusedDocument outlives the focus on purpose, so
        // Ctrl+S needs this separately to decide between the document's asset and the scene (SaveShortcut.hpp).
        bool m_DocumentHasFocus = false;
        // Ctrl+Tab holds the ring still. Landing on a document by cycling must NOT reorder the ring, or the
        // second press would come straight back to where the first started; the order is committed once Ctrl
        // is released, which is the behaviour every alt-tab ring has.
        bool m_CyclingDocuments = false;
        // What the layout file last said about the well's window; a difference marks imgui.ini dirty.
        bool m_DocumentWellOpenInLayout = true;

        // WHERE EACH DOCUMENT KIND WAS LAST PUT (Editor/Core/DocumentPlacement.hpp), keyed by DocumentKindKey —
        // domain and facet, stable across runs. Persisted in imgui.ini as [DocumentPlacement][Kinds], so a
        // named layout carries it too. The next opening of that kind goes back there.
        std::unordered_map<std::string, Editor::DocumentPlacement::Remembered> m_RememberedPlacement;
        // Documents whose opening has been placed. The placement is applied ONCE, on the first frame the
        // window exists; afterwards the window is the person's and is only observed.
        std::unordered_set<SubjectId> m_PlacedDocuments;
        SubjectId                     m_ActiveMajorTab; // null = the level ("Scene") is in front
        std::unordered_set<SubjectId> m_SeenMajorTabs;  // a tab not seen before comes to the front once
        glm::vec2                     m_MajorTabOrigin{ 0.0f };
        glm::vec2                     m_MajorTabSize{ 0.0f };

        // A refused open, waiting to be shown (see DrawOpenRefusedPopup). Holds the census by value: the
        // documents it names may be closed while the dialog is up, and a row pointing at a destroyed panel
        // is the dangling reference this split exists to avoid.
        struct OpenRefusal
        {
            std::string                 AssetName;
            std::string                 TypeName;
            Engine::ViewBudget::Verdict Verdict;          // RequestBytes = the document's forecast + PendingBytes
            Engine::ViewBudget::Reading Reading;          // the ceiling, its source and the usage it was judged on
            uint64_t                    PendingBytes = 0; // spoken for by open documents that have not drawn yet
            std::vector<Engine::ViewBudget::HeldView> Views; // SceneRenderer::LiveHoldings at the refusal
            std::vector<ViewConsumer>                 Census;
        };
        std::optional<OpenRefusal> m_OpenRefusal;
        bool                       m_OpenRefusalPending = false; // raise the modal on the next ImGui frame
        // AskCloseAll raised the questions: run this once the last one is answered (UE: the editor's exit
        // waits on the asset editors' close requests).
        std::function<void()> m_AfterCloseQuestions;
        // The window to bring forward on the next frame (by its ImGui name); cleared when drawn. ONE slot for
        // the whole editor, owned by DockLayout (FocusSlot) and shared with the tool panels, so a later
        // focus request replaces an earlier one whether either names a panel or a document.
        std::string& m_FocusWindow;
    };
} // namespace Desert::Editor
