#pragma once

// THE COMPONENT BLOCKS WHOSE WHOLE ON-DISK FORM IS THEIR REFLECTION — ONE LIST, TWO READERS.
//
// A block in this list is written by Reflection::SerializeReflected over one reflected struct and read back by
// Reflection::DeserializeReflected into it, with nothing else in between. That makes its canonical text a
// function of the struct alone, which two places need:
//
//   * ComponentRegistry registers a serializer per row (MakeReflected / MakeReflectedSelf), in this order;
//   * Tools/SceneMigrator rewrites every such block in the corpus into the saver's bytes (CanonicaliseScene),
//     so a scene saved with no edit is byte-identical to its file.
//
// Before this header the list lived only inside ComponentRegistry.cpp, so the migrator could not know which
// blocks are "reflection and nothing else" without a second copy of the list — the fork this file exists to
// prevent.
//
// NOT IN THIS LIST, because their file form is NOT their reflection alone: every hand-written serializer in
// ComponentRegistry.cpp (StaticMesh, SkinnedMesh, InstancedStaticMesh, Material slots, Script, Landscape root
// and tile, Foliage, AnimGraph, CubeGridBlockout, the authored components, …) and UIRenderTexture, whose
// reflected block has its `ScenePath` replaced by a `Scene` {Guid, Path} reference on disk.
//
// ORDER IS FORMAT. ComponentRegistry::All() is iterated in registration order when a record is captured, and
// the canonical writer keeps member order, so the position of a block among its record's keys IS the position
// its serializer was registered at. The rows are therefore grouped into RUNS, each registered at the point in
// RegisterBuiltins where that run always stood between the hand-written serializers.

#include <Engine/ECS/Components.hpp>

namespace Desert::Core::Serialize
{
    // Where a run is registered among the hand-written serializers (see ORDER IS FORMAT above).
    enum class ReflectedBlockRun
    {
        ActorsAndUI,          // after the animation handlers, before UIRenderTexture
        UIAfterRenderTexture, // after UIRenderTexture, before CubeGridBlockout
        Landscape,            // between the Landscape root and its tiles
        SkyAndAtmosphere      // after the tiles, before Script
    };

    // A block that is one reflected `TData` member of `TComponent`.
    template <class TComponent, class TData>
    struct ReflectedMemberBlock
    {
        using Component = TComponent;
        using Data      = TData;

        const char* Key;      // the block's key in a record
        const char* TypeName; // the reflected type, as ReflectionRegistry names it
        TData TComponent::*Member;
        ReflectedBlockRun  Run;
    };

    // A block that is the WHOLE reflected component (Skybox).
    template <class TComponent>
    struct ReflectedWholeBlock
    {
        using Component = TComponent;
        using Data      = TComponent;

        const char*       Key;
        const char*       TypeName;
        ReflectedBlockRun Run;
    };

