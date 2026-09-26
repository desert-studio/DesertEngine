#pragma once

#include <Editor/Core/CommandPalette.hpp>
#include <Common/Core/ResultStr.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Desert::Editor
{
    /**
     * @brief WHAT THE DETAILS PANEL CAN BE ASKED TO SHOW, AND THE ONE-SHOT REQUESTS TO SHOW IT. Pure: no
     *        ImGui, no ECS, so a suite drives it with strings.
     *
     * ── WHY THE LIST IS WHAT WAS DRAWN, NOT WHAT THE COMPONENTS DECLARE ─────────────────────────────
     *
     * A client on the control channel could reach every panel but the parts of Details that only a mouse
     * reaches: a field below the fold and an asset picker's dropdown. The commands that reach them must be
     * offered exactly when they can work, and "can work" is a fact about the frame — a field inside a
     * collapsed section, a slot the material widget did not draw, a picker the search box filtered out
     * cannot be scrolled to or opened. So the vocabulary is the ledger of the last COMPLETE Details frame:
     * every site that draws a field or a picker notes it as it draws, and the commands are built from that.
     * A declared list would be a second source of truth that disagrees the first time a widget changes.
     *
     * A section header is itself a field ("Show field: Skybox"), and revealing it opens it, so a field in
     * a collapsed section is two commands away rather than unreachable.
     *
     * ── WHY REQUESTS EXPIRE ─────────────────────────────────────────────────────────────────────────
     *
     * A command runs whenever the channel delivers it, possibly after Details drew this frame, so a
     * request must survive to the next draw. It must not survive longer: a request the next draw did not
     * take (the selection changed, the section closed) would otherwise fire on some later frame nobody
     * asked about. It lives through exactly one BeginFrame and is dropped at the second.
     */
    class DetailsNavigation
    {
    public:
        // Called once per Details draw before any component. `subject` is the selected entity's UUID,
        // 0 for "nothing selected"; `subjectName` names it in refusals.
        void BeginFrame( std::uint64_t subject, std::string subjectName )
        {
            m_ShownFields  = m_Subject == subject ? std::move( m_DrawingFields ) : std::vector<std::string>{};
            m_ShownPickers = m_Subject == subject ? std::move( m_DrawingPickers ) : std::vector<std::string>{};
            m_DrawingFields.clear();
            m_DrawingPickers.clear();
            m_Component.clear();
            if ( m_Subject != subject )
                m_Pending = {};
            m_Subject     = subject;
            m_SubjectName = std::move( subjectName );
            if ( m_Pending.What != Kind::None && ++m_Pending.Frames > 1 )
                m_Pending = {};
            m_Drawing = true;
        }

        // After the last component. Other panels draw reflected fields through the same builder, and what
        // they draw must neither enter this ledger nor consume a Details request.
        void EndFrame()
        {
            m_Drawing = false;
            m_Component.clear();
        }

        // A component section header; the fields noted after it are qualified by its name.
        void NoteComponent( std::string_view component )
        {
            if ( !m_Drawing )
                return;
            m_Component = std::string( component );
            Add( m_DrawingFields, m_Component );
        }

        void NoteField( std::string_view field )
        {
            if ( !m_Drawing )
                return;
            Add( m_DrawingFields, m_Component.empty() ? std::string( field )
                                                      : m_Component + " / " + std::string( field ) );
        }

        // The label a field noted now is offered under (the same qualification NoteField applies).
        [[nodiscard]] std::string FieldLabel( std::string_view field ) const
        {
            return m_Component.empty() ? std::string( field ) : m_Component + " / " + std::string( field );
        }

        void NotePicker( std::string_view picker )
        {
            if ( !m_Drawing )
                return;
            Add( m_DrawingPickers, std::string( picker ) );
        }

        [[nodiscard]] const std::vector<std::string>& Fields() const
        {
            return m_ShownFields;
        }

        [[nodiscard]] const std::vector<std::string>& Pickers() const
        {
            return m_ShownPickers;
        }

        [[nodiscard]] Common::BoolResultStr RequestReveal( std::string_view field )
        {
            return Request( Kind::Reveal, field, m_ShownFields, "Show field", "field" );
        }

        [[nodiscard]] Common::BoolResultStr RequestPicker( std::string_view picker )
        {
            return Request( Kind::Picker, picker, m_ShownPickers, "Open picker", "picker" );
        }

        // One-shot: true once, at the draw site of the requested field / picker.
        [[nodiscard]] bool TakeReveal( std::string_view field )
        {
            return Take( Kind::Reveal, field );
        }

        // A picker opens in TWO frames. The first scrolls its row into view; the second opens the popup
        // under the row where the scroll left it. Opened in the first frame, the popup was placed at the
        // row's PRE-scroll position — below the Details window for any row that needed scrolling, which with
        // multi-viewports is a separate OS window outside the editor's, so no shot of the editor showed it.
        enum class PickerStep : std::uint8_t
        {
            None,
            Scroll,
            Open
        };

        [[nodiscard]] PickerStep TakePicker( std::string_view picker )
        {
            if ( !m_Drawing || m_Pending.What != Kind::Picker || m_Pending.Name != picker )
                return PickerStep::None;
            if ( !m_Pending.Scrolled )
            {
                // The row has to be drawn once more after the scroll lands: restart the expiry count.
                m_Pending.Scrolled = true;
                m_Pending.Frames   = 0;
                return PickerStep::Scroll;
            }
            m_Pending = {};
            return PickerStep::Open;
        }

    private:
        enum class Kind : std::uint8_t
        {
            None,
            Reveal,
            Picker
        };

        struct Pending
        {
            Kind        What = Kind::None;
            std::string Name;
            int         Frames   = 0;
            bool        Scrolled = false;
        };

        static void Add( std::vector<std::string>& list, std::string name )
        {
            // First occurrence wins: a nested struct repeating a field name must not offer it twice.
            if ( std::find( list.begin(), list.end(), name ) == list.end() )
                list.push_back( std::move( name ) );
        }

        Common::BoolResultStr Request( Kind kind, std::string_view name, const std::vector<std::string>& shown,
                                       const char* command, const char* what )
        {
            if ( m_Subject == 0 )
                return Common::MakeError( std::string( command ) + ": no entity is selected in the Details panel" );
            if ( std::find( shown.begin(), shown.end(), name ) == shown.end() )
            {
                std::string known;
                for ( const std::string& entry : shown )
                    known += ( known.empty() ? "" : ", " ) + entry;
                return Common::MakeError( std::string( command ) + ": the Details panel shows no " + what + " '" +
                                          std::string( name ) + "' for '" + m_SubjectName +
                                          "'; it shows: " + ( known.empty() ? "(none)" : known ) );
            }
            m_Pending = Pending{ kind, std::string( name ), 0 };
            return Common::MakeSuccess( true );
        }

        bool Take( Kind kind, std::string_view name )
        {
            if ( !m_Drawing || m_Pending.What != kind || m_Pending.Name != name )
                return false;
            m_Pending = {};
            return true;
        }

        bool                     m_Drawing = false;
        std::uint64_t            m_Subject = 0;
        std::string              m_SubjectName;
        std::string              m_Component;
        std::vector<std::string> m_DrawingFields;
        std::vector<std::string> m_DrawingPickers;
        std::vector<std::string> m_ShownFields;
        std::vector<std::string> m_ShownPickers;
        Pending                  m_Pending;
    };

    // The palette's "Details" group, built from what the last Details frame drew. Each entry re-checks at
    // run time, so a stale dictionary (selection changed since it was built) refuses with the reason.
    [[nodiscard]] inline std::vector<PaletteCommand> DetailsPaletteCommands( DetailsNavigation& navigation )
    {
        std::vector<PaletteCommand> commands;
        commands.reserve( navigation.Fields().size() + navigation.Pickers().size() );
        for ( const std::string& field : navigation.Fields() )
            commands.push_back( { "Details", "Show field: " + field,
                                  [&navigation, field] { return navigation.RequestReveal( field ); } } );
        for ( const std::string& picker : navigation.Pickers() )
            commands.push_back( { "Details", "Open picker: " + picker,
                                  [&navigation, picker] { return navigation.RequestPicker( picker ); } } );
        return commands;
    }

    // The editor's one instance, and the two ImGui-side calls every draw site makes (DetailsNavigation.cpp).
    DetailsNavigation& GetDetailsNavigation();

    // At a field's row, right after drawing it: notes the field; scrolls it into view when revealed.
    void MarkDetailsField( std::string_view field );

    // At a picker's open site: notes the picker; true once when a command asked to open it, and the next
    // popup is then placed under the last drawn item (not at the mouse, which a client never moved).
    [[nodiscard]] bool TakeDetailsPickerRequest( std::string_view picker );
} // namespace Desert::Editor
