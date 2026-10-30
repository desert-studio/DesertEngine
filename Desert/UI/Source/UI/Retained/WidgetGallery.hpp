#pragma once

#include <UI/Retained/WidgetTree.hpp>

#include <string>

// THE WIDGET GALLERY — one canvas built by WidgetBuilder with a button, a list, an input field, a slider and a
// toggle, every one bound to the model below by attributes and answering through delegates. It is the
// retained tree's proof (Scenarios/UI/EDITOR-UI-E1.scn): the editor's UI Preview shows it over the level
// ("UI: Toggle the widget gallery"), and it must draw as the same widgets authored as an ECS canvas draw.
namespace Desert::UI
{
    // What the gallery's widgets show and edit. The widgets hold nothing of their own: every value they show
    // is read from here each frame, and every change they make comes back here through a delegate.
    struct WidgetGalleryModel
    {
        int         Clicks   = 0;
        float       Volume   = 0.5f;
        bool        Enabled  = true;
        std::string Name     = "Desert";
        std::string LastPick = "none";
    };

    // Build the gallery into @p tree as a new root canvas bound to @p model; returns the canvas. @p model must
    // outlive the nodes (the attributes and delegates read and write it).
    NodeId BuildWidgetGallery( WidgetTree& tree, WidgetGalleryModel& model );
} // namespace Desert::UI
