#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Editor/Core/Control/ControlPipeline.hpp>
#include <Editor/Core/EditableProperty.hpp>

#include <rflcpp/rfl/Generic.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Editor::Control
{
    /**
     * @brief WHAT THE EDITOR LOOKS LIKE, as plain values, and its rendering as JSON.
     *
     * WHY A SNAPSHOT STRUCT AND NOT "SERIALISE IT WHERE IT LIVES". Every fact below is held somewhere in
     * EditorLayer, and EditorLayer.cpp is compiled by NO test suite at all (scripts/CI/UnreachedSources.sh).
     * A JSON writer over live editor state would be a writer nothing could ever show going red — and this
     * is the half of the channel a client REASONS about: a number here disagreeing with the picture beside
     * it is worse than no number, because a report quotes it.
     *
     * So EditorLayer does the one thing only it can do — read its own members — and everything after that
     * is pure and assertable: the section filter, the shapes, the names of the fields.
     *
     * THE SNAPSHOT IS TAKEN ALL AT ONCE, which is the other reason it is a value. A client asking for the
     * documents and the slot census in one request must get two halves of ONE instant; gathered lazily per
     * section they could straddle a close, and the answer would be a state the editor was never in.
     */

    struct DocumentSnapshot
    {
        std::string Name;    ///< the display half, as a person reads it on the tab
        std::string Type;    ///< the asset type's name
        std::string Subject; ///< SubjectId::ToString() — "asset:2:333" / "component:17293:88"
        bool        HoldsRendererSlot  = false;
        bool        ClaimsRendererSlot = false;
        bool        Focused            = false;

        // ── WHERE THIS DOCUMENT'S EDITS HAVE REACHED ───────────────────────────────────────────────────
        //
        // Reported because a PICTURE OF THE EDITOR AND A NUMBER BESIDE IT MUST SAY THE SAME THING. The
        // claim this channel is most often asked to photograph is "the preview moved and the scene did
        // not", and a capture alone cannot distinguish that from a scene that happens to be out of frame.
        // These three fields are what turn the picture into evidence.
        //
        // Strings and not the enums themselves: this struct is the wire's vocabulary, and a client reading
        // "staged" needs no header of ours to know what it got. See ISubjectDocument for the states.
        std::string EditModel         = "write-through"; ///< "write-through" | "staged"
        bool        HasUnappliedEdits = false;           ///< working differs from what the scenes are rendering
        std::string DiskState         = "untracked";     ///< "clean" | "dirty" | "untracked"
    };

    struct ClosedDocumentSnapshot
    {
        std::string Name;
        std::string Type;
        std::string Subject;
    };

    struct PanelSnapshot
    {
        std::string Name;
        bool        Visible    = false;
        bool        Pinned     = false;
        bool        Contextual = false;
        bool        Relevant   = true;
    };

    struct EntitySnapshot
    {
        std::string Tag;
        std::string Uuid;
    };

    /// WHAT IS BEING AUTHORED ON THE SELECTED RIG, AND BY WHOM (07 §14.2 / Core::AuthoringContext).
    ///
    /// On the wire because the viewport's four modes are drawn as ImGui overlays — bone octahedra,
    /// control shapes — so a window capture is the ONLY picture of them, and a picture of an overlay
    /// cannot say which mode produced it when two modes draw bones. These fields are what let a report
    /// put the number beside the frame; `holder` is here for the same reason the refusals name an owner.
    struct AuthoringSnapshot
    {
        std::string Mode   = "Object"; ///< AuthoringModeName: "Object" | "Skeleton" | "Pose" | "Control"
        std::string Entity;            ///< the character the mode is about; empty when nothing is published
        std::string Holder = "nobody"; ///< AuthoringOwner::Describe()
        int         SelectedBone    = -1; ///< index into Skeleton::GetBones(), -1 for none
        int         SelectedControl = -1; ///< index into the live ControlHierarchy, -1 for none
        bool        ShowBoneNames   = false;
        bool        PreviewsBindPose = false; ///< Skeleton only — 07 §1.3; the scene render shows it
    };

    /// The whole picture, gathered in one pass. Every list is in the order the editor itself shows it —
    /// the documents most-recently-used first, exactly as Ctrl+Tab walks them — so a client reading this
    /// and a person reading the screen are reading the same sequence.
    struct EditorSnapshot
    {
        std::string SceneName;
        bool        SceneHasUnsavedChanges = false;
        bool        InPlayMode             = false;

        std::vector<EntitySnapshot> Selection; ///< last element is the primary selection

        std::vector<DocumentSnapshot>       Documents;      ///< most recently used first
        std::vector<ClosedDocumentSnapshot> RecentlyClosed; ///< newest first
        std::vector<PanelSnapshot>          Panels;         ///< tools only; a document is never here

        uint32_t RendererSlotsLive    = 0;
        uint32_t RendererSlotsPending = 0;
        uint32_t RendererSlotsMax     = 0;

        std::size_t              LogInfoCount    = 0;
        std::size_t              LogWarningCount = 0;
        std::size_t              LogErrorCount   = 0;
        std::vector<std::string> LogTail; ///< oldest first

        AuthoringSnapshot Authoring;

        EditorQuiescence Quiescence;
    };

    /// The sections a client may ask for. A table, so the refusal that lists them is built from the set
    /// actually honoured — the same rule the command line follows for its flags, and for the same reason:
    /// a list written out by hand drifts, and the drift shows up as a section that silently returns nothing.
    inline constexpr const char* kStateSections[] = {
         "scene", "selection", "authoring", "documents", "panels", "renderer_slots", "log", "quiescence",
    };

    [[nodiscard]] inline std::string KnownSectionList()
    {
        std::string list;
        for ( const char* section : kStateSections )
        {
            if ( !list.empty() )
                list += ", ";
            list += section;
        }
        return list;
    }

    /// Refuse a section nobody serves, naming the ones served. A section quietly missing from the answer
    /// would read as "the editor has none of those" — an empty document list is what a client sees either
    /// way, and one of those two readings is a lie.
    [[nodiscard]] inline Common::BoolResultStr ValidateSections( const std::vector<std::string>& sections )
    {
        for ( const std::string& wanted : sections )
        {
            bool known = false;
            for ( const char* section : kStateSections )
            {
                if ( wanted == section )
                {
                    known = true;
                    break;
                }
            }

            if ( !known )
            {
                return Common::MakeFormattedError<bool>(
                     "'{}' is not a section of the editor's state. Asking for one that does not exist would "
                     "come back empty, which reads exactly like a section that exists and is empty. Known "
                     "sections: {}",
                     wanted, KnownSectionList() );
            }
        }
        return Common::MakeSuccess( true );
    }

    namespace StateDetail
    {
        /// Was @p section asked for? An EMPTY request means all of them — a client that wants everything
        /// should not have to enumerate what "everything" is today.
        [[nodiscard]] inline bool Wanted( const std::vector<std::string>& sections, const char* section )
        {
            if ( sections.empty() )
                return true;
            for ( const std::string& asked : sections )
                if ( asked == section )
                    return true;
            return false;
        }

        [[nodiscard]] inline rfl::Generic Str( const std::string& text )
        {
            return rfl::Generic( text );
        }

        [[nodiscard]] inline rfl::Generic Num( double value )
        {
            return rfl::Generic( value );
        }
    } // namespace StateDetail

    /// The snapshot as JSON, restricted to @p sections (empty = all). Callers validate the sections
    /// first; anything unknown here is simply absent, because the refusal already happened.
    [[nodiscard]] inline rfl::Generic::Object ToJson( const EditorSnapshot&           snapshot,
                                                      const std::vector<std::string>& sections )
    {
        using namespace StateDetail;
        rfl::Generic::Object root;

        if ( Wanted( sections, "scene" ) )
        {
            rfl::Generic::Object scene;
            scene["name"]     = Str( snapshot.SceneName );
            scene["modified"] = rfl::Generic( snapshot.SceneHasUnsavedChanges );
            scene["playing"]  = rfl::Generic( snapshot.InPlayMode );
            root["scene"]     = rfl::Generic( scene );
        }

        if ( Wanted( sections, "selection" ) )
        {
            rfl::Generic::Array selection;
            for ( const EntitySnapshot& entity : snapshot.Selection )
            {
                rfl::Generic::Object item;
                item["tag"]  = Str( entity.Tag );
                item["uuid"] = Str( entity.Uuid );
                selection.push_back( rfl::Generic( item ) );
            }
            root["selection"] = rfl::Generic( selection );
        }

        if ( Wanted( sections, "authoring" ) )
        {
            rfl::Generic::Object authoring;
            authoring["mode"]             = Str( snapshot.Authoring.Mode );
            authoring["entity"]           = Str( snapshot.Authoring.Entity );
            authoring["holder"]           = Str( snapshot.Authoring.Holder );
            authoring["selectedBone"]     = Num( snapshot.Authoring.SelectedBone );
            authoring["selectedControl"]  = Num( snapshot.Authoring.SelectedControl );
            authoring["showBoneNames"]    = rfl::Generic( snapshot.Authoring.ShowBoneNames );
            authoring["previewsBindPose"] = rfl::Generic( snapshot.Authoring.PreviewsBindPose );
            root["authoring"]             = rfl::Generic( authoring );
        }

        if ( Wanted( sections, "documents" ) )
        {
            rfl::Generic::Array open;
            for ( const DocumentSnapshot& document : snapshot.Documents )
            {
                rfl::Generic::Object item;
                item["name"]       = Str( document.Name );
                item["type"]       = Str( document.Type );
                item["subject"]    = Str( document.Subject );
                item["holdsSlot"]  = rfl::Generic( document.HoldsRendererSlot );
                item["claimsSlot"] = rfl::Generic( document.ClaimsRendererSlot );
                item["focused"]    = rfl::Generic( document.Focused );
                // The three states, so a client can say in numbers what a capture shows in pixels.
                item["editModel"] = Str( document.EditModel );
                item["unapplied"] = rfl::Generic( document.HasUnappliedEdits );
                item["disk"]      = Str( document.DiskState );
                open.push_back( rfl::Generic( item ) );
            }

            rfl::Generic::Array closed;
            for ( const ClosedDocumentSnapshot& document : snapshot.RecentlyClosed )
            {
                rfl::Generic::Object item;
                item["name"]    = Str( document.Name );
                item["type"]    = Str( document.Type );
                item["subject"] = Str( document.Subject );
                closed.push_back( rfl::Generic( item ) );
            }

            rfl::Generic::Object documents;
            documents["open"] = rfl::Generic( open );
            // The list the empty document well offers back, newest first. Named on the wire because it is
            // the one piece of document state that outlives the window it describes.
            documents["recentlyClosed"] = rfl::Generic( closed );
            root["documents"]           = rfl::Generic( documents );
        }

        if ( Wanted( sections, "panels" ) )
        {
            rfl::Generic::Array panels;
            for ( const PanelSnapshot& panel : snapshot.Panels )
            {
                rfl::Generic::Object item;
                item["name"]       = Str( panel.Name );
                item["visible"]    = rfl::Generic( panel.Visible );
                item["pinned"]     = rfl::Generic( panel.Pinned );
                item["contextual"] = rfl::Generic( panel.Contextual );
                item["relevant"]   = rfl::Generic( panel.Relevant );
                panels.push_back( rfl::Generic( item ) );
            }
            root["panels"] = rfl::Generic( panels );
        }

        if ( Wanted( sections, "renderer_slots" ) )
        {
            rfl::Generic::Object slots;
            slots["live"]          = Num( snapshot.RendererSlotsLive );
            slots["pending"]       = Num( snapshot.RendererSlotsPending );
            slots["max"]           = Num( snapshot.RendererSlotsMax );
            root["renderer_slots"] = rfl::Generic( slots );
        }

        if ( Wanted( sections, "log" ) )
        {
            rfl::Generic::Array tail;
            for ( const std::string& line : snapshot.LogTail )
                tail.push_back( Str( line ) );

            rfl::Generic::Object log;
            log["info"]    = Num( static_cast<double>( snapshot.LogInfoCount ) );
            log["warning"] = Num( static_cast<double>( snapshot.LogWarningCount ) );
            log["error"]   = Num( static_cast<double>( snapshot.LogErrorCount ) );
            log["tail"]    = rfl::Generic( tail );
            root["log"]    = rfl::Generic( log );
        }

        if ( Wanted( sections, "quiescence" ) )
        {
            rfl::Generic::Object quiescence;
            quiescence["settled"] = rfl::Generic( snapshot.Quiescence.Settled() );
            // What is outstanding, in the same words a settle timeout uses. One vocabulary, so a client
            // that read "asset documents are waiting to be opened" here recognises it in a refusal.
            quiescence["outstanding"] = Str( snapshot.Quiescence.Describe() );
            root["quiescence"]        = rfl::Generic( quiescence );
        }

        return root;
    }

    /**
     * @brief The focused document's property census as JSON — the answer to `properties`.
     *
     * Lives beside the state writer and not beside the census itself for the reason that whole file was
     * split this way: MaterialEditorPanel derives the values (it is the only thing that can read a loaded
     * shader), and everything after that is pure and assertable. Nothing here knows what a material is.
     *
     * EVERY FIELD OF EditableProperty IS WRITTEN, including the ones that say a property CANNOT be set. A
     * census that quietly listed only the writable rows would tell a client that a texture slot is not
     * declared by the shader, which is a different fact with a different fix.
     */
    [[nodiscard]] inline rfl::Generic::Object PropertiesToJson( const std::string&                   subject,
                                                                const std::vector<EditableProperty>& properties )
    {
        using namespace StateDetail;

        rfl::Generic::Array entries;
        entries.reserve( properties.size() );

        for ( const EditableProperty& property : properties )
        {
            rfl::Generic::Object item;
            item["name"]       = Str( property.Name );
            item["label"]      = Str( property.Label );
            item["type"]       = Str( property.Type );
            item["components"] = Num( static_cast<double>( property.Components ) );

            // ABSENT rather than null when the declaration states no clamp: a client that reads `min` as a
            // number cannot be handed a null, and "no range" is exactly the absence of the field.
            if ( property.Min.has_value() )
                item["min"] = Num( *property.Min );
            if ( property.Max.has_value() )
                item["max"] = Num( *property.Max );

            rfl::Generic::Array value;
            for ( int i = 0; i < property.Components; ++i )
                value.push_back( Num( property.Value[static_cast<std::size_t>( i )] ) );
            item["value"] = rfl::Generic( value );

            // THE GROUP THE WINDOW DRAWS THIS ROW UNDER. Present even when empty, and that is deliberate:
            // "" is the shader declaring no Category, which the window shows under its own heading, so the
            // field carries a fact either way. Omitting it when empty would make "this param has no
            // category" indistinguishable from "this editor is too old to report groups" — the §1.4 shape,
            // one field down.
            item["group"] = Str( property.Group );

            // WHEN A WRITE TO THIS ROW BECOMES VISIBLE — "Immediate", "Rebake", or absent when the schema
            // makes no claim. OMITTED rather than sent empty, which is the opposite choice from `group`
            // above and made for the same reason: an empty group is a FACT about the shader (it declares
            // none), whereas an empty timing is the absence of a claim, and a client must not be able to
            // read "" as "immediate".
            //
            // It is on the wire because the channel's contract is that a reply is released only after a
            // frame that shows the command's effect — and for a `Rebake` property that frame is SECONDS
            // away, behind a CPU precomputation (the cloud volume: 3.3 to 14.1 s). A client that does not
            // know which kind it just wrote reads the unchanged frame as a failed write, which is exactly
            // the conclusion the owner reached by hand, twice.
            if ( !property.Timing.empty() )
                item["timing"] = Str( property.Timing );

            item["settable"] = rfl::Generic( property.Settable );
            if ( !property.Settable )
                item["why"] = Str( property.NotSettableReason );
            if ( property.OverridesParent )
                item["overridesParent"] = rfl::Generic( true );

            entries.push_back( rfl::Generic( item ) );
        }

        rfl::Generic::Object payload;
        // NAMED "subject" AND NOT "document", and the rename is not cosmetic. The census answers for two
        // kinds of thing now — the focused document, and the editor's own view — so a field called
        // `document` carrying the word "viewport" would be a label asserting something the value denies,
        // which is a shape this project has found nine times and stopped tolerating.
        //
        // Named at all, because BOTH subjects move: the focus changes, and so does whether the editor's
        // camera is the one driving. A client that asked for properties and then set one has to be able to
        // see WHICH thing answered, or a change between the two requests is invisible in both replies.
        payload["subject"]    = Str( subject );
        payload["properties"] = rfl::Generic( entries );
        return payload;
    }
} // namespace Desert::Editor::Control