    // Calls `visit` once per row, in registration order. The rows are distinct types, so a visitor is a
    // generic lambda; `decltype(row)::Data` is the struct a block is read into.
    template <class TVisit>
    void ForEachReflectedComponentBlock( TVisit&& visit )
    {
        using R = ReflectedBlockRun;
        using namespace ECS;
        // clang-format off
        visit( ReflectedMemberBlock<CameraComponent, CameraData>{ "Camera", "CameraData", &CameraComponent::Data, R::ActorsAndUI } );
        visit( ReflectedMemberBlock<DirectionLightComponent, DirectionalLightData>{ "DirectionLight", "DirectionalLightData", &DirectionLightComponent::Data, R::ActorsAndUI } );
        visit( ReflectedMemberBlock<PointLightComponent, PointLightData>{ "PointLight", "PointLightData", &PointLightComponent::Data, R::ActorsAndUI } );
        visit( ReflectedMemberBlock<SpotLightComponent, SpotLightData>{ "SpotLight", "SpotLightData", &SpotLightComponent::Data, R::ActorsAndUI } );
        visit( ReflectedMemberBlock<TwoBoneIKComponent, TwoBoneIKData>{ "TwoBoneIK", "TwoBoneIKData", &TwoBoneIKComponent::Data, R::ActorsAndUI } );
        visit( ReflectedMemberBlock<ControlRigComponent, ControlRigData>{ "ControlRig", "ControlRigData", &ControlRigComponent::Data, R::ActorsAndUI } );
        visit( ReflectedMemberBlock<RetargetComponent, RetargetData>{ "Retarget", "RetargetData", &RetargetComponent::Data, R::ActorsAndUI } );
        visit( ReflectedMemberBlock<ColliderComponent, ColliderData>{ "Collider", "ColliderData", &ColliderComponent::Data, R::ActorsAndUI } );
        visit( ReflectedMemberBlock<RigidBodyComponent, RigidBodyData>{ "RigidBody", "RigidBodyData", &RigidBodyComponent::Data, R::ActorsAndUI } );
        visit( ReflectedMemberBlock<CharacterControllerComponent, CharacterControllerData>{ "CharacterController", "CharacterControllerData", &CharacterControllerComponent::Data, R::ActorsAndUI } );
        visit( ReflectedMemberBlock<PlayerStartComponent, PlayerStartData>{ "PlayerStart", "PlayerStartData", &PlayerStartComponent::Data, R::ActorsAndUI } );
        visit( ReflectedMemberBlock<StreamingSourceComponent, StreamingSourceData>{ "StreamingSource", "StreamingSourceData", &StreamingSourceComponent::Data, R::ActorsAndUI } );
        visit( ReflectedMemberBlock<AudioSourceComponent, AudioSourceData>{ "AudioSource", "AudioSourceData", &AudioSourceComponent::Data, R::ActorsAndUI } );
        visit( ReflectedMemberBlock<ParticleEmitterComponent, ParticleEmitterData>{ "ParticleEmitter", "ParticleEmitterData", &ParticleEmitterComponent::Data, R::ActorsAndUI } );
        visit( ReflectedMemberBlock<UICanvasComponent, UICanvasData>{ "UICanvas", "UICanvasData", &UICanvasComponent::Data, R::ActorsAndUI } );
        visit( ReflectedMemberBlock<UILayoutComponent, UILayoutData>{ "UILayout", "UILayoutData", &UILayoutComponent::Data, R::ActorsAndUI } );
        visit( ReflectedMemberBlock<UIPanelComponent, UIPanelData>{ "UIPanel", "UIPanelData", &UIPanelComponent::Data, R::ActorsAndUI } );
        visit( ReflectedMemberBlock<UITextComponent2D, UITextData>{ "UIText", "UITextData", &UITextComponent2D::Data, R::ActorsAndUI } );
        visit( ReflectedMemberBlock<UIButtonComponent, UIButtonData>{ "UIButton", "UIButtonData", &UIButtonComponent::Data, R::ActorsAndUI } );
        visit( ReflectedMemberBlock<UIIconComponent, UIIconData>{ "UIIcon", "UIIconData", &UIIconComponent::Data, R::ActorsAndUI } );

        visit( ReflectedMemberBlock<UIBindingComponent, UIBindingData>{ "UIBinding", "UIBindingData", &UIBindingComponent::Data, R::UIAfterRenderTexture } );
        visit( ReflectedMemberBlock<UIScreenComponent, UIScreenData>{ "UIScreen", "UIScreenData", &UIScreenComponent::Data, R::UIAfterRenderTexture } );
        visit( ReflectedMemberBlock<UIScreenStackComponent, UIScreenStackData>{ "UIScreenStack", "UIScreenStackData", &UIScreenStackComponent::Data, R::UIAfterRenderTexture } );
        visit( ReflectedMemberBlock<UITweenComponent, UITweenData>{ "UITween", "UITweenData", &UITweenComponent::Data, R::UIAfterRenderTexture } );
        visit( ReflectedMemberBlock<UIPointerEventsComponent, UIPointerEventsData>{ "UIPointerEvents", "UIPointerEventsData", &UIPointerEventsComponent::Data, R::UIAfterRenderTexture } );
        visit( ReflectedMemberBlock<UIDraggableComponent, UIDraggableData>{ "UIDraggable", "UIDraggableData", &UIDraggableComponent::Data, R::UIAfterRenderTexture } );
        visit( ReflectedMemberBlock<UIDropTargetComponent, UIDropTargetData>{ "UIDropTarget", "UIDropTargetData", &UIDropTargetComponent::Data, R::UIAfterRenderTexture } );
        visit( ReflectedMemberBlock<UIImageComponent, UIImageData>{ "UIImage", "UIImageData", &UIImageComponent::Data, R::UIAfterRenderTexture } );
        visit( ReflectedMemberBlock<UILayoutGroupComponent, UILayoutGroupData>{ "UILayoutGroup", "UILayoutGroupData", &UILayoutGroupComponent::Data, R::UIAfterRenderTexture } );
        visit( ReflectedMemberBlock<UIProgressBarComponent, UIProgressBarData>{ "UIProgressBar", "UIProgressBarData", &UIProgressBarComponent::Data, R::UIAfterRenderTexture } );
        visit( ReflectedMemberBlock<UIStyleComponent, UIStyleData>{ "UIStyle", "UIStyleData", &UIStyleComponent::Data, R::UIAfterRenderTexture } );
        visit( ReflectedMemberBlock<UIToggleComponent, UIToggleData>{ "UIToggle", "UIToggleData", &UIToggleComponent::Data, R::UIAfterRenderTexture } );
        visit( ReflectedMemberBlock<UISliderComponent, UISliderData>{ "UISlider", "UISliderData", &UISliderComponent::Data, R::UIAfterRenderTexture } );
        visit( ReflectedMemberBlock<UIScrollViewComponent, UIScrollViewData>{ "UIScrollView", "UIScrollViewData", &UIScrollViewComponent::Data, R::UIAfterRenderTexture } );
        visit( ReflectedMemberBlock<UIListViewComponent, UIListViewData>{ "UIListView", "UIListViewData", &UIListViewComponent::Data, R::UIAfterRenderTexture } );
        visit( ReflectedMemberBlock<UIInputFieldComponent, UIInputFieldData>{ "UIInputField", "UIInputFieldData", &UIInputFieldComponent::Data, R::UIAfterRenderTexture } );
        visit( ReflectedMemberBlock<UIDropdownComponent, UIDropdownData>{ "UIDropdown", "UIDropdownData", &UIDropdownComponent::Data, R::UIAfterRenderTexture } );
        // Overlays (Ю12): an overlay canvas and a trigger are ordinary scene data, so a tooltip, a menu, a
        // dialog and a toast stack survive a save and a reload because they are entities like any other.
        visit( ReflectedMemberBlock<UIOverlayComponent, UIOverlayData>{ "UIOverlay", "UIOverlayData", &UIOverlayComponent::Data, R::UIAfterRenderTexture } );
        visit( ReflectedMemberBlock<UIOverlayTriggerComponent, UIOverlayTriggerData>{ "UIOverlayTrigger", "UIOverlayTriggerData", &UIOverlayTriggerComponent::Data, R::UIAfterRenderTexture } );

        visit( ReflectedMemberBlock<LandscapeMaterialComponent, LandscapeMaterialData>{ "LandscapeMaterial", "LandscapeMaterialData", &LandscapeMaterialComponent::Data, R::Landscape } );

        // Skybox reflects WHOLE (RA3): it carries the HDR path only, the procedural sky is SkyAtmosphere.
        visit( ReflectedWholeBlock<SkyboxComponent>{ "Skybox", "SkyboxComponent", R::SkyAndAtmosphere } );
        visit( ReflectedMemberBlock<SkyAtmosphereComponent, SkyAtmosphereData>{ "SkyAtmosphere", "SkyAtmosphereData", &SkyAtmosphereComponent::Data, R::SkyAndAtmosphere } );
        visit( ReflectedMemberBlock<ExponentialHeightFogComponent, ExponentialHeightFogData>{ "ExponentialHeightFog", "ExponentialHeightFogData", &ExponentialHeightFogComponent::Data, R::SkyAndAtmosphere } );
        visit( ReflectedMemberBlock<PostProcessVolumeComponent, PostProcessVolumeData>{ "PostProcessVolume", "PostProcessVolumeData", &PostProcessVolumeComponent::Data, R::SkyAndAtmosphere } );
        visit( ReflectedMemberBlock<VolumetricCloudComponent, VolumetricCloudData>{ "VolumetricCloud", "VolumetricCloudData", &VolumetricCloudComponent::Data, R::SkyAndAtmosphere } );
        visit( ReflectedMemberBlock<HeroCloudComponent, HeroCloudData>{ "HeroCloud", "HeroCloudData", &HeroCloudComponent::Data, R::SkyAndAtmosphere } );
        // clang-format on
    }
} // namespace Desert::Core::Serialize
