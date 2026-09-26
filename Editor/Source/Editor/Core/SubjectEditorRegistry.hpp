#pragma once

// DELIBERATELY NOT SubjectOpenRequest.hpp, even though this is what services one. That header opens
// `namespace Desert::Editor::Core`, and this one is included by EditorLayer.hpp — ahead of the render-system
// headers, which spell Desert::Core::Scene as an unqualified `Core::Scene` from inside Desert::Editor. Make
// Desert::Editor::Core visible before them and every one of those names silently rebinds to the wrong
// namespace. PreviewViewport.hpp carries a note about the same trap. So the registry takes the fields of a
// request rather than the request, and nothing here drags that namespace along.
#include <Engine/Core/ViewBudget.hpp>
#include <Editor/Panels/IPanel.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Editor
{
    // WHICH EDITOR OPENS WHICH KIND OF SUBJECT. One entry per subject TYPE — an asset kind or a component
    // kind — and the next kind is a second registration, not a second branch in the asset browser or a
    // fourth `if` in a double-click handler.
    //
    // KEYED ON SubjectTypeKey AND NOT ON Assets::AssetTypeID, which is the whole of task U7. While the key
    // was an asset type, only a file could have an editor: the anim graph, the particle emitter and the UI
    // canvas edit data that lives in a COMPONENT ON AN ENTITY, so there was no type to register them under
    // and no request that could carry their subject. Editor/Core/EditorSubject.hpp has the argument for the
    // key's shape.
    //
    // The registry deliberately does NOT own the documents it makes: it holds factories, and the documents
    // they build belong to Editor/Core/OpenDocuments.hpp. There is EXACTLY ONE owner of open documents and
    // therefore exactly one answer to "which documents exist" — a second container here would be a second
    // answer, and the two would disagree the first frame a close was handled halfway; the editor already
    // paid for that shape once (Editor/Core/SceneViewIdentity.hpp). Open-or-focus is OpenDocuments::Find, and
    // a second document for one subject is refused by OpenDocuments::Open rather than appended — which is
    // what makes a SECOND VIEW (the Clouds window) safe to add.
    //
    // The well is a separate owner from the TOOL panels for a different reason again, and that one is about
    // lifetime rather than bookkeeping: a tool's visibility is a setting the user keeps and a document's
    // existence is not, so one flag cannot serve both. See Editor/Core/PanelRegistry.hpp.
    class SubjectEditorRegistry
    {
    public:
        // Builds the document window for one subject — see Registration::Make.
        using Factory = std::function<std::unique_ptr<ISubjectDocument>( const SubjectId& )>;

        // "Is there something under this subject to open?" — see Registration::Exists.
        using Presence = std::function<bool( const SubjectId& )>;

        // WHAT ONE LINE OF REGISTRATION SAYS. Everything the editor needs to know about a kind of subject
        // that is not the document itself: what to call it, what to draw beside it, how to build one, and
        // how to tell whether there is one there to build.
        //
        // GROUPED, and that is not tidiness. The name and the icon used to live in two hand-written tables
        // in EditorLayer.cpp keyed on Assets::AssetTypeID — one `switch` for the icon and one call to
        // AssetTypeName for the text — so adding a kind of document meant three edits in three files and
        // the two that were not the registration are the ones that got forgotten. All four fields are
        // REQUIRED and an incomplete registration is refused by name: a half-registered kind is the stub
        // this project's contract forbids, and it would surface as a wrong window rather than an error.
        struct Registration
        {
            // What a log, a refusal census and the control channel call this kind of subject — "Material",
            // "AnimationComponent". Required rather than derived: a component facet is a digest of a type
            // name and cannot be turned back into one, and a census that could only print numbers is a
            // census a reader has to go and decode.
            std::string TypeName;

            // The glyph the document well, the Documents menu and the window title draw. A plain UTF-8
            // string (the ICON_MDI_* literals), so this header stays free of ImGui.
            const char* Icon = nullptr;

            // Builds the document window for one subject. Given only the subject: everything else the
            // editor needs (the asset manager, the scene, the shape of the window) belongs to whatever
            // registers this, captured there once instead of threaded through the call.
            //
            // This is the REACH half of what a subject is (EditorSubject.hpp): the identity is data anyone
            // can hold, and turning it back into typed authored data is knowledge only the registrant has.
            Factory Make;

            // IS THERE SOMETHING HERE TO OPEN, RIGHT NOW? Asked about subjects that are NOT open — "does
            // entity 12 have an AnimationComponent?" — which is why it cannot be asked of a document: no
            // document exists yet, and building one to find out would construct a node-editor context per
            // entity per keystroke.
            //
            // NOT THE SAME QUESTION AS ISubjectDocument::IsSubjectAlive, and the difference is load-bearing
            // rather than a nuance. This one is answered in the CURRENT context — the active scene, the
            // asset manager as it stands — because that is what "what can I open?" means. A document
            // answers about the context IT captured: an anim graph opened over the second scene view stays
            // bound to that scene, so the registry's answer about the active scene would be about a
            // different registry entirely and would close a window that is perfectly alive. Two questions,
            // two askers, and neither can stand in for the other.
            Presence Exists;
        };

        // Registering a second editor for a type REPLACES the first and says so: two editors for one kind
        // is a programming error, and the silent winner would be whichever registration ran last.
        //
        // A SECOND NAME UNDER ONE KEY IS AN ERROR, NOT A REPLACEMENT, and that check is what makes the
        // component facet safe to derive from a hash. Two component type names whose 32-bit digests collide
        // would otherwise share one editor silently, and the symptom would be a Details button opening
        // somebody else's window. Here it is a startup error naming both types.
        void Register( SubjectTypeKey type, Registration editor );

        [[nodiscard]] bool HasEditorFor( SubjectTypeKey type ) const noexcept;

        // What this kind of subject is called. "unregistered subject type" for one nothing claims — a
        // string a caller can print, because every caller of this is building a message.
        [[nodiscard]] std::string TypeName( SubjectTypeKey type ) const;

        [[nodiscard]] std::string TypeName( const SubjectId& subject ) const
        {
            return TypeName( subject.Type() );
        }

        // The glyph for this kind, or @p fallback when nothing is registered for it. The fallback is the
        // CALLER's, because the registry has no opinion about what an unknown thing looks like and a
        // hard-coded one here would be a second place icons come from.
        [[nodiscard]] const char* Icon( SubjectTypeKey type, const char* fallback ) const;

        [[nodiscard]] const char* Icon( const SubjectId& subject, const char* fallback ) const
        {
            return Icon( subject.Type(), fallback );
        }

        // Is there something under @p subject to open right now? FALSE for a kind nothing is registered
        // for, which is the honest answer to "can I open this?" and not a claim about the subject.
        [[nodiscard]] bool Exists( const SubjectId& subject ) const;

        // The document for @p subject, or null when no editor is registered for its type — logged with the
        // type's name, because "double-clicking it did nothing" is otherwise indistinguishable from a window
        // that failed to draw. Never returns a panel for a null subject.
        [[nodiscard]] std::unique_ptr<ISubjectDocument> Create( const SubjectId& subject ) const;

        // ── OPENING A FILE, WITHOUT A HAND-WRITTEN PARSE ───────────────────────────────────────────────
        //
        // A double-click in the asset browser has a PATH and needs a subject. Resolving one to the other is
        // per-format work — check the extension, find-or-create the asset, load it, resolve its
        // dependencies — so it cannot live in the browser, and it did not: the browser carried a chain of
        // `else if` on the file type, one arm per kind, and EditorLayer carried a second copy of the same
        // chain for `--open-panel`. A new kind of document meant editing both and remembering that they
        // exist, which is exactly the census nobody refills.
        //
        // So an opener is registered BESIDE the editor that consumes it. Each one answers whether the path
        // was its business; the browser asks the registry once and stops caring what formats exist.
        enum class PathOpenOutcome
        {
            NotMine,   // the string names nothing this opener recognises; nothing was logged, nothing wrong
            Failed,    // it WAS one of ours and it would not resolve — the opener logged why, with the path
            Requested, // queued; the window appears on the next frame
        };

        using PathOpener = std::function<PathOpenOutcome( const std::string& path )>;

        // AN OPENER SAYS WHICH EXTENSIONS IT ANSWERS FOR, and that is not bookkeeping — it is what makes
        // the project's openable files ENUMERABLE rather than merely openable.
        //
        // The palette has to offer one entry per openable asset, and it used to build that list from the
        // asset manager's cache: whatever the startup preloader had registered so far. Measured, that list
        // is 0 entries, then 106, then 130 as five separate startup stages fill it — so for 3.3 s of every
        // boot the palette successfully offered every material and no cloud asset at all. The fix is to
        // enumerate the FILES, which means something has to know which files are openable, and the only
        // honest owner of that fact is the opener itself. A list kept anywhere else is the second census
        // that falls behind — this registry exists because there used to be three of those.
        //
        // Dot included, lower case ({ ".demat" }): the filter compares against a lower-cased extension, so
        // a registration spelled ".DEMAT" would silently claim nothing. An EMPTY list is refused rather
        // than accepted as "claims nothing": an opener that answers for no extension can still be reached
        // through OpenPath and would therefore be openable-but-not-listed, which is exactly the
        // reachable-only-by-hand state the command palette exists to abolish.
        void RegisterPathOpener( std::vector<std::string> extensions, PathOpener opener );

        // Every extension any registered opener claims, deduplicated, for whoever is enumerating content.
        // The union and not a per-opener view: a caller asking "can this project's files be opened?" does
        // not care which opener would take them, and would only be able to get that wrong.
        [[nodiscard]] std::vector<std::string> ClaimedExtensions() const;

        // Consults the registered openers in registration order and stops at the first that claims the
        // path. NotMine when none does — which is a normal answer (a `.png` is not a document) and is why
        // it is not logged here.
        [[nodiscard]] PathOpenOutcome OpenPath( const std::string& path ) const;

        [[nodiscard]] std::size_t Size() const noexcept
        {
            return m_Editors.size();
        }

        // Every registered type. For the census below and for the "what can this editor open at all?"
        // question a refusal has to be able to answer.
        [[nodiscard]] std::vector<SubjectTypeKey> RegisteredTypes() const;

    private:
        // An opener and the formats it answers for, held together because they are one registration: two
        // containers would let a format be claimed by nothing, or listed with no opener behind it.
        struct RegisteredOpener
        {
            std::vector<std::string> Extensions;
            PathOpener               Open;
        };

        std::unordered_map<SubjectTypeKey, Registration> m_Editors;
        std::vector<RegisteredOpener>                    m_PathOpeners;
    };

    // ── THE RELATION, NOT A LIST ───────────────────────────────────────────────────────────────────────
    //
    // "The set of open documents is a subset of the set of subject types that have a registered editor."
    //
    // That sentence is the seam. Every way a document can come into existence goes through
    // SubjectEditorRegistry::Create, so it ought to hold by construction — and "ought to by construction"
    // is exactly the claim that stops being true the day somebody adds a second way in. It has happened in
    // this editor already: before the well existed, documents were built in three hand-wired places
    // (NodeGraphPanel::RequestOpen, MaterialPreviewPanel::RequestPreview, SceneOpenRequest), each of which
    // was a route the registry knew nothing about.
    //
    // Written as a COUNT of the exceptions rather than as a bool, so a failing suite says how many and the
    // editor can name them. A free function over the ranges, rather than a loop inside EditorLayer, for the
    // reason the slot counters next door are free functions: EditorLayer.cpp is compiled by no suite
    // (scripts/CI/UnreachedSources.sh), so a rule written there is a rule nothing can assert.
    struct DocumentEditorCensus
    {
        std::size_t Documents       = 0;
        std::size_t RegisteredTypes = 0;
        // Open documents whose subject type nothing is registered for — i.e. documents that came from
        // somewhere other than the registry. Zero, or the seam has a second door.
        std::size_t UnregisteredDocuments = 0;

        [[nodiscard]] bool EveryDocumentCameFromTheRegistry() const noexcept
        {
            return UnregisteredDocuments == 0;
        }
    };

    template <typename DocumentRange>
    [[nodiscard]] DocumentEditorCensus CensusOfDocumentEditors( const SubjectEditorRegistry& registry,
                                                                const DocumentRange&         documents )
    {
        DocumentEditorCensus census;
        census.RegisteredTypes = registry.Size();
        for ( const auto& document : documents )
        {
            if ( !document )
                continue;
            ++census.Documents;
            if ( !registry.HasEditorFor( document->Subject().Type() ) )
                ++census.UnregisteredDocuments;
        }
        return census;
    }

    // FindOpenAssetDocument USED TO LIVE HERE, and its removal is the point of the change that took it out.
    //
    // It searched a PANEL LIST for a document, by dynamic_cast, because documents were mixed in among the
    // tools and had to be sifted back out at every site that wanted one. That cast is gone from every such
    // site now: documents have their own owner, so "the already-open document for this subject" is
    // OpenDocuments::Find and there is nothing to sift. Keeping this function beside it would have left two
    // functions answering one question — the very thing the note above says the editor has already paid for
    // once — and the survivor would have been the one that could still be pointed at the wrong container.

    // How many renderer slots the open documents have SPOKEN FOR but not yet taken.
    //
    // The cap is checked as `live + pending >= kMaxRendererSlots`, and `live` counts renderers that exist.
    // A document is created before it first draws, and a Material Editor builds its PreviewViewport on that
    // first frame — so between the two it holds no slot and has a claim coming. Counting only live
    // renderers would admit a document there is no slot for and discover it a frame later, with the
    // symptom being two surfaces quietly trading each other's per-frame camera.
    //
    // A DOCUMENT THAT WILL NEVER CLAIM ONE IS NOT PENDING DEMAND, and that half is not symmetry for its own
    // sake: the four cloud documents bake on the CPU and upload an Image2D, so five of them open beside the
    // main viewport would reach the cap on paper and the sixth would be refused — with a census telling the
    // user to close windows that were holding nothing and would never hold anything. See
    // ISubjectDocument::ClaimsView.
    //
    // A free function over the range, rather than a loop inside EditorLayer, for the reason
    // FindOpenAssetDocument above is one: EditorLayer.cpp is compiled by no suite
    // (scripts/CI/UnreachedSources.sh), so a rule written there is a rule nothing can assert. Templated on
    // the range so a test can drive it with a plain vector and no editor anywhere near.
    template <typename Range>
    [[nodiscard]] uint32_t PendingRendererSlotDemand( const Range& panels )
    {
        uint32_t pending = 0;
        for ( const auto& panel : panels )
        {
            const auto* document = dynamic_cast<const ISubjectDocument*>( &*panel );
            if ( document && document->ClaimsView() && !document->HoldsView() )
                ++pending;
        }
        return pending;
    }
    // How many BYTES the open documents have SPOKEN FOR but not yet allocated: the forecast of every
    // document that will build a view (ClaimsView) and has not built it yet (!HoldsView).
    //
    // Why pending demand is counted at all: a document is created before it first draws, and a Material
    // Editor builds its PreviewViewport on that first frame — so between the two its memory is in no usage
    // figure the device reports, and a check that trusted usage alone would admit a document whose view
    // does not fit and discover it a frame later as an allocation failure. A view that EXISTS is not counted
    // here: its memory is already in the usage, and counting its forecast too would count it twice.
    //
    // A DOCUMENT THAT WILL NEVER BUILD A VIEW ADDS NOTHING: the four cloud documents bake on the CPU, and
    // counting them once refused a window with a census telling the user to close windows that held nothing.
    //
    // A free function over the range, for the reason FindOpenAssetDocument above is one: EditorLayer.cpp is
    // compiled by no suite, so a rule written there is a rule nothing can assert.
    template <typename Range>
    [[nodiscard]] uint64_t PendingViewBytes( const Range& panels )
    {
        uint64_t pending = 0;
        for ( const auto& panel : panels )
        {
            const auto* document = dynamic_cast<const ISubjectDocument*>( &*panel );
            if ( document && document->ClaimsView() && !document->HoldsView() )
                pending += document->ViewForecastBytes();
        }
        return pending;
    }

    // May @p document be opened beside documents that have @p pendingBytes still to allocate?
    //
    // NOT A SECOND BUDGET RULE: this is Engine::ViewBudget::MayCreate for a user surface (the person asked
    // for this window by name, so it keeps no reserve), asked for the new document's forecast PLUS the
    // pending demand — memory that is spoken for but not yet in the reading's usage. A document that builds
    // no view asks for its share of the pending demand only, so a CPU-drawn window is refused only when the
    // documents already open cannot fit either.
    [[nodiscard]] inline Engine::ViewBudget::Verdict
    AdmitDocumentView( const ISubjectDocument& document, const uint64_t pendingBytes,
                       const Engine::ViewBudget::Reading& reading )
    {
        const uint64_t own = document.ClaimsView() && !document.HoldsView() ? document.ViewForecastBytes() : 0;
        return Engine::ViewBudget::MayCreate( Engine::ViewBudget::Demand::UserSurface, own + pendingBytes, 0,
                                              reading );
    }
} // namespace Desert::Editor
