#pragma once

#include <memory>
#include <functional>
#include <string>
#include <vector>

#include <ImGui/imgui.h>

#include <Common/Core/Events/Event.hpp>
#include <Common/Core/ResultStr.hpp>

#include <Editor/Core/EditableProperty.hpp>
#include <Editor/Core/EditorSubject.hpp>
#include <Editor/Core/PreviewViewpoints.hpp>

#include <Engine/Assets/Common.hpp>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor
{
    class IPanel
    {
    public:
        explicit IPanel( std::string&& panelName, bool showPanel = true )
             : m_PanelName( std::move( panelName ) ), m_SowPanel( showPanel )
        {
        }

        virtual void OnEvent( Common::Event& /*e*/ )
        {
        }
        virtual void OnPreUpdate()               {}
        virtual ~IPanel()                        = default;
        virtual void       OnUIRender()          = 0;

        // Multi-scene editing: rebind a scene-bound panel to the newly-focused scene so the Outliner,
        // Details, Settings, etc. follow whichever viewport the user is working in. No-op for panels that
        // are not tied to a specific scene (asset browser, logs, ...). See EditorLayer::SetActiveScene.
        virtual void SetScene( const std::shared_ptr<Desert::Core::Scene>& /*scene*/ )
        {
        }
        const std::string& GetName() const
        {
            return m_PanelName;
        }

        virtual void ToggleVisibility() final
        {
            m_SowPanel = !m_SowPanel;
        }

        virtual bool& GetVisibility()
        {
            return m_SowPanel;
        }

        // --- Contextual panels ---------------------------------------------------------------------
        // A tool panel is only meaningful for a particular selection or mode: the Sequencer for something
        // animatable, the Particle Editor for an emitter, Modeling for Modeling mode. Rather than hanging
        // around empty ("Select a ... to ..."), such a panel OPENS ITSELF when its context appears and
        // steps aside when it goes away — the editor shows what the work needs, not everything at once.
        //
        // Explicit intent always wins: opening a panel by hand (View menu / command palette) PINS it, and
        // a pinned panel is never auto-closed. Closing it by hand unpins it again.
        virtual bool IsContextual() const
        {
            return false;
        }

        // Is this panel's context present right now? Only consulted when IsContextual().
        virtual bool IsRelevant() const
        {
            return true;
        }

        bool& Pinned()
        {
            return m_Pinned;
        }

        // Window padding for this panel. One number for the whole editor keeps every panel's content
        // breathing the same way; the viewport overrides it to zero because its image must reach the
        // window edges. (Pushed by the panel loop around Begin/End — panels don't do it themselves.)
        virtual ImVec2 GetWindowPadding() const
        {
            return ImVec2( 8.0f, 8.0f );
        }

        // Preferred window size the FIRST time the panel ever opens (0,0 = let ImGui decide). Once
        // the user moves/resizes it, imgui.ini remembers their layout instead. Floating tool windows
        // (Node Graph, Sequencer, Build Settings) override this so they don't pop up as tiny
        // arbitrarily-placed windows.
        virtual ImVec2 GetDefaultSize() const
        {
            return ImVec2( 0.0f, 0.0f );
        }

    protected:
        const std::string m_PanelName;
        bool              m_SowPanel;
        bool              m_Pinned = false; // opened by hand: never auto-closed (see IsContextual)
    };

    // The window title a document must carry: "<display name>###doc<subject>".
    //
    // THE ###id IS NOT DECORATION. Every panel is drawn with ImGui::Begin( PanelDisplayTitle( GetName() ) ),
    // and ImGui derives a window's identity from the text after the LAST "###" — so two windows whose titles
    // agree there are ONE window, merged, with the second one's content drawn into the first. Every panel
    // before this was a singleton and never met the problem; ViewportPanel is the one type that did, and it
    // escapes exactly this way ("<name>###sceneview<id>", EditorLayer::AddSceneView). This is that same
    // escape, keyed on the SUBJECT rather than on a counter, which is also what makes open-or-focus fall out
    // for free: the same material can only ever produce the same title, hence the same window.
    //
    // The subject and not a fresh id, deliberately: an id source would let one material open twice, and the
    // two windows would then edit one asset through two parameter tables.
    //
    // THE WHOLE SUBJECT AND NOT ITS OWNER. This used to append the subject handle alone, which was
    // unambiguous only while every document was an asset. It no longer is: an entity's UUID and an asset's
    // handle are both 64-bit ids drawn from the same space, and two documents over one entity — its anim
    // graph and its particle emitter — share an owner and differ only in the facet. Appending
    // SubjectId::ToString() puts the domain and the facet in the id too, so those are three distinct
    // windows rather than one window three things fight over.
    //
    // The display half is stripped of any "###" of its own, and that is not defensive programming: what
    // EditorLayer hands to Begin() is PanelDisplayTitle(GetName()), which appends "###" + the whole name
    // again, and ImGui takes the id from the LAST "###" in the string. A display name carrying one (an asset
    // file may be named anything) would therefore end up deciding the window id — and two such assets would
    // merge into one window, which is the exact failure this function exists to prevent.
    [[nodiscard]] inline std::string DocumentTitle( const std::string& displayName, const SubjectId& subject )
    {
        std::string label = displayName;
        if ( const auto pos = label.find( "###" ); pos != std::string::npos )
            label.erase( pos );

        return label + "###doc" + subject.ToString();
    }

    // A panel that edits ONE SUBJECT: a document, not a tool.
    //
    // The difference that matters to the editor: a tool panel is created once at startup and toggled, so its
    // name is a constant and hiding it is the whole of "closing" it. A document is created when the user
    // opens an asset and DESTROYED when the window is dismissed — that destruction is what returns the
    // Scene, the SceneRenderer and one of the six renderer slots, and there is no way to write it as a
    // visibility flag.
    //
    // WHICH IS WHY THE TWO HAVE DIFFERENT OWNERS. A document is never in the tool registry
    // (Editor/Core/PanelRegistry.hpp, which refuses one at compile time); it lives in
    // Editor/Core/OpenDocuments.hpp, and it is closed by EditorLayer::RequestDocumentClose rather than by
    // anything writing GetVisibility(). While the two shared a container they shared that bool too, and
    // "hide" for a tool therefore had to mean "destroy" for a document: unticking one in the View menu
    // destroyed it, and re-ticking could not bring it back.
    //
    // GetVisibility() is inherited here and MEANS NOTHING for a document. Nothing reads it, nothing writes
    // it, and nothing should: a document is open, or it does not exist.
    //
    // NOT "IAssetEditorPanel" ANY MORE, and the rename is the task rather than tidying. The subject was an
    // Assets::AssetHandle, so a document could only ever be a file — which made the Details-panel button
    // the owner asked for ("edit the thing this component holds") inexpressible, since the request it
    // would send has nowhere to put a component. Editor/Core/EditorSubject.hpp has the whole argument for
    // what replaced it.
    class ISubjectDocument : public IPanel
    {
    public:
        ISubjectDocument( const std::string& displayName, const SubjectId& subject )
             : IPanel( DocumentTitle( displayName, subject ), /*showPanel=*/true ), m_Subject( subject )
        {
        }

        // WHAT THIS DOCUMENT EDITS, fixed for its whole life. Immutable because it is the window's identity:
        // the title, and therefore the ImGui window id, is derived from it, and a title that changed under a
        // live window would either merge it into another document's window or orphan its saved dock entry.
        // Editing a different subject means opening a different document.
        [[nodiscard]] const SubjectId& Subject() const noexcept
        {
            return m_Subject;
        }

        // ── IS THE SUBJECT STILL THERE? ────────────────────────────────────────────────────────────────
        //
        // The owner's decision: a document CLOSES WITH ITS SUBJECT. Delete the entity, or delete the asset,
        // and the window goes with a named reason rather than staying open over nothing.
        //
        // PURE VIRTUAL, and that is the point of it. A default returning true would be a document asserting
        // its subject exists on no evidence — which is the shape this editor has paid for before
        // (GetDiskState's own note below: "UNTRACKED IS NOT CLEAN"). Every document already resolves its
        // subject to draw anything at all; this asks it to say what that resolution found.
        //
        // ASKED, NOT WATCHED. There is no deletion event to subscribe to that covers both an asset removed
        // from the manager and an entity destroyed in any of the open scenes, and a subscription that
        // covered one of the two would be worse than none: the uncovered half would look handled.
        [[nodiscard]] virtual bool IsSubjectAlive() const = 0;

        // ── WHAT THIS WINDOW CAN BE ASKED TO DO, BY SOMETHING WITHOUT A MOUSE ─────────────────────────
        //
        // A document's own view modes — "show these keys as curves", "show the graph's parameters" — live
        // on buttons inside it, and a button is the one gesture an unattended run cannot make. Every claim
        // about such a mode was therefore unphotographable: `--shot` draws no interface at all, and the
        // control channel could reach the command palette but the palette knew nothing a document could do.
        //
        // A LABEL AND A CLOSURE, and deliberately nothing more. Not a registry, not an id namespace: this
        // exists so that a window's modes are reachable, and an id scheme would be a second name for each
        // of them to drift from. The palette prefixes the document's own name, so two Sequencers do not
        // offer two identical entries.
        //
        // Empty by default: a document with no modes has nothing to say here, and that is not a stub.
        struct DocumentAction
        {
            std::string           Label;
            std::function<void()> Run;
        };

        [[nodiscard]] virtual std::vector<DocumentAction> Actions()
        {
            return {};
        }

        // Is this document holding one of the six renderer slots RIGHT NOW?
        //
        // Asked by the slot census before a seventh consumer is admitted. A document that is open but has
        // not drawn yet holds nothing and still has a claim coming, which is why the census counts pending
        // demand separately rather than trusting the live-renderer count alone.
        [[nodiscard]] virtual bool HoldsRendererSlot() const = 0;

        // Will this document EVER claim one of the six renderer slots?
        //
        // HoldsRendererSlot answers "right now"; this answers "ever", and the census needs both. It counts
        // an open document that holds no slot as PENDING DEMAND, because a Material Editor that has not
        // drawn yet is a claim that has not landed — but a document that renders on the CPU has no claim
        // coming at all, and counting it would refuse a window that costs nothing. With the four cloud
        // documents (which bake on the CPU and upload an Image2D) that is not hypothetical: five of them
        // open beside the main viewport made `live + pending` reach the cap, and the sixth was refused with
        // a census telling the user to close windows that were holding nothing.
        //
        // Defaults to TRUE, which is the conservative answer: a new document type that forgets to say is
        // treated as a claimant and refused early, rather than admitted past the cap and discovered as two
        // surfaces quietly trading each other's per-frame camera some minutes later.
        [[nodiscard]] virtual bool ClaimsRendererSlot() const
        {
            return true;
        }

        // ── GIVE THE SLOT BACK WHILE NOBODY IS LOOKING ─────────────────────────────────────────────────
        //
        // There are six renderer slots and a document holds one for as long as it is OPEN, which is not
        // the same as for as long as it is SEEN. Four documents docked as tabs in one node show one tab;
        // the other three were rendering a preview nobody could see and holding three of the six slots
        // while they did it, so the fifth document the user opened was refused over resources that were
        // being spent on hidden windows.
        //
        // THE ALTERNATIVE WAS TO CLOSE THE DOCUMENT ON FOCUS LOSS, AND THE OWNER REFUSED IT: a layout that
        // rearranges itself reads as an editor that lost your panel. So the window stays and the RESOURCE
        // goes. Called by EditorLayer from ServiceDocumentCloses — behind the same device-idle wait a close
        // uses, and for the same reason: the last submitted frame may still be executing against the
        // renderer this releases.
        //
        // AFTERWARDS HoldsRendererSlot() MUST ANSWER FALSE. That is the contract, it is what the editor
        // logs an error about if it is broken, and it is what a suite can assert over a document with no
        // renderer anywhere near (DocumentOwnership). The default body is empty because a document that
        // never claims a slot has nothing to give back — for one that does, an empty override would fail
        // that check out loud rather than quietly keep the slot.
        virtual void ReleaseRendererSlot()
        {
        }

        // Does this document show a 3D PREVIEW that can be put at a named viewpoint?
        //
        // Asked by the command palette, which offers "Preview: Front", "Preview: Back" and the rest for
        // whichever document has the focus — the replacement for `--preview-orbit yaw,pitch`, whose
        // continuous angle pair a palette entry has nowhere to carry (Editor/Core/PreviewViewpoints.hpp
        // has the whole argument). A document with no preview offers none of those entries, rather than
        // offering seven that do nothing.
        [[nodiscard]] virtual bool HasPreview() const
        {
            return false;
        }

        // Put that preview at @p viewpoint. Only called when HasPreview(); the default does nothing
        // because a document without a preview is never asked.
        virtual void SetPreviewViewpoint( const PreviewViewpoint& /*viewpoint*/ )
        {
        }

        // ── WHAT AN EDIT HAS REACHED: three states, not one ────────────────────────────────────────────
        //
        //   WORKING   what this window shows and the artist is changing
        //   APPLIED   what every open scene is rendering
        //   ON DISK   what the file holds
        //
        // Before this interface existed there was ONE state. A document wrote its edit into the asset in
        // memory and pushed it at the runtime in the same statement, so moving a slider changed every mesh
        // in the level immediately and there was no way back at all — not a reload, not a snapshot. That
        // was defended as the only way the preview and the scene could be guaranteed to agree, and the
        // defence confuses two questions: whether the preview shows what the material WILL be (it must)
        // and whether the scene shows edits nobody has accepted (it must not). Agreement comes from one
        // shared working copy, not from the two audiences being literally one object; identity is stronger
        // than agreement, and its price was that an accidental drag was permanent and the next deliberate
        // Save wrote the accident to disk.
        //
        // Declared HERE and not privately in the Material Editor because it is not one window's problem.
        // The dirty dot on a document tab, the question on close, "Save All", one question at exit, the
        // modified badge on a browser tile — every one of them needs exactly these answers from every
        // document type, and there are five.
        enum class EditModel
        {
            WriteThrough, // an edit is in the scene the moment it is made; there is no way back
            Staged        // an edit waits in a working copy until ApplyEdits()
        };

        [[nodiscard]] virtual EditModel GetEditModel() const
        {
            return EditModel::WriteThrough;
        }

        // WORKING differs from APPLIED.
        //
        // FALSE for a write-through document is a fact and not a default standing in for one: its edit
        // reached the scene as it was made, so nothing is outstanding.
        [[nodiscard]] virtual bool HasUnappliedEdits() const
        {
            return false;
        }

        // Publish WORKING into APPLIED — the scene changes HERE and nowhere else.
        //
        // Returns whether the published state actually moved, so a caller cannot read "there was nothing
        // to publish" as "it was published". False for a write-through document (already published) and
        // for a staged one with no outstanding edit.
        virtual bool ApplyEdits()
        {
            return false;
        }

        // WORKING <- APPLIED: the way back.
        //
        // IT RETURNS A BOOL BECAUSE "THIS DOCUMENT HAS NO WAY BACK" MUST BE SAYABLE. A Discard that
        // quietly did nothing would rebuild the exact trap this interface removes — a person believing an
        // edit is reversible because the editor showed them something that says so. A caller offers the
        // action only for EditModel::Staged, and a false return from one of those is a defect in the
        // document, not a state to swallow.
        virtual bool DiscardEdits()
        {
            return false;
        }

        // Does what this document holds differ from its file?
        //
        // UNTRACKED IS NOT CLEAN. A document that takes no snapshot when it opens cannot answer, and a
        // caller that drew "no dot" for it would be asserting the file is up to date — a claim it has no
        // evidence for. The dot belongs on Dirty alone. Untracked is what a document answers until it
        // takes a snapshot of its own.
        enum class DiskState
        {
            Clean,
            Dirty,
            Untracked
        };

        [[nodiscard]] virtual DiskState GetDiskState() const
        {
            return DiskState::Untracked;
        }

        // Write WORKING to the file. Returns whether the file was actually written, for the same reason
        // ApplyEdits returns whether anything moved: a caller must not report a save that did not happen.
        //
        // Declared here so "Save the focused document" can be a command like any other. A document type
        // that has no file of its own leaves this false and is simply not offered the entry.
        virtual bool SaveDocument()
        {
            return false;
        }

        // ── DIRECT MANIPULATION: the properties this document exposes, and the one way to write them ────
        //
        // The other half of what a person can do. The command palette covers everything with a NAME —
        // open, close, apply, save; this covers everything with a VALUE, which is the half a mouse does by
        // dragging and which no dictionary of actions can express (see Editor/Core/EditableProperty.hpp
        // for the whole argument, and for why this is a second CATEGORY and not a second execution path).
        //
        // A document that offers no properties answers with an empty census and refuses every write,
        // saying so. That is the honest default: a silent no-op here would tell a client that a value it
        // sent had been accepted.

        /// Everything this document can be asked to change, DERIVED from whatever declares it — for a
        /// material, the shader's own schema. Never a list maintained by hand; see EditableProperty.hpp.
        [[nodiscard]] virtual std::vector<EditableProperty> EditableProperties() const
        {
            return {};
        }

        /// Set one of them. @p value carries as many components as the caller sent, and the document is
        /// what checks that against its own declaration — the count is part of the property's identity,
        /// so three numbers for a float is a caller who meant a different property.
        ///
        /// THE WRITE MUST GO THROUGH THE SAME SETTER THE WIDGET CALLS. An implementation that reached the
        /// value by its own route would be a second execution path, and the two would drift on the day
        /// somebody adds a step to the widget's — the publish, the undo entry, the dirty derivation. What
        /// this method is allowed to do is CALL that path.
        [[nodiscard]] virtual Common::BoolResultStr SetEditableProperty( const std::string& name,
                                                                         const std::vector<float>& /*value*/ )
        {
            return Common::MakeFormattedError<bool>(
                 "this document exposes no editable properties, so '{}' cannot be set on it. Ask "
                 "'properties' for the ones the focused document offers.",
                 name );
        }

    private:
        const SubjectId m_Subject;
    };
} // namespace Desert::Editor