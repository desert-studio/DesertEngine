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

        static EditorMode Get() { return s_Mode; }
        static void       Set( EditorMode mode )
        {
            if ( s_Mode == mode )
                return;
            s_Mode = mode;
            if ( s_OnChange != nullptr )
                s_OnChange( mode );
        }
        static void SetChangeListener( ChangeListener listener ) { s_OnChange = listener; }
        static bool       IsFoliage() { return s_Mode == EditorMode::Foliage; }
        static bool       IsModeling()
        {
            return s_Mode == EditorMode::Modeling;
        }

    private:
        static inline EditorMode     s_Mode     = EditorMode::Select;
        static inline ChangeListener s_OnChange = nullptr;
    };
} // namespace Desert::Editor::Core
