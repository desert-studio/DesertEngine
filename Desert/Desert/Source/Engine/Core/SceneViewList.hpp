#pragma once

#include <memory>
#include <optional>
#include <vector>

namespace Desert::Core
{
    // THE ANGLES A WORLD IS BEING LOOKED AT FROM, in the order they were opened.
    //
    // WHAT THIS REPLACES, AND WHY IT IS A LIST. A Scene used to hold ONE `SceneRenderer*` and ONE
    // camera, which made "the same world from two angles" inexpressible: the only way to get a second
    // viewport was a second Scene, which is a second DOCUMENT — two independent copies of the same file
    // that drift apart the moment either is edited. The editor's "New Scene View" did exactly that, and
    // the outliner reporting "0 entities" in the new window is what that looks like from the outside.
    //
    // A VIEW IS (RENDERER, CAMERA) AND NOTHING ELSE. The renderer says WHERE the pixels go, the camera
    // says FROM WHERE. Everything else about a view — its window, its title, its gizmo mode — belongs to
    // the editor, and none of it is here, which is what keeps this compilable without a Vulkan device.
    //
    // WHY IT IS A TEMPLATE AND NOT A PLAIN CLASS OVER Graphic::SceneRenderer. The rules below are
    // pointer identity and ordering; they do not read one byte of either type. Making them a template is
    // what lets a suite drive the REAL rules with stand-in types — Scene.cpp needs a VkDevice and is one
    // of the translation units no test compiles (scripts/CI/UnreachedSources.sh), so a copy of these
    // rules inside a test would be a copy that can drift from the rules it claims to check.
    //
    // THE LIST MAY BE EMPTY, and that is a state, not a failure — see Scene::OnUpdate. A world with no
    // view still SIMULATES (physics, scripts, animation all run off the ECS pass, which happens once and
    // before any view is touched); it just has nowhere to put a picture. Making zero legal is also what
    // removed an uninitialised raw `SceneRenderer* m_SceneRenderer` from Scene: the default constructor
    // never assigned it, so a default-constructed Scene carried whatever byte its allocation landed on,
    // and "no renderer" and "a garbage renderer" were the same value.
    template <typename RendererT, typename CameraT>
    class SceneViewList
    {
    public:
        struct View
        {
            // Non-owning: the renderer is owned by whoever opened the view (the editor's viewport, a
            // preview panel), and it is in this list for exactly as long as that owner keeps it.
            RendererT* Renderer = nullptr;
            // Owning, and per-view rather than per-scene. This is the half that was on the SCENE before
            // and is the reason two viewports could not look from two places: one `m_MainCamera` means
            // one angle no matter how many renderers exist.
            std::shared_ptr<CameraT> Camera;
        };

        // Appends a view and returns its index.
        //
        // REFUSES a null renderer and a renderer that is already in the list, and says so by returning
        // nothing rather than by silently succeeding. A duplicate is the dangerous one: the same
        // renderer twice means the frame loop below would record two passes into one set of per-frame
        // GPU state, which is the (frame x slot) rule broken from inside the engine
        // (Docs/RENDERER_FRAME_STATE.md) and shows up as a torn picture with nothing in the log.
        [[nodiscard]] std::optional<size_t> Add( RendererT* renderer, std::shared_ptr<CameraT> camera )
        {
            if ( renderer == nullptr || IndexOf( renderer ).has_value() )
                return std::nullopt;

            m_Views.push_back( View{ renderer, std::move( camera ) } );
            return m_Views.size() - 1;
        }

        // Drops the view recording through @p renderer. False for a renderer this list never had, so a
        // double close is readable instead of being an assert or a no-op that looks like a success.
        //
        // The surviving views KEEP THEIR RELATIVE ORDER (this is an erase, not a swap-and-pop): index 0
        // is "the first view still open", and the play-state camera rule in Scene reads exactly that one.
        // A swap would silently move a different view into the position that rule points at.
        bool Remove( const RendererT* renderer )
        {
            const auto index = IndexOf( renderer );
            if ( !index )
                return false;

            m_Views.erase( m_Views.begin() + static_cast<std::ptrdiff_t>( *index ) );
            return true;
        }

        [[nodiscard]] std::optional<size_t> IndexOf( const RendererT* renderer ) const
        {
            for ( size_t i = 0; i < m_Views.size(); ++i )
            {
                if ( m_Views[i].Renderer == renderer )
                    return i;
            }
            return std::nullopt;
        }

        [[nodiscard]] size_t Count() const noexcept
        {
            return m_Views.size();
        }

        [[nodiscard]] bool Empty() const noexcept
        {
            return m_Views.empty();
        }

        // Null for an index nobody opened. Returned as a pointer rather than a reference precisely so
        // that "there is no such view" is a value the caller has to look at: a viewport whose view was
        // closed under it asks this every frame.
        [[nodiscard]] View* At( size_t index ) noexcept
        {
            return index < m_Views.size() ? &m_Views[index] : nullptr;
        }

        [[nodiscard]] const View* At( size_t index ) const noexcept
        {
            return index < m_Views.size() ? &m_Views[index] : nullptr;
        }

        [[nodiscard]] const std::vector<View>& All() const noexcept
        {
            return m_Views;
        }

        [[nodiscard]] std::vector<View>& All() noexcept
        {
            return m_Views;
        }

    private:
        std::vector<View> m_Views;
    };
} // namespace Desert::Core
