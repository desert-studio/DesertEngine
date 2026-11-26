#pragma once

namespace Desert::Editor::Core
{
    // UE5-style viewport tool mode (the dropdown at the viewport's top-left). Select = normal gizmo/picking
    // (skeleton-edit is a sub-mode of Select); Foliage = paint instanced vegetation with a brush; Modeling =
    // geometry tools (CubeGrid blockout); Fracture = UE's Fracture Mode (DST-02, Panels/Fracture/FracturePanel).
    enum class EditorMode
    {
        Select = 0,
        Foliage,
        Modeling,
        Landscape,
        Fracture,
        VFXPaint ///< VFX-15: paint a VFX placement's point set (Panels/ViewportPanel/Tools/VFXPaintTool)
    };

    class ViewportMode final
    {
    public:
        // The one listener told of a CHANGE of mode (ViewportPanel::OnEditorModeChanged installs it: each live
        // viewport writes its scene's ECS::FoliageEditing then, never per frame). Null until a viewport exists.
        using ChangeListener = void ( * )( EditorMode mode );

        static EditorMode Get();
        static void Set( EditorMode mode );
        static void SetChangeListener( ChangeListener listener );
        static bool IsFoliage();
        static bool IsModeling();

    private:
        static EditorMode s_Mode;
        static ChangeListener s_OnChange;
    };
} // namespace Desert::Editor::Core
