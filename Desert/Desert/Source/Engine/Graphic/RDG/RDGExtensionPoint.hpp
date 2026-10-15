#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// WHERE A PASS FROM OUTSIDE THE FRAME BUILD JOINS THE FRAME (UE: ISceneViewExtension). The engine's own passes
// are placed by the order of the calls in SceneRenderer's frame-build functions (AddFrame*); nothing outside the
// engine can add a call there, so the frame build names the places it offers instead and, at each one, adds the
// passes registered for it - in registration order, as one explicit call (SceneRenderer::AddExtensionPoint). An
// editor tool, a plugin or an external graph picks a point by meaning, never by a number; adding a point is adding
// an enumerator and the one call that invokes it at its place.
//
// VULKAN-FREE ON PURPOSE, like the rest of Graphic/RDG: the registry is tested device-free (RenderGraphCompile).
namespace Desert::Graphic::RDG
{
    enum class ExtensionPoint : uint8_t
    {
        AfterOpaque,       // the opaque scene is lit and composited; before the height fog apply / translucency
        AfterTranslucency, // after the last translucent pass (particles), before the temporal resolve; the pass
                           // draws into the render-extent scene target, jittered like the scene under it
        Overlay,           // after the temporal resolve and the engine's debug lines: unjittered, output extent
        UI,                // after the overlays and the backdrop blur the UI samples, before the post chain
        // AFTER THE TONEMAP AND THE SPATIAL AA (UE: the EditorPrimitives composite, PostProcessing.cpp
        // EPass::EditorPrimitive): the editor-only primitives (grid, colliders, tool previews, debug lines) draw
        // into the view's editor-primitives target (EditorPrimitivesLayout: display-linear colour cleared
        // transparent, the scene depth populated into a multisampled depth), which one node then composites onto
        // the display image. Nothing drawn here reaches the exposure histogram, the bloom, a temporal history or
        // any other HDR pass; a pass registered here builds its pipeline against EditorPrimitivesLayout().
        EditorPrimitives,
    };

    [[nodiscard]] constexpr std::string_view ToString( const ExtensionPoint point )
    {
        switch ( point )
        {
            case ExtensionPoint::AfterOpaque:
                return "AfterOpaque";
            case ExtensionPoint::AfterTranslucency:
                return "AfterTranslucency";
            case ExtensionPoint::Overlay:
                return "Overlay";
            case ExtensionPoint::UI:
                return "UI";
            case ExtensionPoint::EditorPrimitives:
                return "EditorPrimitives";
        }
        return "<invalid ExtensionPoint>";
    }

    // The passes registered at the extension points, by name, in registration order. @p Pass carries at least
    // `std::string Name` and `ExtensionPoint Point`. Registering a name already present REPLACES that pass and
    // moves it to the back of the order (the order of the latest registrations, the same order a viewer opened
    // later sees); one list for every point, so the order inside a point is the order of the registrations.
    template <typename Pass>
    class ExtensionRegistry
    {
    public:
        void Register( Pass&& pass )
        {
            // The name is read before the pass is moved, as a statement of its own (see ArgumentOrder).
            const std::string name = pass.Name;
            Unregister( name );
            m_Passes.push_back( std::move( pass ) );
        }

        // False when no pass has that name.
        bool Unregister( std::string_view name )
        {
            const auto it = std::find_if( m_Passes.begin(), m_Passes.end(),
                                          [name]( const Pass& pass ) { return pass.Name == name; } );
            if ( it == m_Passes.end() )
                return false;
            m_Passes.erase( it );
            return true;
        }

        // Calls @p visit for each pass registered at @p point, in registration order.
        template <typename Visit>
        void ForEachAt( const ExtensionPoint point, Visit&& visit ) const
        {
            for ( const Pass& pass : m_Passes )
            {
                if ( pass.Point == point )
                    visit( pass );
            }
        }

        [[nodiscard]] const std::vector<Pass>& All() const
        {
            return m_Passes;
        }

    private:
        std::vector<Pass> m_Passes;
    };
} // namespace Desert::Graphic::RDG
