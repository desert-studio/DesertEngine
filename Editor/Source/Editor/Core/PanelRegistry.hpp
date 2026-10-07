#pragma once

#include <Editor/Panels/IPanel.hpp>

#include <memory>
#include <type_traits>
#include <vector>

namespace Desert::Editor
{
    // THE TOOLS, AND ONLY THE TOOLS.
    //
    // A tool panel and a document have opposite lifetimes. A tool is constructed once at startup and
    // lives until the editor exits; its visibility is a SETTING the user keeps, and "hide" is the whole of
    // "close". A document is constructed for a subject, dies with it, and its existence is not a setting at
    // all — destroying it is what returns the Scene, the SceneRenderer and one of the six renderer slots.
    //
    // They used to share one vector and one visibility bool, and that is what produced the defect: unticking
    // a document in the View menu set GetVisibility() false, CloseDismissedAssetDocuments destroyed the
    // object on the next frame, and re-ticking could not bring it back because there was nothing left to
    // tick. The menu was not wrong; the container was. One flag serving two lifetimes forces "hide" for a
    // tool to mean "destroy" for a document.
    //
    // Hence a container that CANNOT HOLD A DOCUMENT. The View menu, the command palette and `--open-panel`
    // are all loops over this registry, so none of them can list a document — not because each remembers to
    // skip one, but because there is none there to skip. A predicate would have put the same rule in three
    // places and obliged every fourth place to learn it; this puts it in the type, where the compiler
    // enforces it and a new call site inherits it for free. Documents live in OpenDocuments, next door.
    //
    // The registry owns and hands out non-owning access; the editor still drives every panel through IPanel.
    class PanelRegistry
    {
    public:
        // Whether this registry will accept @p Panel at all. Exposed as a trait rather than hidden inside
        // the static_assert below so a TEST can state the rule as an expression — "a document is not a
        // tool" is the whole point of this class, and a rule only the compiler can see is a rule no suite
        // can show going red.
        template <typename Panel>
        static constexpr bool Accepts =
             std::is_base_of_v<IPanel, Panel> && !std::is_base_of_v<ISubjectDocument, Panel> &&
             !std::is_same_v<Panel, IPanel>;

        // Constructs a tool panel in place and returns it. The concrete type is a template parameter and not
        // an incidental one: it is what lets the check below run at compile time.
        template <typename Panel, typename... Args>
        Panel& Add( Args&&... args )
        {
            static_assert( Accepts<Panel>,
                           "A document is not a tool. An ISubjectDocument belongs to OpenDocuments: putting "
                           "one here would put it back in the View menu, the command palette and "
                           "--open-panel, where unticking it destroys it." );

            auto   owned = std::make_unique<Panel>( std::forward<Args>( args )... );
            Panel& ref   = *owned;
            m_Panels.emplace_back( std::move( owned ) );
            JoinTheTree( ref );
            return ref;
        }

        // Takes over a panel that had to be built before it could be handed over (the viewports and the
        // asset browser are configured by their creator first).
        //
        // The CONCRETE type is required, and `IPanel` is rejected by name: a unique_ptr<IPanel> would
        // deduce Panel = IPanel, and IPanel is not derived from ISubjectDocument, so the check above would
        // pass while the pointer underneath was a document. That is the exact hole a runtime predicate
        // would leave open, so it is closed here instead of asserted about later.
        template <typename Panel>
        Panel& Adopt( std::unique_ptr<Panel> panel )
        {
            static_assert( Accepts<Panel>,
                           "A document is not a tool, and a unique_ptr<IPanel> hides which one this is — "
                           "hand over the concrete panel type. Documents belong to OpenDocuments." );

            Panel& ref = *panel;
            m_Panels.emplace_back( std::move( panel ) );
            JoinTheTree( ref );
            return ref;
        }

        // Drops @p panel if it is here. The editor calls this when a scene view is torn down.
        void Remove( const IPanel* panel )
        {
            std::erase_if( m_Panels,
                           [panel]( const std::unique_ptr<IPanel>& held ) { return held.get() == panel; } );
        }

        void Clear()
        {
            m_Panels.clear();
        }

        [[nodiscard]] std::size_t Size() const noexcept
        {
            return m_Panels.size();
        }

        [[nodiscard]] auto begin() noexcept
        {
            return m_Panels.begin();
        }
        [[nodiscard]] auto end() noexcept
        {
            return m_Panels.end();
        }
        [[nodiscard]] auto begin() const noexcept
        {
            return m_Panels.begin();
        }
        [[nodiscard]] auto end() const noexcept
        {
            return m_Panels.end();
        }

        void JoinEvents( Common::EventTree& events, Common::EventNodeId parent )
        {
            m_Events      = &events;
            m_EventParent = parent;
        }

    private:
        template <typename Panel>
        void JoinTheTree( Panel& panel )
        {
            if ( m_Events != nullptr )
                panel.JoinEvents(
                     Common::EventNodeLink( *m_Events, m_Events->Attach<Panel>( m_EventParent, panel ) ) );
        }

        std::vector<std::unique_ptr<IPanel>> m_Panels;
        Common::EventTree*                   m_Events = nullptr;
        Common::EventNodeId                  m_EventParent{};
    };
} // namespace Desert::Editor
