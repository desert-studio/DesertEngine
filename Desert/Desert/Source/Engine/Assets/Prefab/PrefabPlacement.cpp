#include <Engine/Assets/Prefab/PrefabPlacement.hpp>

#include <spdlog/fmt/fmt.h>

namespace Desert::Assets
{
    PrefabRootKind ClassifyPrefabRoot( const EntityData& rootRecord )
    {
        // THE KEYS ARE ComponentRegistry'S OWN ("UICanvas", "UILayout"), so a record written by this
        // engine classifies by the same names it was written under. Reading a live entity instead would
        // mean the verdict could only be had after the entities existed — which is after the damage.
        if ( rootRecord.Components.get( "UICanvas" ).has_value() )
        {
            return PrefabRootKind::UICanvas;
        }
        if ( rootRecord.Components.get( "UILayout" ).has_value() )
        {
            return PrefabRootKind::UIElement;
        }
        return PrefabRootKind::World;
    }

    PrefabPlacementVerdict CheckPrefabPlacement( PrefabRootKind kind, bool targetUnderCanvas,
                                                 std::string_view source, std::string_view targetName )
    {
        switch ( kind )
        {
            case PrefabRootKind::UIElement:
                if ( !targetUnderCanvas )
                {
                    return {
                         false,
                         fmt::format( "[Prefab] '{0}' is a UI prefab: its root is a UI element, and a UI element "
                                      "is drawn only by the walk that starts at a UICanvas. Placed under {1}, "
                                      "which is not inside a canvas, it would exist in the outliner and cover "
                                      "NO PIXELS. NOTHING WAS CREATED. Select a UI Canvas (or any element under "
                                      "one) and instantiate it there, or add a Canvas to the scene first.",
                                      source, targetName ) };
                }
                return { true, {} };

            case PrefabRootKind::UICanvas:
                if ( targetUnderCanvas )
                {
                    return {
                         false,
                         fmt::format( "[Prefab] '{0}' carries its own UI Canvas, and {1} is already inside a "
                                      "canvas. A canvas under a canvas is walked twice — once as a child of the "
                                      "outer canvas and once as a root of its own — so every element in it would "
                                      "be drawn and hit-tested twice. NOTHING WAS CREATED. Instantiate it at the "
                                      "scene root, or drop the outer canvas.",
                                      source, targetName ) };
                }
                return { true, {} };

            case PrefabRootKind::World:
            default:
                // A world prefab under a canvas is NOT refused, and that is a decision. A canvas is an
                // ordinary entity with children; a mesh parented into one keeps its own 3D transform and
                // renders through the 3D path exactly as it would anywhere else. Refusing it would forbid
                // a legal arrangement in order to guess at an intent.
                return { true, {} };
        }
    }
} // namespace Desert::Assets
