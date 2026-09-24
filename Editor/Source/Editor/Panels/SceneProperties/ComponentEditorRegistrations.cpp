// Reflected component editors: the full Details UI is auto-built from each data block's REFLECT()
// metadata (PropertyEditorBuilder) — no widget class, no edit to ComponentEditor. To expose a new
// reflected component in the editor, copy one line below.

#include <Editor/Panels/ViewportPanel/CameraPilot.hpp>
#include <Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp>
#include <Editor/Panels/UI/UIAnchorControls.hpp>
#include <Editor/Core/DragPayloads.hpp>
#include <Editor/Core/SubjectOpenRequest.hpp>
#include <Editor/Panels/PanelContext.hpp>
#include <Editor/Panels/Clouds/CloudsPanel.hpp>
#include <Editor/Panels/Particles/ParticleEditorPanel.hpp>
#include <Editor/Panels/UI/UIEditorPanel.hpp>
#include <Editor/Panels/Sequencer/SequencerPanel.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/System/SystemRules.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Engine/Geometry/PrimitiveMeshFactory.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Font/FontService.hpp>
#include <Engine/Runtime/Services/UITheme/UIThemeService.hpp>
#include <Engine/UI/UICanvasLayout.hpp>
#include <Engine/UI/UIStyleResolver.hpp>
#include <Engine/Graphic/Clouds/CloudMaterialValues.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/Shader.hpp>
#include <Editor/Import/MeshDnD.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <Engine/Core/Formats/ShaderProgramMeta.hpp>

#include <ImGui/imgui.h>
#include <glm/glm.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ThemeManager.hpp>
#include <Editor/Import/TextureDnD.hpp>
#include <Editor/Core/ColliderFit.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Panels/SceneProperties/ComponentWidgets/MaterialsPanelComponent.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/TextureAsset.hpp>
#include <Engine/Assets/MaterialData.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
// rfl serialization environment (the same three the mesh slot editor pulls in for the same reason) — a
// fresh landscape material is written to disk with its stable GUID before the asset is created + registered.
#include <Common/Core/Serialization/GlmReflection.hpp>
#include <rflcpp/rfl/json.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
#include <Editor/Panels/MaterialEditor/MaterialDocumentOpen.hpp>
#include <Common/Core/Logger.hpp>
#include <Engine/Core/Serialize/ComponentRegistry.hpp>
#include <Engine/Animation/AnimationLibrary.hpp>
#include <Engine/Scripting/ScriptEngine.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Core/Constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>

// Directional light is a CUSTOM entry: the reflected fields PLUS a sun dial, because the sun's direction
// is not a field — it hides in TransformComponent.Translation. See MakeDirectionalLightEntry.
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::PointLightComponent, Data, "PointLightData", "Point Light" )
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::SpotLightComponent, Data, "SpotLightData", "Spot Light" )
// Camera is a CUSTOM entry: reflected fields + a focal-length readout and "look through". See MakeCameraEntry.
// Landscape Material is a CUSTOM entry: reflected LandscapeMaterialData UI (the layer modes) + the
// landscape's MATERIAL ROW (an asset field and an Edit button — the material itself is authored in the
// Material Editor window, like every other). See MakeLandscapeMaterialEntry below.
// Collider is registered as a CUSTOM component below (auto-fit to mesh bounds on add) instead of the
// plain reflected one-liner — see MakeColliderEntry.
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::RigidBodyComponent, Data, "RigidBodyData", "Rigid Body" )
// Character Controller is a CUSTOM entry: the reflected capsule fields PLUS the live state the physics
// step writes back (on ground / speed / swimming). Those are the values you actually need while the game
// runs, and they were invisible. See MakeCharacterControllerEntry.
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::AudioSourceComponent, Data, "AudioSourceData", "Audio Source" )
// Two-Bone IK is the reflected one-liner and deliberately so: it is four values an artist types, and every
// piece of behaviour behind them belongs to the Animator's control list rather than to this page. The entry
// next door — AnimationComponent — is a custom one because it owns an Animator and a graph; that is the line
// between the two.
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::TwoBoneIKComponent, Data, "TwoBoneIKData", "Two-Bone IK" )
// Control Rig is the reflected one-liner for Two-Bone IK's reason and more so: it is ONE value an artist
// picks. Everything behind it — resolving the file's names against this entity's skeleton, building the
// pipeline stage, rebuilding it when the file changes — belongs to AnimationECSSystem, and the panel that
// lets an animator grab a control is a panel (View -> Control Rig), not a Details page.
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::ControlRigComponent, Data, "ControlRigData", "Control Rig" )
// Retarget is the reflected one-liner for the Control Rig's reason: it is ONE value an artist picks, and
// everything behind it — binding the source rig by signature, resolving both rigs' names, building the
// retargeter, rebuilding it when either side moves — belongs to AnimationECSSystem.
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::RetargetComponent, Data, "RetargetData", "Retarget" )
// Particle Emitter is a CUSTOM entry: the reflected fields plus a transport (play / pause / restart),
// because "is it emitting right now" is a state you drive, not a value you type. See MakeEmitterEntry.
// UI Canvas is a CUSTOM entry: the reflected fields PLUS "Open in UI Editor", which is what the UI Editor
// becoming a document (U7-2) bought. The window used to be a tool that drew the FIRST canvas in the scene,
// so a button here could only ever have said "reveal that window", never "edit THIS canvas". Ю1 removed the
// last of that: the renderer itself takes the canvas as an argument, so the window draws the canvas the
// button names. See MakeUICanvasEntry.
// UI Layout is a CUSTOM entry (not the reflected one-liner) so the Details panel gets Unity-style anchor
// presets ("Fill / Match Parent" + a 4x4 grid) above the raw anchor/offset fields. See MakeUILayoutEntry.
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::UIPanelComponent, Data, "UIPanelData", "UI Panel" )
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::UITextComponent2D, Data, "UITextData", "UI Text" )
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::UIButtonComponent, Data, "UIButtonData", "UI Button" )
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::UIIconComponent, Data, "UIIconData", "UI Icon" )
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::UIRenderTextureComponent, Data, "UIRenderTextureData",
                                     "UI Render Texture" )
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::UIBindingComponent, Data, "UIBindingData", "UI Binding" )
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::UIScreenComponent, Data, "UIScreenData", "UI Screen" )
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::UIScreenStackComponent, Data, "UIScreenStackData",
                                     "UI Screen Stack" )
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::UITweenComponent, Data, "UITweenData", "UI Tween" )
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::UIPointerEventsComponent, Data, "UIPointerEventsData",
                                     "UI Pointer Events" )
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::UIDraggableComponent, Data, "UIDraggableData", "UI Draggable" )
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::UIDropTargetComponent, Data, "UIDropTargetData",
                                     "UI Drop Target" )
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::UIImageComponent, Data, "UIImageData", "UI Image" )
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::UILayoutGroupComponent, Data, "UILayoutGroupData",
                                     "UI Layout Group" )
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::UIProgressBarComponent, Data, "UIProgressBarData",
                                     "UI Progress Bar" )
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::UIToggleComponent, Data, "UIToggleData", "UI Toggle" )
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::UISliderComponent, Data, "UISliderData", "UI Slider" )
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::UIScrollViewComponent, Data, "UIScrollViewData",
                                     "UI Scroll View" )
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::UIListViewComponent, Data, "UIListViewData", "UI List View" )
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::UIInputFieldComponent, Data, "UIInputFieldData",
                                     "UI Input Field" )
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::UIDropdownComponent, Data, "UIDropdownData", "UI Dropdown" )
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::UIOverlayComponent, Data, "UIOverlayData", "UI Overlay" )
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::UIOverlayTriggerComponent, Data, "UIOverlayTriggerData",
                                     "UI Overlay Trigger" )
// Exponential Height Fog is the plain reflected one-liner: every field is a value, nothing needs the
// scene, and the fog height deliberately is not a field (it is the entity's transform Y).
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::ExponentialHeightFogComponent, Data,
                                     "ExponentialHeightFogData", "Exponential Height Fog" )

// Volumetric Cloud is a CUSTOM entry since O1: the reflected budget/routing fields PLUS the material
// row — the same one-handle-with-Edit-button arrangement the landscape has, for the same Stage 3 reason.
// See MakeVolumetricCloudEntry below.

// The seam's AUTHORED producer: one sculpted body, placed by this entity's transform. It is a per-entity
// component rather than another field of the layer, because there may be several of them and each has a
// place in the world; the layer is one shell and has none.
DESERT_REGISTER_REFLECTED_COMPONENT( ::Desert::ECS::HeroCloudComponent, Data, "HeroCloudData", "Hero Cloud" )
// Sky Atmosphere is a CUSTOM entry: the reflected fields PLUS the sky-colour ramp (which needs the scene's
// sun elevation, and that is not a field) and the IBL bake button. See
// ComponentWidgets/SkyAtmosphereComponent.cpp.

namespace Desert::Editor
{
    // The engine has its own Desert::ImGui namespace, so an unqualified ImGui:: inside Desert::Editor
    // resolves there instead of to the library. Alias it once, like every other editor TU does.
    namespace ImGui = ::ImGui;

    // Creates a `.demat` already set to the Terrain shader and registers its shell.
    //
    // The shader is not a choice the user makes here. There is exactly ONE program of domain Terrain, so a
    // picker offering it would be a control with a single entry; the material that draws a terrain is a
    // Terrain material or it is nothing, and pressing New is how you say so.
    //
    // Filename is "M_<Entity>_Landscape" (sanitized), first free index — a LABEL only: the stable identity is
    // the MaterialId inside the file, so renaming the entity or the file later breaks no reference. Same
    // rule, and the same write-then-load order, as the mesh slot editor's CreateAndRegisterMaterial.
    static ::Desert::Assets::AssetHandle CreateLandscapeMaterial( const std::string&              entityName,
                                                                  ::Desert::Assets::AssetManager* assetMgr )
    {
        if ( !assetMgr )
            return ::Desert::Assets::AssetHandle( static_cast<uint64_t>( 0 ) );

        std::string base;
        base.reserve( entityName.size() + 8 );
        for ( const char c : entityName )
            base += ( std::isalnum( static_cast<unsigned char>( c ) ) || c == '_' || c == '-' ) ? c : '_';
        base = "M_" + ( base.empty() ? std::string( "Landscape" ) : base ) + "_Landscape";

        const std::string           ext( ::Common::Constants::Extensions::MATERIAL_EXTENSION );
        const std::filesystem::path dir = ::Common::Constants::Path::MATERIAL_PATH;
        std::error_code             ec;
        std::filesystem::create_directories( dir, ec );

        std::filesystem::path path = dir / ( base + ext );
        for ( int n = 1; std::filesystem::exists( path, ec ); ++n )
            path = dir / ( base + "_" + std::to_string( n ) + ext );

        // Write the file FIRST (Terrain shader + a freshly stamped MaterialId), then create-with-load: the
        // asset adopts its in-file GUID as the internal handle during Load, so the handle registered here is
        // the one every future editor run resolves to.
        {
            ::Desert::Assets::MaterialData data;
            data.ShaderName = "Terrain";
            data.MaterialId = ::Common::UUID::Generate();
            // The second step below is checked carefully and the first was not, even though the whole
            // point of this order is that the asset ADOPTS the GUID out of the file: an unwritten file
            // means CreateAsset loads defaults, the material is not a Terrain material at all, and the
            // handle is not the one any future run will resolve.
            if ( const auto written = ::Common::Utils::FileSystem::WriteContentToFileAtomic(
                      path.generic_string(), rfl::json::write( data ) );
                 !written )
            {
                LOG_ERROR( "[Landscape] could not write the landscape material '{}': {} — the landscape's "
                           "material slot is unchanged.",
                           path.generic_string(), written.GetError() );
                return ::Desert::Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
            }
        }

        auto asset = assetMgr->CreateAsset<::Desert::Assets::SurfaceMaterialAsset>(
             ::Desert::Assets::AssetPriority::High, path.generic_string() );
        if ( !asset )
        {
            LOG_ERROR( "[Landscape] could not create a landscape material at '{}' — the landscape's material "
                       "slot is unchanged.",
                       path.generic_string() );
            return ::Desert::Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
        }

        // The SHELL only (RegisterAsset, not Register). A landscape material never becomes a runtime
        // Graphic::Material: TerrainRenderer owns one material of its own and takes this asset's values by
        // name. Registering eagerly would build a per-object material for a tessellated patch program that
        // nothing draws.
        if ( auto* materialService = ::Desert::Runtime::ResourceRegistry::GetMaterialService() )
            materialService->RegisterAsset( asset );
        return asset->GetMetadata().Handle;
    }

    // Opens the Material Editor window on the landscape's material — the same seam, and the same three
    // outcomes, the mesh slot editor's Edit button uses. After M2 this is the only place a material's
    // parameters and textures are edited, the landscape's included.
    static void OpenMaterialEditorFor( const ::Desert::Assets::AssetHandle& handle,
                                       ::Desert::Assets::AssetManager* assetMgr, const char* tag )
    {
        if ( !assetMgr )
            return;
        auto asset = assetMgr->FindByHandle<::Desert::Assets::SurfaceMaterialAsset>(
             ::Common::UUID( static_cast<uint64_t>( handle ) ) );
        if ( !asset )
        {
            LOG_ERROR( "{} the material in this slot (handle {}) is not in the asset database — no "
                       "window was opened.",
                       tag, static_cast<uint64_t>( handle ) );
            return;
        }

        const std::string path = asset->GetMetadata().Filepath.generic_string();
        switch ( ::Desert::Editor::RequestMaterialDocument( assetMgr, path ) )
        {
            case ::Desert::Editor::MaterialDocumentRequest::Requested:
            case ::Desert::Editor::MaterialDocumentRequest::Failed: // already reported, with the path
                break;
            case ::Desert::Editor::MaterialDocumentRequest::NotAMaterialPath:
                // The slot resolves to an asset whose file is not on disk. Silence would read as a dead button.
                LOG_ERROR( "{} this slot's material has no `.demat` on disk ('{}') — no window was "
                           "opened. Save it first, or reassign the slot.",
                           tag, path );
                break;
        }
    }

    // The landscape's MATERIAL row. What it is NOT, because it replaced exactly that: a shader combo and a
    // schema-driven parameter table writing into the entity's ECS::MaterialComponent. That was the last
    // place in the editor that AUTHORED a material anywhere but in a `.demat`, and it meant the terrain had
    // its own way to edit a material while everything else had the Material Editor window
    // (Docs/MaterialEditor/PLAN_STAGE3_ASSET_DOCUMENTS.md, M3).
    //
    // ONE handle, not a slot vector. `Terrain.shader` is a single program of domain Terrain whose three
    // splat layers (u_GrassTex/u_RockTex/u_SnowTex) are TEXTURE PARAMETERS of that one program, blended
    // in-shader by the layer modes above. A vector would promise a material per layer and nothing
    // downstream could consume one.
    //
    // The combo went with the schema table: there is exactly one Terrain-domain shader, so "which shader"
    // was a control with one entry. A landscape material is created with that shader already chosen.
    static void DrawLandscapeMaterialRow( ::Desert::ECS::LandscapeMaterialData& landscape,
                                          const std::string& entityName, ::Desert::Assets::AssetManager* assetMgr )
    {
        namespace ImGui = ::ImGui;

        const ::Desert::Assets::SurfaceMaterialAsset* asset = nullptr;
        if ( assetMgr && static_cast<uint64_t>( landscape.Material ) != 0 )
        {
            asset = assetMgr
                         ->FindByHandle<::Desert::Assets::SurfaceMaterialAsset>(
                              ::Common::UUID( static_cast<uint64_t>( landscape.Material ) ) )
                         .get();
        }

        // A bound-but-unresolvable handle reads "(missing)" and never a raw number: the scene names a
        // material the asset database does not have, and that is a different problem from an empty slot.
        std::string display = "None";
        if ( static_cast<uint64_t>( landscape.Material ) != 0 )
        {
            display = asset ? std::filesystem::path( asset->GetMetadata().Filepath.string() ).stem().string()
                            : "(missing)";
        }

        if ( !ImGui::BeginTable( "##landscape_mat", 2,
                                 ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings ) )
            return;
        ImGui::TableSetupColumn( "label", ImGuiTableColumnFlags_WidthStretch, 0.38f );
        ImGui::TableSetupColumn( "control", ImGuiTableColumnFlags_WidthStretch, 0.62f );

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( "Material" );
        ImGui::TableNextColumn();

        ImGui::PushItemWidth( -FLT_MIN );
        ImGui::Button( ( display + "##landscape_mat_slot" ).c_str(), ImVec2( -FLT_MIN, 0.0f ) );
        ImGui::PopItemWidth();
        if ( ImGui::BeginDragDropTarget() )
        {
            if ( const ImGuiPayload* pl =
                      ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::MaterialAsset ) )
            {
                const std::string path( static_cast<const char*>( pl->Data ),
                                        pl->DataSize > 0 ? pl->DataSize - 1 : 0 );
                if ( assetMgr && !path.empty() )
                {
                    auto dropped = assetMgr->FindByPath<::Desert::Assets::SurfaceMaterialAsset>( path );
                    if ( !dropped )
                    {
                        dropped = assetMgr->CreateAsset<::Desert::Assets::SurfaceMaterialAsset>(
                             ::Desert::Assets::AssetPriority::High, path );
                        if ( dropped && !dropped->IsReadyForUse() )
                            dropped->Load();
                    }
                    if ( dropped )
                    {
                        // The shell only: the landscape never asks for a runtime Graphic::Material, it asks
                        // MaterialService for this material's VALUES (ResolveOverrides) and applies them to
                        // the one material the TerrainRenderer owns.
                        if ( auto* materialService = ::Desert::Runtime::ResourceRegistry::GetMaterialService() )
                            materialService->RegisterAsset( dropped );
                        landscape.Material = dropped->GetMetadata().Handle;
                    }
                    else
                    {
                        LOG_ERROR( "[Landscape] '{}' could not be opened as a material — the landscape's "
                                   "material slot is unchanged.",
                                   path );
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Drag a .demat here, or press New to author one on the Terrain shader." );

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        if ( static_cast<uint64_t>( landscape.Material ) == 0 )
        {
            if ( ImGui::Button( "New Landscape Material", ImVec2( -FLT_MIN, 0.0f ) ) )
            {
                const auto created = CreateLandscapeMaterial( entityName, assetMgr );
                if ( static_cast<uint64_t>( created ) != 0 )
                {
                    landscape.Material = created;
                    OpenMaterialEditorFor( created, assetMgr, "[Landscape]" );
                }
            }
        }
        else
        {
            const float half = ( ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x ) * 0.5f;
            if ( ImGui::Button( "Edit", ImVec2( half, 0.0f ) ) )
                OpenMaterialEditorFor( landscape.Material, assetMgr, "[Landscape]" );
            ImGui::SameLine();
            if ( ImGui::Button( "Clear", ImVec2( half, 0.0f ) ) )
                landscape.Material = ::Desert::Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
        }
        ImGui::EndTable();
    }

    // Sizes a collider to the entity's mesh bounds (so the green wireframe wraps the visible object —
    // UE auto-fits collision to the mesh instead of leaving a default 0.5 cube). HalfExtents/Radius are
    // world units, so we multiply the local AABB by the entity's scale (PhysicsECSSystem feeds these to
    // Jolt directly, ignoring the transform's scale).
    // Collider fitting lives in Editor/Core/ColliderFit.hpp — the toolbar's Collision menu measures the
    // same mesh the same way, and a warning that disagrees with the button that silences it is worse than
    // no warning.
    using ::Desert::Editor::Core::FitColliderToMesh;
    using ::Desert::Editor::Core::MeshHalfExtents;

    // Collider editor: same auto-built reflected UI as the one-liner, PLUS a one-time auto-fit on Add and
    // a manual "Fit to Mesh Bounds" button.
    // A top-down hemisphere dial for a sun direction: the centre is straight up (elevation 90 deg), the
    // rim is the horizon, and the angle around the circle is the compass azimuth (up = +Z, right = +X).
    // Dragging moves the sun; the numeric sliders beside it stay the precise path (and are the only way
    // to put the sun BELOW the horizon, which a hemisphere cannot show).
    static bool DrawSunDial( float& azimuthDeg, float& elevationDeg, float diameter )
    {
        ImDrawList*  dl     = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float  r      = diameter * 0.5f;
        const ImVec2 c( origin.x + r, origin.y + r );

        ImGui::InvisibleButton( "##sundial", ImVec2( diameter, diameter ) );

        bool changed = false;
        if ( ImGui::IsItemActive() )
        {
            const ImVec2 m   = ImGui::GetIO().MousePos;
            const float  dx  = m.x - c.x;
            const float  dy  = m.y - c.y;
            const float  len = std::sqrt( dx * dx + dy * dy );

            azimuthDeg = glm::degrees( std::atan2( dx, -dy ) );
            if ( azimuthDeg < 0.0f )
                azimuthDeg += 360.0f;
            elevationDeg = ( 1.0f - glm::min( 1.0f, len / r ) ) * 90.0f;
            changed      = true;
        }

        const ImU32 ring = ImGui::GetColorU32( ImGuiCol_Border );
        dl->AddCircleFilled( c, r, ImGui::GetColorU32( ImGuiCol_FrameBg ), 48 );
        dl->AddCircle( c, r, ring, 48 );
        dl->AddCircle( c, r * 0.5f, ring, 32 ); // the 45 deg elevation ring
        dl->AddLine( ImVec2( c.x - r, c.y ), ImVec2( c.x + r, c.y ), ring );
        dl->AddLine( ImVec2( c.x, c.y - r ), ImVec2( c.x, c.y + r ), ring );

        const ImU32 label = ImGui::GetColorU32( ImGuiCol_TextDisabled );
        dl->AddText( ImVec2( c.x - 3.0f, c.y - r - 2.0f ), label, "N" );
        dl->AddText( ImVec2( c.x + r - 6.0f, c.y - 7.0f ), label, "E" );
        dl->AddText( ImVec2( c.x - 3.0f, c.y + r - 14.0f ), label, "S" );
        dl->AddText( ImVec2( c.x - r + 2.0f, c.y - 7.0f ), label, "W" );

        // The sun itself. Below the horizon it is pinned to the rim and drawn hollow — the dial covers
        // the sky, so "night" has to read as a state rather than a position.
        const bool   belowHorizon = elevationDeg < 0.0f;
        const float  el           = glm::clamp( elevationDeg, 0.0f, 90.0f );
        const float  rr           = ( 1.0f - el / 90.0f ) * r;
        const float  azr          = glm::radians( azimuthDeg );
        const ImVec2 sun( c.x + std::sin( azr ) * rr, c.y - std::cos( azr ) * rr );
        dl->AddLine( c, sun, ImGui::GetColorU32( ImGuiCol_TextDisabled ) );
        if ( belowHorizon )
            dl->AddCircle( sun, 6.0f, IM_COL32( 120, 130, 160, 255 ), 16, 2.0f );
        else
            dl->AddCircleFilled( sun, 6.0f, IM_COL32( 255, 210, 90, 255 ), 16 );

        return changed;
    }

    // Directional light: the reflected fields, plus the thing that is NOT a field — where the sun is.
    // The engine stores the sun as the direction light TRAVELS in TransformComponent.Translation
    // (Scene.cpp uploads normalize(Translation); the sky negates it), which is unauthorable as three
    // raw numbers. This edits it as azimuth/elevation.
    static ComponentEditorEntry MakeDirectionalLightEntry()
    {
        using C = ::Desert::ECS::DirectionLightComponent;
        ComponentEditorEntry e;
        e.Name              = "Directional Light";
        e.CanRemove         = true;
        e.ReflectedTypeName = "DirectionalLightData";
        e.Has               = []( ::Desert::ECS::Entity& en ) { return en.HasComponent<C>(); };
        e.Add               = []( ::Desert::ECS::Entity& en ) { en.AddComponent<C>(); };
        e.Remove            = []( ::Desert::ECS::Entity& en ) { en.RemoveComponent<C>(); };
        e.DataPtr           = []( ::Desert::ECS::Entity& en ) -> void* { return &en.GetComponent<C>().Data; };
        e.Draw = []( ::Desert::ECS::Entity& en, ::Desert::Core::Scene*, const ComponentEditContext& ctx )
        {
            auto& c = en.GetComponent<C>();
            PropertyEditorBuilder::Draw( &c.Data, "DirectionalLightData", ctx.AssetMgr(), ctx.UIHelper,
                                         ctx.FieldFilter );

            if ( ctx.FieldFilter || !en.HasComponent<::Desert::ECS::TransformComponent>() )
                return; // while searching, only the matched fields are on screen

            auto& t = en.GetComponent<::Desert::ECS::TransformComponent>();

            // Translation is the TRAVEL direction; the sun sits the other way.
            // Through the rules on both counts. The threshold was a fourth spelling of
            // kSunDirectionEpsilon, and `-travel / length` was a fourth copy of the negation that
            // SystemRules.hpp calls "the engine's ONE negation" — in the widget a user reaches for to
            // aim a sun, so a disagreement here shows a direction for a light the renderer has already
            // thrown away.
            glm::vec3 travel = t.Translation;
            if ( !::Desert::ECS::Rules::IsSunDirectionValid( travel ) )
                travel = glm::vec3( -0.4f, -1.0f, -0.5f ); // the dial's placeholder aim, not a light
            const float     length = glm::length( travel );
            const glm::vec3 toSun  = ::Desert::ECS::Rules::AtmosphereSunDirection( travel );

            float elevation = glm::degrees( std::asin( glm::clamp( toSun.y, -1.0f, 1.0f ) ) );
            float azimuth   = glm::degrees( std::atan2( toSun.x, toSun.z ) );
            if ( azimuth < 0.0f )
                azimuth += 360.0f;

            if ( !::Desert::Editor::Utils::ImGuiUtilities::SectionHeader( ICON_MDI_WEATHER_SUNNY
                                                                          "  Sun Direction" ) )
                return;

            ImGui::Indent( 6.0f );
            bool changed = DrawSunDial( azimuth, elevation, 120.0f );

            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::SetNextItemWidth( -1.0f );
            changed |= ImGui::SliderFloat( "##azimuth", &azimuth, 0.0f, 360.0f, "Azimuth %.0f deg" );
            ImGui::SetNextItemWidth( -1.0f );
            changed |= ImGui::SliderFloat( "##elevation", &elevation, -90.0f, 90.0f, "Elevation %.0f deg" );
            if ( elevation < 0.0f )
                ImGui::TextColored( ImVec4( 0.6f, 0.65f, 0.8f, 1.0f ), ICON_MDI_WEATHER_NIGHT " below horizon" );
            ImGui::EndGroup();

            if ( changed )
            {
                const float     az = glm::radians( azimuth );
                const float     el = glm::radians( elevation );
                const glm::vec3 dir( std::cos( el ) * std::sin( az ), std::sin( el ),
                                     std::cos( el ) * std::cos( az ) );
                // Keep the vector's length: some scenes author it as a "sun position" and only the
                // direction is read, so rewriting the magnitude would be a silent edit.
                t.Translation = -dir * length;
            }

            ImGui::Unindent( 6.0f );
        };
        return e;
    }

    // Camera: the reflected fields, plus the two things a camera needs that numbers alone don't give —
    // the lens in millimetres photographers think in, and a way to see what it sees.
    static ComponentEditorEntry MakeCameraEntry()
    {
        using C = ::Desert::ECS::CameraComponent;
        ComponentEditorEntry e;
        e.Name              = "Camera";
        e.CanRemove         = true;
        e.ReflectedTypeName = "CameraData";
        e.Has               = []( ::Desert::ECS::Entity& en ) { return en.HasComponent<C>(); };
        e.Add               = []( ::Desert::ECS::Entity& en ) { en.AddComponent<C>(); };
        e.Remove            = []( ::Desert::ECS::Entity& en ) { en.RemoveComponent<C>(); };
        e.DataPtr           = []( ::Desert::ECS::Entity& en ) -> void* { return &en.GetComponent<C>().Data; };
        e.Draw = []( ::Desert::ECS::Entity& en, ::Desert::Core::Scene* scene, const ComponentEditContext& ctx )
        {
            auto& c = en.GetComponent<C>();
            PropertyEditorBuilder::Draw( &c.Data, "CameraData", ctx.AssetMgr(), ctx.UIHelper, ctx.FieldFilter );

            if ( ctx.FieldFilter )
                return;

            // Focal length <-> vertical FOV on a 35mm full-frame sensor (24mm high), the lens language
            // every reference shot is quoted in. Purely a second view of the SAME field.
            constexpr float kSensorHalfHeightMm = 12.0f;
            const float     fovRad              = glm::radians( glm::clamp( c.Data.FOV, 1.0f, 179.0f ) );
            float           focalMm             = kSensorHalfHeightMm / std::tan( fovRad * 0.5f );

            ImGui::Separator();
            ImGui::SetNextItemWidth( 180.0f );
            if ( ImGui::DragFloat( "Focal length", &focalMm, 0.5f, 4.0f, 800.0f, "%.0f mm" ) )
            {
                const float newFov =
                     2.0f * glm::degrees( std::atan( kSensorHalfHeightMm / glm::max( focalMm, 1.0f ) ) );
                c.Data.FOV = glm::clamp( newFov, 10.0f, 120.0f );
            }
            ::Desert::Editor::Utils::ImGuiUtilities::Tooltip(
                 "The same setting as Field of View, in 35mm-equivalent lens terms (24mm sensor height)." );

            // PILOT, AS IN UE: the viewport locks to this camera and shows exactly what it sees — and keeps
            // showing it, so the FOV above moves the view the moment it is dragged. It replaced a one-off
            // jump of the editor camera that carried neither roll nor lens (CameraPilot.hpp).
            if ( scene && en.HasComponent<::Desert::ECS::TransformComponent>() &&
                 en.HasComponent<::Desert::ECS::UUIDComponent>() )
            {
                const auto uuid = en.GetComponent<::Desert::ECS::UUIDComponent>().UUID;
                if ( ::Desert::Editor::IsPiloted( uuid ) )
                {
                    if ( ImGui::Button( ICON_MDI_EJECT "  Eject", ImVec2( -1.0f, 0.0f ) ) )
                    {
                        if ( const auto ejected = ::Desert::Editor::EjectPilot(); !ejected )
                            LOG_WARN( "[Camera] eject refused: {}", ejected.GetError() );
                    }
                    ::Desert::Editor::Utils::ImGuiUtilities::Tooltip(
                         "Stop piloting: the viewport returns to where it was before." );
                }
                else
                {
                    if ( ImGui::Button( ICON_MDI_EYE "  Pilot (look through this camera)",
                                        ImVec2( -1.0f, 0.0f ) ) )
                    {
                        if ( const auto piloted = ::Desert::Editor::PilotCameraEntity( uuid ); !piloted )
                            LOG_WARN( "[Camera] pilot refused: {}", piloted.GetError() );
                    }
                    ::Desert::Editor::Utils::ImGuiUtilities::Tooltip(
                         "Locks the viewport to this camera: it shows exactly what the camera sees (FOV, roll,\n"
                         "near/far), and flying the viewport moves the camera. Eject returns the viewport." );
                }
            }
        };
        return e;
    }

    // Particle emitter: transport first, then the reflected parameters. Pause writes Enabled (the same
    // field the renderer reads, so nothing new can drift out of sync) and Restart raises the component's
    // one-shot flag that ParticleRenderer consumes next frame.
    static ComponentEditorEntry MakeEmitterEntry()
    {
        using C = ::Desert::ECS::ParticleEmitterComponent;
        ComponentEditorEntry e;
        e.Name              = "Particle Emitter";
        e.CanRemove         = true;
        e.ReflectedTypeName = "ParticleEmitterData";
        e.Has               = []( ::Desert::ECS::Entity& en ) { return en.HasComponent<C>(); };
        e.Add               = []( ::Desert::ECS::Entity& en ) { en.AddComponent<C>(); };
        e.Remove            = []( ::Desert::ECS::Entity& en ) { en.RemoveComponent<C>(); };
        e.DataPtr           = []( ::Desert::ECS::Entity& en ) -> void* { return &en.GetComponent<C>().Data; };
        e.Draw = []( ::Desert::ECS::Entity& en, ::Desert::Core::Scene*, const ComponentEditContext& ctx )
        {
            auto& c = en.GetComponent<C>();

            if ( !ctx.FieldFilter )
            {
                const float w = ( ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x ) * 0.5f;
                if ( ImGui::Button( c.Data.Enabled ? ICON_MDI_PAUSE "  Pause" : ICON_MDI_PLAY "  Play",
                                    ImVec2( w, 0.0f ) ) )
                    c.Data.Enabled = !c.Data.Enabled;
                ImGui::SameLine();
                if ( ImGui::Button( ICON_MDI_RESTART "  Restart", ImVec2( -1.0f, 0.0f ) ) )
                    c.RequestRestart = true;
                ::Desert::Editor::Utils::ImGuiUtilities::Tooltip(
                     "Kill every live particle and start emitting from scratch" );

                // OPEN THE EMITTER'S OWN EDITOR, ON THIS ENTITY. The Particle Editor used to be a window
                // in the View menu that drew whatever was selected, and its own RequestOpen inbox had no
                // callers at all — there was no button here because a button could only have said "reveal
                // that window", never "edit THIS emitter". A document is asked for by subject, so now it
                // can. Open-or-focus falls out of the subject: pressing it twice brings the window that is
                // already on this emitter forward rather than making a second one.
                if ( ImGui::Button( ICON_MDI_CREATION "  Open in Particle Editor", ImVec2( -1.0f, 0.0f ) ) )
                {
                    ::Desert::Editor::Core::SubjectOpenRequests::Request(
                         ::Desert::Editor::ParticleEditorPanel::SubjectFor( ::Desert::Editor::EntityId( en ) ) );
                }
                ::Desert::Editor::Utils::ImGuiUtilities::Tooltip(
                     "Presets, the colour-over-life gradient and the size curve, in a window of their own" );
                ImGui::Spacing();
            }

            PropertyEditorBuilder::Draw( &c.Data, "ParticleEmitterData", ctx.AssetMgr(), ctx.UIHelper,
                                         ctx.FieldFilter );
        };
        return e;
    }

    // Creates a `.demat` already set to the Volume-domain cloud shader and registers its shell — the cloud
    // twin of CreateLandscapeMaterial above, on the same argument: there is exactly ONE program of domain
    // Volume (CloudRaymarch, whose Properties block IS the cloud material schema), so a picker would be a
    // control with a single entry. Pressing New is how you say "a cloud material".
    static ::Desert::Assets::AssetHandle CreateCloudMaterial( const std::string&              entityName,
                                                              ::Desert::Assets::AssetManager* assetMgr )
    {
        if ( !assetMgr )
            return ::Desert::Assets::AssetHandle( static_cast<uint64_t>( 0 ) );

        std::string base;
        base.reserve( entityName.size() + 8 );
        for ( const char c : entityName )
            base += ( std::isalnum( static_cast<unsigned char>( c ) ) || c == '_' || c == '-' ) ? c : '_';
        base = "M_" + ( base.empty() ? std::string( "Clouds" ) : base ) + "_Clouds";

        // Explicit construction, not assignment: the extensions became constexpr string_views (А5), and
        // string_view -> string is deliberately not implicit. Spelled the same way as the landscape
        // twin so the two read as one idiom.
        const std::string           ext( ::Common::Constants::Extensions::MATERIAL_EXTENSION );
        const std::filesystem::path dir = ::Common::Constants::Path::MATERIAL_PATH;
        std::error_code             ec;
        std::filesystem::create_directories( dir, ec );

        std::filesystem::path path = dir / ( base + ext );
        for ( int n = 1; std::filesystem::exists( path, ec ); ++n )
            path = dir / ( base + "_" + std::to_string( n ) + ext );

        // Write the file FIRST (cloud shader + a freshly stamped MaterialId), then create-with-load — the
        // same order CreateLandscapeMaterial documents, and for the same handle-adoption reason.
        {
            ::Desert::Assets::MaterialData data;
            data.ShaderName = ::Desert::Graphic::kCloudMaterialShaderName;
            data.MaterialId = ::Common::UUID::Generate();
            // Checked for the same reason CreateLandscapeMaterial checks it, one function above.
            if ( const auto written = ::Common::Utils::FileSystem::WriteContentToFileAtomic(
                      path.generic_string(), rfl::json::write( data ) );
                 !written )
            {
                LOG_ERROR( "[Clouds] could not write the cloud material '{}': {} — the layer's material "
                           "slot is unchanged.",
                           path.generic_string(), written.GetError() );
                return ::Desert::Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
            }
        }

        auto asset = assetMgr->CreateAsset<::Desert::Assets::SurfaceMaterialAsset>(
             ::Desert::Assets::AssetPriority::High, path.generic_string() );
        if ( !asset )
        {
            LOG_ERROR( "[Clouds] could not create a cloud material at '{}' — the layer's material slot is "
                       "unchanged.",
                       path.generic_string() );
            return ::Desert::Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
        }

        // The SHELL only, exactly as the landscape registers: a cloud material never becomes a runtime
        // Graphic::Material — VolumetricCloudRenderer asks MaterialService for its VALUES
        // (ResolveOverrides) and packs them itself.
        if ( auto* materialService = ::Desert::Runtime::ResourceRegistry::GetMaterialService() )
            materialService->RegisterAsset( asset );
        return asset->GetMetadata().Handle;
    }

    // The cloud layer's MATERIAL row — the landscape row's twin (O1). One handle, drag a `.demat`, New
    // authors one on the cloud shader, Edit opens the Material Editor window; nothing edits a material
    // here. An EMPTY slot is a working sky: the CloudRaymarch schema's own defaults.
    static void DrawCloudMaterialRow( ::Desert::ECS::VolumetricCloudData& cloud, const std::string& entityName,
                                      ::Desert::Assets::AssetManager* assetMgr, const bool allowPanelJumps )
    {
        namespace ImGui = ::ImGui;

        const ::Desert::Assets::SurfaceMaterialAsset* asset = nullptr;
        if ( assetMgr && static_cast<uint64_t>( cloud.Material ) != 0 )
        {
            asset = assetMgr
                         ->FindByHandle<::Desert::Assets::SurfaceMaterialAsset>(
                              ::Common::UUID( static_cast<uint64_t>( cloud.Material ) ) )
                         .get();
        }

        // Empty means the schema defaults and SAYS so — for every other slot "None" reads as a hole, but
        // an unauthored sky is the shipped state of most scenes in the repository.
        std::string display = "Default (schema)";
        if ( static_cast<uint64_t>( cloud.Material ) != 0 )
        {
            display = asset ? std::filesystem::path( asset->GetMetadata().Filepath.string() ).stem().string()
                            : "(missing)";
        }

        if ( !ImGui::BeginTable( "##cloud_mat", 2,
                                 ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings ) )
            return;
        ImGui::TableSetupColumn( "label", ImGuiTableColumnFlags_WidthStretch, 0.38f );
        ImGui::TableSetupColumn( "control", ImGuiTableColumnFlags_WidthStretch, 0.62f );

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( "Material" );
        ImGui::TableNextColumn();

        ImGui::PushItemWidth( -FLT_MIN );
        ImGui::Button( ( display + "##cloud_mat_slot" ).c_str(), ImVec2( -FLT_MIN, 0.0f ) );
        ImGui::PopItemWidth();
        if ( ImGui::BeginDragDropTarget() )
        {
            if ( const ImGuiPayload* pl =
                      ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::MaterialAsset ) )
            {
                const std::string path( static_cast<const char*>( pl->Data ),
                                        pl->DataSize > 0 ? pl->DataSize - 1 : 0 );
                if ( assetMgr && !path.empty() )
                {
                    auto dropped = assetMgr->FindByPath<::Desert::Assets::SurfaceMaterialAsset>( path );
                    if ( !dropped )
                    {
                        dropped = assetMgr->CreateAsset<::Desert::Assets::SurfaceMaterialAsset>(
                             ::Desert::Assets::AssetPriority::High, path );
                        if ( dropped && !dropped->IsReadyForUse() )
                            dropped->Load();
                    }
                    if ( dropped )
                    {
                        // The shell only: the layer asks MaterialService for this material's VALUES.
                        if ( auto* materialService = ::Desert::Runtime::ResourceRegistry::GetMaterialService() )
                            materialService->RegisterAsset( dropped );
                        cloud.Material = dropped->GetMetadata().Handle;
                    }
                    else
                    {
                        LOG_ERROR( "[Clouds] '{}' could not be opened as a material — the layer's material "
                                   "slot is unchanged.",
                                   path );
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "The cloud LOOK — species, weather, placement, layout, detail, lighting — "
                               "authored as a material. Drag a .demat here, or press New to author one on "
                               "the cloud shader. Empty renders the schema defaults." );

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        if ( static_cast<uint64_t>( cloud.Material ) == 0 )
        {
            if ( ImGui::Button( "New Cloud Material", ImVec2( -FLT_MIN, 0.0f ) ) )
            {
                const auto created = CreateCloudMaterial( entityName, assetMgr );
                if ( static_cast<uint64_t>( created ) != 0 )
                {
                    cloud.Material = created;
                    OpenMaterialEditorFor( created, assetMgr, "[Clouds]" );
                }
            }
        }
        else
        {
            // ROUTE B OF THE ROUND TRIP: the material slot is stage 2 of the Clouds window, and stage 2 is
            // the hub the other four hang off. Edit opens the material's own document window as it always
            // did; "In Clouds" opens the SAME document embedded in the Clouds window, beside the layout,
            // the types and the noise it names. Two routes to one document, never two documents — the one
            // instance belongs to Editor/Core/OpenDocuments.hpp.
            const int   buttons = allowPanelJumps ? 3 : 2;
            const float width   = ( ImGui::GetContentRegionAvail().x -
                                  ImGui::GetStyle().ItemSpacing.x * static_cast<float>( buttons - 1 ) ) /
                                static_cast<float>( buttons );
            if ( ImGui::Button( "Edit", ImVec2( width, 0.0f ) ) )
                OpenMaterialEditorFor( cloud.Material, assetMgr, "[Clouds]" );
            if ( allowPanelJumps )
            {
                ImGui::SameLine();
                if ( ImGui::Button( "In Clouds", ImVec2( width, 0.0f ) ) )
                    ::Desert::Editor::CloudsPanel::OpenAt( ::Desert::Editor::CloudStage::Material );
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "Open this material as stage 2 of the Clouds window, beside the "
                                       "layout, the cloud types and the noise volume it names." );
            }
            ImGui::SameLine();
            if ( ImGui::Button( "Clear", ImVec2( width, 0.0f ) ) )
                cloud.Material = ::Desert::Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
        }
        ImGui::EndTable();
    }

    // The cloud layer: reflected budget/routing fields, then the material row — the landscape arrangement.
    static ComponentEditorEntry MakeVolumetricCloudEntry()
    {
        ComponentEditorEntry e;
        // The name is a REGISTRY KEY now, not just a caption: the Clouds window's first stage looks this
        // entry up by it so that the layer's fields are drawn by the same code Details runs rather than by
        // a second copy of them. Hence one constant with two readers.
        e.Name      = kVolumetricCloudComponentEditor;
        e.CanRemove = true;
        e.Has       = []( ::Desert::ECS::Entity& en )
        { return en.HasComponent<::Desert::ECS::VolumetricCloudComponent>(); };
        e.Add    = []( ::Desert::ECS::Entity& en ) { en.AddComponent<::Desert::ECS::VolumetricCloudComponent>(); };
        e.Remove = []( ::Desert::ECS::Entity& en )
        { en.RemoveComponent<::Desert::ECS::VolumetricCloudComponent>(); };
        e.Draw = []( ::Desert::ECS::Entity& en, ::Desert::Core::Scene*, const ComponentEditContext& ctx )
        {
            auto& c = en.GetComponent<::Desert::ECS::VolumetricCloudComponent>();

            // ROUTE A OF THE ROUND TRIP the owner asked for: from the component to the window that shows
            // the whole sky, landing on stage 1 — which IS this component, so the landing is not arbitrary.
            //
            // ONLY TWO OF THE SIX STAGES ARE REACHABLE FROM HERE, and that is a property of the O1 split
            // rather than a shortfall: the layout, the four cloud types and the noise volume are MATERIAL
            // parameters now, and this component names exactly one asset — its material. A button here that
            // opened stage 5 would have to invent a noise volume the entity does not name.
            if ( ctx.AllowPanelJumps )
            {
                if ( ::ImGui::Button( ICON_MDI_WEATHER_CLOUDY "  Open in Clouds", ImVec2( -FLT_MIN, 0.0f ) ) )
                    ::Desert::Editor::CloudsPanel::OpenAt( ::Desert::Editor::CloudStage::Layer );
                if ( ::ImGui::IsItemHovered() )
                    ::ImGui::SetTooltip( "One window for the whole sky: this layer, its material, the "
                                         "painted layout, the cloud types, the noise they are cut from and "
                                         "the hero bodies \xe2\x80\x94 in the order the sky is built." );
                ::ImGui::Separator();
            }

            PropertyEditorBuilder::Draw( &c.Data, "VolumetricCloudData", ctx.AssetMgr(), ctx.UIHelper );

            ::ImGui::Separator();
            DrawCloudMaterialRow( c.Data, en.GetComponent<::Desert::ECS::TagComponent>().Tag, ctx.AssetMgr(),
                                  ctx.AllowPanelJumps );
        };
        return e;
    }

    // Landscape Material: the reflected LandscapeMaterialData UI (the three layer-mode combos), plus the
    // material ROW in the same section — one asset field, an Edit button that opens the Material Editor
    // window, and nothing that edits a material here. The handle itself is `PROPERTY Hidden` in the
    // reflection, because the builder's generic asset slot is texture-oriented; the row below is its UI.
    static ComponentEditorEntry MakeLandscapeMaterialEntry()
    {
        using ::Desert::ECS::LandscapeMaterialComponent;
        ComponentEditorEntry e;
        e.Name      = "Landscape Material";
        e.CanRemove = true;
        e.Has       = []( ::Desert::ECS::Entity& en ) { return en.HasComponent<LandscapeMaterialComponent>(); };
        e.Add       = []( ::Desert::ECS::Entity& en ) { en.AddComponent<LandscapeMaterialComponent>(); };
        e.Remove    = []( ::Desert::ECS::Entity& en ) { en.RemoveComponent<LandscapeMaterialComponent>(); };
        e.Draw      = []( ::Desert::ECS::Entity& en, ::Desert::Core::Scene*, const ComponentEditContext& ctx )
        {
            auto& c = en.GetComponent<LandscapeMaterialComponent>();
            PropertyEditorBuilder::Draw( &c.Data, "LandscapeMaterialData", ctx.AssetMgr(), ctx.UIHelper );

            ::ImGui::Separator();
            DrawLandscapeMaterialRow( c.Data, en.GetComponent<::Desert::ECS::TagComponent>().Tag, ctx.AssetMgr() );
        };
        return e;
    }

    static ComponentEditorEntry MakeColliderEntry()
    {
        ComponentEditorEntry e;
        e.Name      = "Collider";
        e.CanRemove = true;
        e.Has = []( ::Desert::ECS::Entity& en ) { return en.HasComponent<::Desert::ECS::ColliderComponent>(); };
        e.Add = []( ::Desert::ECS::Entity& en )
        {
            auto& c = en.AddComponent<::Desert::ECS::ColliderComponent>();
            FitColliderToMesh( en, c.Data );
        };
        e.Remove = []( ::Desert::ECS::Entity& en ) { en.RemoveComponent<::Desert::ECS::ColliderComponent>(); };
        e.Draw   = []( ::Desert::ECS::Entity& en, ::Desert::Core::Scene*, const ComponentEditContext& ctx )
        {
            auto& c = en.GetComponent<::Desert::ECS::ColliderComponent>();
            PropertyEditorBuilder::Draw( &c.Data, "ColliderData", ctx.AssetMgr(), ctx.UIHelper, ctx.FieldFilter );

            // A collider that disagrees with the mesh it is supposed to wrap is invisible until something
            // walks into thin air — the greybox house shipped with double-size colliders for exactly this
            // reason (see the world-units commit). Say it here, next to the button that fixes it.
            if ( !ctx.FieldFilter )
            {
                if ( const auto meshHalf = MeshHalfExtents( en ) )
                {
                    const glm::vec3 colliderHalf =
                         c.Data.Shape == ::Desert::Physics::ShapeType::Box
                              ? c.Data.HalfExtents
                              : glm::vec3( c.Data.Radius,
                                           c.Data.Shape == ::Desert::Physics::ShapeType::Capsule
                                                ? c.Data.HalfHeight + c.Data.Radius
                                                : c.Data.Radius,
                                           c.Data.Radius );

                    // Relative on purpose: 5 cm matters on a doorknob and not on a hillside.
                    const glm::vec3 ref   = glm::max( *meshHalf, glm::vec3( 1.0f ) );
                    const glm::vec3 delta = glm::abs( colliderHalf - *meshHalf ) / ref;
                    const float     worst = glm::max( delta.x, glm::max( delta.y, delta.z ) );
                    if ( worst > 0.25f )
                    {
                        ImGui::PushStyleColor( ImGuiCol_Text, ::Desert::Editor::ThemeManager::GetWarningColor() );
                        ImGui::TextWrapped( ICON_MDI_ALERT " Collision is %.0f%% off the mesh bounds "
                                                           "(mesh half-extents %.0f x %.0f x %.0f cm)",
                                            worst * 100.0f, meshHalf->x, meshHalf->y, meshHalf->z );
                        ImGui::PopStyleColor();
                    }
                }
            }

            if ( ::ImGui::Button( "Fit to Mesh Bounds", ImVec2( -1.0f, 0.0f ) ) )
                FitColliderToMesh( en, c.Data );
        };
        return e;
    }

    // UI Canvas: the reflected fields, plus the way into the window that authors this canvas.
    // ── UI STYLE ────────────────────────────────────────────────────────────────────────────────────
    //
    // WHY THIS IS A CUSTOM ENTRY AND NOT THE REFLECTED ONE-LINER. The two fields are the easy half; the
    // half that had to exist is the TABLE UNDER THEM, which says, slot by slot, WHERE the value the
    // element is drawn with came from — the theme (and through which token) or the element's own field.
    //
    // That table is the answer to "a colour set on the element and a colour set by the theme: who wins,
    // and can a person see it". The honest answer is that neither wins, because a slot has exactly one
    // source; but an answer nobody can see is indistinguishable from the silent one. This is where it is
    // seen. The engine's recurring defect is precisely two plausible values with no way to tell which
    // one reached the screen, and a table that prints the source per slot is the cheapest possible cure.
    static ComponentEditorEntry MakeUIStyleEntry()
    {
        using C = ::Desert::ECS::UIStyleComponent;
        ComponentEditorEntry e;
        e.Name              = "UI Style";
        e.CanRemove         = true;
        e.ReflectedTypeName = "UIStyleData";
        e.Has               = []( ::Desert::ECS::Entity& en ) { return en.HasComponent<C>(); };
        e.Add               = []( ::Desert::ECS::Entity& en ) { en.AddComponent<C>(); };
        e.Remove            = []( ::Desert::ECS::Entity& en ) { en.RemoveComponent<C>(); };
        e.DataPtr           = []( ::Desert::ECS::Entity& en ) -> void* { return &en.GetComponent<C>().Data; };
        e.Draw = []( ::Desert::ECS::Entity& en, ::Desert::Core::Scene*, const ComponentEditContext& ctx )
        {
            auto& c = en.GetComponent<C>();
            PropertyEditorBuilder::Draw( &c.Data, "UIStyleData", ctx.AssetMgr(), ctx.UIHelper, ctx.FieldFilter );

            if ( ctx.FieldFilter )
                return; // while searching, only the matched fields are on screen

            entt::registry* reg = en.GetRegistry();
            if ( reg == nullptr )
                return;

            // The theme is the CANVAS's, found from this element the only way the engine allows — by its
            // canvas ancestor. An element outside a canvas has no theme and the panel says so rather than
            // drawing an empty table, because "no theme" and "a theme that binds nothing" look identical.
            const entt::entity canvas = ::Desert::UI::CanvasOf( *reg, en.GetHandle() );
            const auto*        theme = canvas != entt::null && reg->has<::Desert::ECS::UICanvasComponent>( canvas )
                                            ? ::Desert::Runtime::ResourceRegistry::GetUIThemeService()->Get(
                                            reg->get<::Desert::ECS::UICanvasComponent>( canvas ).Data.Theme )
                                            : nullptr;

            ImGui::Spacing();
            if ( theme == nullptr )
            {
                ImGui::TextDisabled( "This element's canvas has no theme, so every slot below is this "
                                     "element's own value." );
                return;
            }

            bool                             unknown = false;
            const ::Desert::UI::CanvasStyle  canvasStyle( theme, 1.0f, false );
            const ::Desert::UI::ElementStyle style = canvasStyle.For( c.Data.Style, unknown );

            if ( unknown )
            {
                ImGui::TextColored( ThemeManager::GetErrorColor(), "%s",
                                    ( "The theme \"" + theme->Name + "\" declares no style \"" + c.Data.Style +
                                      "\" — every slot below is local." )
                                         .c_str() );
                return;
            }
            if ( c.Data.Source == ::Desert::ECS::UIStyleSource::Local )
            {
                ImGui::TextDisabled( "Source is Local, so the theme is not consulted: every slot below is "
                                     "this element\'s own value." );
            }

            // WHICH SLOTS ARE THIS ELEMENT'S. The register names slots "<Element>.<Slot>"; the rows below
            // pair a prefix with the component that draws it, so a panel sees the panel's slots and an
            // element that is both a panel and a text block sees both sets. A prefix with no row here
            // simply does not appear — which is a row missing from a TABLE OF WHAT TO SHOW, not a slot
            // going unread; Desert/Tests/Engine/UIStyle is what guards the second thing.
            const auto ElementHasPrefix = [&en]( std::string_view prefix )
            {
                using namespace ::Desert::ECS;
                if ( prefix == "Panel" )
                    return en.HasComponent<UIPanelComponent>();
                if ( prefix == "Button" )
                    return en.HasComponent<UIButtonComponent>();
                if ( prefix == "Text" )
                    return en.HasComponent<UITextComponent2D>();
                if ( prefix == "Icon" )
                    return en.HasComponent<UIIconComponent>();
                if ( prefix == "Image" )
                    return en.HasComponent<UIImageComponent>();
                if ( prefix == "Progress" )
                    return en.HasComponent<UIProgressBarComponent>();
                if ( prefix == "Toggle" )
                    return en.HasComponent<UIToggleComponent>();
                if ( prefix == "Slider" )
                    return en.HasComponent<UISliderComponent>();
                // ONE PREFIX, TWO CONTAINERS. The slot names a LOOK — the background and the thumb of a
                // scrolling container — and a theme has one of those, so UIListView draws through the same
                // two tokens rather than through a second pair the author would have to keep identical.
                if ( prefix == "ScrollView" )
                    return en.HasComponent<UIScrollViewComponent>() || en.HasComponent<UIListViewComponent>();
                if ( prefix == "Input" )
                    return en.HasComponent<UIInputFieldComponent>();
                if ( prefix == "Dropdown" )
                    return en.HasComponent<UIDropdownComponent>();
                if ( prefix == "DropTarget" )
                    return en.HasComponent<UIDropTargetComponent>();
                if ( prefix == "LayoutGroup" )
                    return en.HasComponent<UILayoutGroupComponent>();
                return false;
            };

            if ( !ImGui::BeginTable( "##uistyleslots", 3,
                                     ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp ) )
                return;

            ImGui::TableSetupColumn( "Slot" );
            ImGui::TableSetupColumn( "Source" );
            ImGui::TableSetupColumn( "Value" );
            ImGui::TableHeadersRow();

            for ( std::size_t i = 0; i < ::Desert::UI::kStyleSlotCount; ++i )
            {
                const auto             slot = static_cast<::Desert::UI::StyleSlot>( i );
                const std::string_view name = ::Desert::UI::StyleSlotName( slot );
                const std::string_view prefix( name.data(), name.find( '.' ) );
                if ( !ElementHasPrefix( prefix ) )
                    continue;

                const bool themed = c.Data.Source == ::Desert::ECS::UIStyleSource::Theme && style.IsThemed( slot );

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted( name.data(), name.data() + name.size() );

                ImGui::TableNextColumn();
                if ( themed )
                    ImGui::TextColored( ThemeManager::GetHighlightColor(), "Theme" );
                else
                    ImGui::TextDisabled( "Local" );

                ImGui::TableNextColumn();
                switch ( ::Desert::UI::StyleSlotKindOf( slot ) )
                {
                    case ::Desert::UI::StyleSlotKind::Color:
                    {
                        const glm::vec3 v = style.Color( slot, glm::vec3( 0.5f ) );
                        ImGui::ColorButton( name.data(), ImVec4( v.r, v.g, v.b, 1.0f ),
                                            ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
                                            ImVec2( 40.0f, 0.0f ) );
                        break;
                    }
                    case ::Desert::UI::StyleSlotKind::Metric:
                        ImGui::Text( "%.1f px", style.Metric( slot, 0.0f ) );
                        break;
                    case ::Desert::UI::StyleSlotKind::Font:
                        ImGui::Text( "%.1f px", style.FontSize( slot, 0.0f ) );
                        break;
                }
            }
            ImGui::EndTable();

            ::Desert::Editor::Utils::ImGuiUtilities::Tooltip(
                 "A slot the style binds comes from the theme; every other slot is this element's own "
                 "field. Nothing overrides anything — a slot has exactly one source, and this is it." );
        };
        return e;
    }

    static ComponentEditorEntry MakeUICanvasEntry()
    {
        using C = ::Desert::ECS::UICanvasComponent;
        ComponentEditorEntry e;
        e.Name              = "UI Canvas";
        e.CanRemove         = true;
        e.ReflectedTypeName = "UICanvasData";
        e.Has               = []( ::Desert::ECS::Entity& en ) { return en.HasComponent<C>(); };
        e.Add               = []( ::Desert::ECS::Entity& en ) { en.AddComponent<C>(); };
        e.Remove            = []( ::Desert::ECS::Entity& en ) { en.RemoveComponent<C>(); };
        e.DataPtr           = []( ::Desert::ECS::Entity& en ) -> void* { return &en.GetComponent<C>().Data; };
        e.Draw = []( ::Desert::ECS::Entity& en, ::Desert::Core::Scene*, const ComponentEditContext& ctx )
        {
            auto& c = en.GetComponent<C>();

            if ( !ctx.FieldFilter ) // while searching, only the matched fields are on screen
            {
                // ONE CANVAS, NAMED. The button sends a SUBJECT (this entity's UICanvasComponent), so a
                // scene with two canvases has two windows and each is about the one it was opened from. The
                // UI Editor used to be a singleton tool over the FIRST canvas in the registry, and with one
                // window there was nothing a button could say — which is why there was no button.
                // Open-or-focus falls out of the subject: pressing it twice brings the window that is
                // already on this canvas forward rather than making a second one.
                if ( ImGui::Button( ICON_MDI_VIEW_DASHBOARD "  Open in UI Editor", ImVec2( -FLT_MIN, 0.0f ) ) )
                {
                    ::Desert::Editor::Core::SubjectOpenRequests::Request(
                         ::Desert::Editor::UIEditorPanel::SubjectFor( ::Desert::Editor::EntityId( en ) ) );
                }
                ::Desert::Editor::Utils::ImGuiUtilities::Tooltip(
                     "Place elements on this canvas and preview it at its design resolution, in a window of "
                     "its own" );
                ImGui::Spacing();
            }

            PropertyEditorBuilder::Draw( &c.Data, "UICanvasData", ctx.AssetMgr(), ctx.UIHelper, ctx.FieldFilter );
        };
        return e;
    }

    // UI Layout (RectTransform): anchor-preset controls ("Fill / Match Parent" + 4x4 grid) on top of the
    // reflected anchor/offset fields, so you can match the parent from the inspector (not just the
    // viewport toolbar). Presets act in design space (keep the authored size; stretch fills the axis).
    static ComponentEditorEntry MakeUILayoutEntry()
    {
        using C = ::Desert::ECS::UILayoutComponent;
        ComponentEditorEntry e;
        e.Name      = "UI Layout";
        e.CanRemove = true;
        e.Has       = []( ::Desert::ECS::Entity& en ) { return en.HasComponent<C>(); };
        e.Add       = []( ::Desert::ECS::Entity& en ) { en.AddComponent<C>(); };
        e.Remove    = []( ::Desert::ECS::Entity& en ) { en.RemoveComponent<C>(); };
        e.Draw      = []( ::Desert::ECS::Entity& en, ::Desert::Core::Scene*, const ComponentEditContext& ctx )
        {
            auto& c = en.GetComponent<C>();
            UIAnchors::DrawControls( c.Data );
            PropertyEditorBuilder::Draw( &c.Data, "UILayoutData", ctx.AssetMgr(), ctx.UIHelper );

            // ── THE ELEMENT'S PROPERTY TIMELINE ────────────────────────────────────────────────────────
            //
            // CREATE-THEN-OPEN when there is no clip yet, the way the landscape and cloud material rows
            // create a `.demat` and open it in one press. The Sequencer's UI half is a document over the
            // UIAnimComponent, so an element without one has no subject to open — the old "Add UI
            // Animation" button lived INSIDE that window, which meant the window had to be able to exist
            // over nothing. It cannot any more, so the button moved to where the thing is made.
            if ( ctx.FieldFilter )
                return; // while searching, only the matched fields are on screen

            const bool hasClip = en.HasComponent<::Desert::ECS::UIAnimComponent>();
            ImGui::Spacing();
            if ( ImGui::Button( hasClip ? ICON_MDI_CHART_TIMELINE_VARIANT "  Open in Sequencer"
                                        : ICON_MDI_PLUS "  Add UI Animation",
                                ImVec2( -FLT_MIN, 0.0f ) ) )
            {
                if ( !hasClip )
                    en.AddComponent<::Desert::ECS::UIAnimComponent>();
                ::Desert::Editor::Core::SubjectOpenRequests::Request(
                     ::Desert::Editor::SequencerPanel::UISubjectFor( ::Desert::Editor::EntityId( en ) ) );
            }
            ::Desert::Editor::Utils::ImGuiUtilities::Tooltip(
                 hasClip ? "Key this element's Offset / Size / Opacity / Color over time"
                         : "Add a clip that keys Offset / Size / Opacity / Color over time, and open it" );
        };
        return e;
    }
    // ============================================================================================
    // INSTANCED STATIC MESH — the panel that makes one entity able to stand for five hundred props.
    //
    // WHY THIS IS A LEVER AND NOT A COSMETIC. Measured on the world-scale scene (Tools/WorldGen,
    // 50 179 entities): the ECS mesh walk costs ~55 ms a frame against 22.6 ms of GPU, because it
    // visits every entity. Auto-batching already folds those draws into ~25 — it saves DRAW CALLS,
    // and the walk has already happened by the time it runs. An ISM is the other saving: N repeated
    // props become ONE entity carrying N world matrices, so the walk visits one.
    //
    // WHAT IS DELIBERATELY NOT DRAWN HERE, so the next reader does not take it for forgetfulness:
    //   * per-instance custom data — the instanced shader reads gl_InstanceIndex into the transform
    //     SSBO and nothing else; there is no consumer, so the field would be a knob moving nothing;
    //   * cull distances — culling in this engine is frustum-only (MeshRenderer::IsVisibleInView);
    //     there is no distance test to feed;
    //   * per-instance collision presets — Jolt is wired for box, sphere and capsule on a Collider
    //     COMPONENT, which is per entity; an ISM has one entity for N instances and no per-instance
    //     body exists to configure.
    // Each of the three would be a row that writes a field nobody reads. §1.3 of the contract.
    // ============================================================================================
    static ComponentEditorEntry MakeInstancedStaticMeshEntry()
    {
        using ISMC   = ::Desert::ECS::InstancedStaticMeshComponent;
        namespace GG = ::Desert::Geometry;

        ComponentEditorEntry e;
        e.Name      = "Instanced Static Mesh";
        e.CanRemove = true;
        e.Has       = []( ::Desert::ECS::Entity& en ) { return en.HasComponent<ISMC>(); };
        e.Add       = []( ::Desert::ECS::Entity& en )
        {
            auto& c     = en.AddComponent<ISMC>();
            c.Primitive = GG::PrimitiveType::Cube; // renders immediately
            if ( c.InstanceTransforms.empty() )
                c.InstanceTransforms.emplace_back( 1.0f );
        };
        e.Remove = []( ::Desert::ECS::Entity& en ) { en.RemoveComponent<ISMC>(); };
        e.Draw   = []( ::Desert::ECS::Entity& en, ::Desert::Core::Scene* scene, const ComponentEditContext& ctx )
        {
            namespace U = ::Desert::Editor::Utils;
            auto& c     = en.GetComponent<ISMC>();

            // ── MESH SOURCE ───────────────────────────────────────────────────────────────────────
            U::ImGuiUtilities::ResetPropertyRows();

            if ( c.MeshHandle.IsNull() )
            {
                // OFFERED LIST AND STORED VALUE BOTH FROM Geometry::kAuthorablePrimitives. The hand-typed
                // list that stood here was { "Cube", "Sphere", "Plane", "Pyramid" } against an enum that
                // runs Cube, Sphere, Pyramid, Plane — so picking "Plane" built a pyramid, and Cylinder and
                // Capsule could not be picked at all. Nothing in a frame says which enumerator a combo
                // meant, which is how that lived for as long as this editor has.
                std::array<const char*, GG::kAuthorablePrimitives.size()> shapes{};
                int                                                       current = 0;
                for ( size_t i = 0; i < GG::kAuthorablePrimitives.size(); ++i )
                {
                    shapes[i] = GG::PrimitiveTypeName( GG::kAuthorablePrimitives[i] );
                    if ( c.Primitive.has_value() && GG::kAuthorablePrimitives[i] == *c.Primitive )
                        current = static_cast<int>( i );
                }
                U::ImGuiUtilities::BeginPropertyRow( "Shape", "The built-in mesh every instance draws" );
                if ( ::ImGui::Combo( "##ism_shape", &current, shapes.data(), static_cast<int>( shapes.size() ) ) )
                {
                    c.Primitive = GG::kAuthorablePrimitives[static_cast<size_t>( current )];
                    c.RuntimeMesh.reset();
                }
                U::ImGuiUtilities::EndPropertyRow();
            }

            U::ImGuiUtilities::BeginPropertyRow( "Mesh", "Drop a .stmesh here to instance an asset mesh" );
            ::ImGui::Button( c.MeshHandle.IsNull() ? "Drop a .stmesh to use an asset mesh"
                                                   : "Mesh: <asset> (drop to replace)",
                             ImVec2( -1.0f, 0.0f ) );
            if ( ::ImGui::BeginDragDropTarget() )
            {
                if ( const ImGuiPayload* pl =
                          ::ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::MeshAsset );
                     pl && ctx.AssetMgr() )
                {
                    const std::string path( static_cast<const char*>( pl->Data ),
                                            pl->DataSize > 0 ? pl->DataSize - 1 : 0 );
                    const auto        handle = ::Desert::Editor::MeshDnD::ResolveOrImport( *ctx.AssetMgr(), path );
                    if ( !handle.IsNull() )
                    {
                        c.MeshHandle = handle;
                        c.Primitive.reset();
                        c.RuntimeMesh.reset();
                    }
                }
                ::ImGui::EndDragDropTarget();
            }
            U::ImGuiUtilities::EndPropertyRow();

            if ( !c.MeshHandle.IsNull() && ::ImGui::SmallButton( "Use Primitive Instead" ) )
            {
                c.MeshHandle = {};
                c.Primitive  = GG::PrimitiveType::Cube;
                c.RuntimeMesh.reset();
            }

            // ── MATERIAL SLOTS ────────────────────────────────────────────────────────────────────
            //
            // The SAME widget the static and skinned meshes use, reached through MaterialHost — which
            // was built as "a view rather than a component type" for exactly this. An ISM's slots feed
            // MeshECSSystem's RuntimeMaterialInstances identically; until this call existed they could
            // only be written by editing the scene file.
            {
                static ::Desert::Editor::MaterialComponentWidget s_InstancedMaterials( ctx.AssetMgr() );
                s_InstancedMaterials.Render( en, scene );
            }

            // ── WHAT IT COSTS, COMPUTED ───────────────────────────────────────────────────────────
            //
            // EVERY NUMBER HERE IS DERIVED FROM THE THING IT DESCRIBES, never from the frame counters.
            // DrawCounter::LastFrame() answers for the WHOLE frame; a per-component panel quoting it
            // would be an instrument answering a different question than the one asked — the shape this
            // project keeps finding. The instance count is the array's size, the triangle count is the
            // mesh's own index counts, and the draw figure is the batch rule stated as a rule.
            const size_t instanceCount = c.InstanceTransforms.size();

            // The renderer's own order of preference (MeshECSSystem): the in-editor mesh, then the
            // asset, then the built-in shape. Spelled as statements rather than three nested conditional
            // operators, which is both what the analyser asks for and what makes the order readable.
            ::Desert::Mesh* mesh = nullptr;
            if ( c.RuntimeMesh )
                mesh = static_cast<::Desert::Mesh*>( c.RuntimeMesh.get() );
            else if ( !c.MeshHandle.IsNull() )
                mesh = ::Desert::Runtime::ResourceRegistry::GetMeshService()->Get( c.MeshHandle );
            else if ( c.Primitive.has_value() )
                mesh = GG::PrimitiveMeshFactory::GetShared( *c.Primitive );
            // AND THE LOD CHAIN IS EMPTY FOR EXACTLY THE MESHES AN ISM USUALLY HOLDS. Submesh::LODs says
            // so in its own header — "empty for meshes with no LODs (procedural / skinned)" — and the
            // first version of this readout asked only the chain, so every primitive ISM reported
            // "0 (0 each)" triangles while drawing nine cubes. A count that is silently zero reads as
            // "this is free", which is the opposite of what the row is here to say. The submesh's own
            // IndexCount is the geometry either way; LODs[0] is byte-identical to it when it exists.
            uint64_t trianglesEach = 0;
            if ( mesh != nullptr )
                for ( const auto& submesh : mesh->GetSubmeshes() )
                    trianglesEach +=
                         ( submesh.LODs.empty() ? submesh.IndexCount : submesh.LODs.front().IndexCount ) / 3;

            uint32_t cascades = 0;
            if ( c.CastShadows && scene != nullptr && scene->GetSceneRenderer() != nullptr )
                cascades = scene->GetSceneRenderer()->GetShadowCascadeCount();

            if ( U::ImGuiUtilities::SectionHeader( ICON_MDI_CHART_BAR "  Statistics", true ) )
            {
                U::ImGuiUtilities::ResetPropertyRows();

                U::ImGuiUtilities::BeginPropertyRow( "Instances", "World transforms in this component" );
                ::ImGui::Text( "%zu", instanceCount );
                U::ImGuiUtilities::EndPropertyRow();

                U::ImGuiUtilities::BeginPropertyRow( "Triangles", "Instances x the mesh's LOD 0 triangles, before "
                                                                  "per-instance culling and LOD" );
                if ( mesh != nullptr )
                    ::ImGui::Text( "%llu  (%llu each)",
                                   static_cast<unsigned long long>( trianglesEach * instanceCount ),
                                   static_cast<unsigned long long>( trianglesEach ) );
                else
                    ::ImGui::TextDisabled( "mesh not resolved" );
                U::ImGuiUtilities::EndPropertyRow();

                // THE ROW THIS PANEL EXISTS FOR. One batch against one entity apiece, with the cascades
                // counted on both sides — the shadow passes are where the difference is largest, and the
                // measurement that added CastShadows to this component (25/62 draws -> 21/26) is exactly
                // this arithmetic seen from the other end.
                U::ImGuiUtilities::BeginPropertyRow( "Draw calls",
                                                     "One instanced batch per pass, whatever the instance "
                                                     "count; the alternative is one draw per entity" );
                ::ImGui::Text( "%u  vs %zu as separate entities", 1u + cascades,
                               instanceCount * ( 1u + cascades ) );
                U::ImGuiUtilities::EndPropertyRow();

                U::ImGuiUtilities::BeginPropertyRow( "ECS entities", "What the per-frame mesh walk visits" );
                ::ImGui::Text( "1  vs %zu as separate entities", instanceCount );
                U::ImGuiUtilities::EndPropertyRow();
            }

            // ── RENDERING ─────────────────────────────────────────────────────────────────────────
            //
            // CAST SHADOWS is the knob an ISM was the only mesh kind not to have. The cascade pass read
            // StaticMeshComponent::CastShadows and SkinnedMeshComponent::CastShadows and appended every
            // instanced batch unconditionally, so a scattered field of grass — the very thing an ISM is
            // for — could not be taken out of the shadow maps at any price. Same wording and same place
            // as on the other two kinds.
            if ( U::ImGuiUtilities::SectionHeader( ICON_MDI_EYE "  Rendering", false ) )
            {
                U::ImGuiUtilities::ResetPropertyRows();
                U::ImGuiUtilities::BeginPropertyRow( "Cast Shadows",
                                                     "Skip every instance in the shadow (depth) passes" );
                ::ImGui::Checkbox( "##ism_castshadows", &c.CastShadows );
                U::ImGuiUtilities::EndPropertyRow();
            }

            // ── THE INSTANCE LIST ─────────────────────────────────────────────────────────────────
            //
            // WORLD-SPACE, AND THE HEADER SAYS SO because it is the one thing about this component that
            // surprises everybody: MeshECSSystem hands InstanceTransforms to the draw command untouched,
            // so the entity's own transform does NOT move its instances.
            //
            // THE ROWS ARE VIRTUALISED (ImGuiListClipper). A field folded from five hundred props is the
            // normal case, not the extreme one, and five hundred three-vector rows a frame is what turns
            // a panel into the frame's most expensive window.
            //
            // AND A ROW IS WRITTEN BACK ONLY WHEN IT CHANGED. Decompose-then-recompose does not return
            // the matrix it was given (euler angles are not unique, and a scale-carrying matrix comes
            // back a few ULPs away), so re-composing every row every frame would walk the instances off
            // their placements without anybody touching a control.
            if ( U::ImGuiUtilities::SectionHeader( ICON_MDI_FORMAT_LIST_NUMBERED "  Instances (world space)",
                                                   true ) )
            {
                if ( ::ImGui::Button( "Add" ) )
                {
                    // 200 cm, AND THE OLD NUMBER WAS 2. These two buttons stepped by `2.0f`, which is a
                    // metre-era constant left behind by the units migration: one world unit is one
                    // centimetre and PrimitiveMeshFactory scales every built-in shape by
                    // Units::UnitsPerMetre, so the default cube is 100 cm across and an instance placed
                    // 2 cm along sat wholly INSIDE the previous one. Ten by ten of them made one cube.
                    // The button looked broken and was in fact doing exactly what it said.
                    glm::mat4 seed( 1.0f );
                    if ( !c.InstanceTransforms.empty() )
                        seed = glm::translate( c.InstanceTransforms.back(), glm::vec3( 200.0f, 0.0f, 0.0f ) );
                    c.InstanceTransforms.push_back( seed );
                }
                ::ImGui::SameLine();
                if ( ::ImGui::Button( "Add 10x10 Grid" ) )
                {
                    for ( int z = 0; z < 10; ++z )
                        for ( int x = 0; x < 10; ++x )
                            c.InstanceTransforms.push_back( glm::translate(
                                 glm::mat4( 1.0f ), glm::vec3( static_cast<float>( x ) * 200.0f, 0.0f,
                                                               static_cast<float>( z ) * 200.0f ) ) );
                }
                ::ImGui::SameLine();
                if ( ::ImGui::Button( "Clear" ) )
                    c.InstanceTransforms.clear();

                // Deferred so the vector is not resized while the clipper is walking it.
                int duplicateIndex = -1;
                int removeIndex    = -1;

                U::ImGuiUtilities::ResetPropertyRows();
                ImGuiListClipper clipper;
                clipper.Begin( static_cast<int>( c.InstanceTransforms.size() ) );
                while ( clipper.Step() )
                {
                    for ( int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i )
                    {
                        ::ImGui::PushID( i );

                        const auto decomposed = ::Desert::ECS::Rules::DecomposeTransform(
                             c.InstanceTransforms[static_cast<size_t>( i )] );
                        glm::vec3 translation = decomposed.Translation;
                        // `+ 0.0f` turns IEEE negative zero into positive zero: eulerAngles of an
                        // unrotated instance hands back -0.0 on two axes, and a row reading "-0.0" on a
                        // prop nobody has rotated reads as a defect in the decomposition.
                        glm::vec3 degrees = glm::degrees( decomposed.Rotation ) + 0.0f;
                        glm::vec3 scale   = decomposed.Scale;

                        bool              moved = false;
                        const std::string label = "[" + std::to_string( i ) + "]";

                        U::ImGuiUtilities::BeginPropertyRow( label.c_str(), "World position, in centimetres" );
                        moved |= U::ImGuiUtilities::VectorField( "pos", &translation.x, 3, 1.0f, "%.1f" );
                        U::ImGuiUtilities::EndPropertyRow();

                        // Stored as a matrix, shown in degrees like every other rotation in the editor.
                        U::ImGuiUtilities::BeginPropertyRow( "  Rotation", "World rotation, in degrees" );
                        moved |= U::ImGuiUtilities::VectorField( "rot", &degrees.x, 3, 0.5f, "%.1f\xc2\xb0" );
                        U::ImGuiUtilities::EndPropertyRow();

                        U::ImGuiUtilities::BeginPropertyRow( "  Scale", nullptr );
                        moved |= U::ImGuiUtilities::VectorField( "scl", &scale.x, 3, 0.01f, "%.3f" );
                        U::ImGuiUtilities::EndPropertyRow();

                        U::ImGuiUtilities::BeginPropertyRow( "  ", nullptr );
                        if ( ::ImGui::SmallButton( "Duplicate" ) )
                            duplicateIndex = i;
                        ::ImGui::SameLine();
                        if ( ::ImGui::SmallButton( "Delete" ) )
                            removeIndex = i;
                        U::ImGuiUtilities::EndPropertyRow();

                        if ( moved )
                        {
                            c.InstanceTransforms[static_cast<size_t>( i )] =
                                 glm::translate( glm::mat4( 1.0f ), translation ) *
                                 glm::toMat4( glm::quat( glm::radians( degrees ) ) ) *
                                 glm::scale( glm::mat4( 1.0f ), scale );
                        }

                        ::ImGui::PopID();
                    }
                }
                clipper.End();

                if ( duplicateIndex >= 0 )
                {
                    // Offset by the same 200 cm as Add, and for the reason given there: a duplicate that
                    // lands inside its source is a button that appears to do nothing.
                    c.InstanceTransforms.insert(
                         c.InstanceTransforms.begin() + duplicateIndex + 1,
                         glm::translate( c.InstanceTransforms[static_cast<size_t>( duplicateIndex )],
                                         glm::vec3( 200.0f, 0.0f, 0.0f ) ) );
                }
                if ( removeIndex >= 0 )
                    c.InstanceTransforms.erase( c.InstanceTransforms.begin() + removeIndex );
            }
        };
        return e;
    }

    // ============================================================================================
    // Components that had no Details entry at all — their data existed, was serialized and was read by
    // the systems, but could only be authored by editing the scene file. One entry each.
    // ============================================================================================

    // UE's "Sockets ▸ Parent Socket": follow a BONE of another skinned entity (weapon in hand, hat on
    // head). The bone list comes from the TARGET's skeleton, so the name can only ever be one that
    // exists — typing it by hand was the alternative.
    static ComponentEditorEntry MakeSocketEntry()
    {
        using C = ::Desert::ECS::SocketAttachmentComponent;
        ComponentEditorEntry e;
        e.Name      = "Socket";
        e.CanRemove = true;
        e.Has       = []( ::Desert::ECS::Entity& en ) { return en.HasComponent<C>(); };
        e.Add       = []( ::Desert::ECS::Entity& en ) { en.AddComponent<C>(); };
        e.Remove    = []( ::Desert::ECS::Entity& en ) { en.RemoveComponent<C>(); };
        e.Draw      = []( ::Desert::ECS::Entity& en, ::Desert::Core::Scene* scene, const ComponentEditContext& )
        {
            namespace U = ::Desert::Editor::Utils;
            auto& c     = en.GetComponent<C>();

            U::ImGuiUtilities::ResetPropertyRows();

            // --- Target: any OTHER entity carrying a skinned mesh (only those have bones) -------------
            std::string                          targetName   = "None";
            const ::Desert::ECS::Entity*         targetEntity = nullptr;
            std::optional<::Desert::ECS::Entity> targetStorage;
            if ( scene && !c.Target.IsNull() )
            {
                if ( auto ref = scene->FindEntityByID( c.Target ) )
                {
                    targetStorage = ref->get();
                    targetEntity  = &*targetStorage;
                    if ( targetStorage->HasComponent<::Desert::ECS::TagComponent>() )
                        targetName = targetStorage->GetComponent<::Desert::ECS::TagComponent>().Tag;
                }
                else
                {
                    targetName = "<missing entity>";
                }
            }

            U::ImGuiUtilities::BeginPropertyRow( "Target", "The skinned entity whose bone this follows" );
            if ( U::ImGuiUtilities::AssetSlot( "sockettarget", targetName.c_str(), c.Target.IsNull() ) )
                ImGui::OpenPopup( "socket_target" );
            if ( ImGui::BeginPopup( "socket_target" ) )
            {
                if ( ImGui::Selectable( "None (detached)" ) )
                {
                    c.Target = {};
                    c.BoneName.clear();
                }
                if ( scene )
                {
                    auto& registry = scene->GetRegistry();
                    auto view = registry.view<::Desert::ECS::SkinnedMeshComponent, ::Desert::ECS::UUIDComponent>();
                    for ( auto handle : view )
                    {
                        ::Desert::ECS::Entity candidate( handle, registry );
                        const auto            uuid = candidate.GetComponent<::Desert::ECS::UUIDComponent>().UUID;
                        const std::string     name = candidate.HasComponent<::Desert::ECS::TagComponent>()
                                                          ? candidate.GetComponent<::Desert::ECS::TagComponent>().Tag
                                                          : std::string( "Entity" );
                        if ( ImGui::Selectable( name.c_str(), uuid == c.Target ) )
                        {
                            c.Target = uuid;
                            c.BoneName.clear(); // a bone of the OLD rig means nothing on the new one
                        }
                    }
                }
                ImGui::EndPopup();
            }
            U::ImGuiUtilities::EndPropertyRow();

            // --- Bone: the target's own bone names --------------------------------------------------
            const ::Desert::Animation::Skeleton* skeleton = nullptr;
            if ( targetEntity )
            {
                const auto&     smc = targetEntity->GetComponent<::Desert::ECS::SkinnedMeshComponent>();
                ::Desert::Mesh* mesh =
                     smc.RuntimeMesh
                          ? static_cast<::Desert::Mesh*>( smc.RuntimeMesh.get() )
                          : ::Desert::Runtime::ResourceRegistry::GetMeshService()->Get( smc.MeshHandle );
                if ( mesh && mesh->IsSkinned() )
                    skeleton = &static_cast<::Desert::SkinnedMesh*>( mesh )->GetSkeleton();
            }

            U::ImGuiUtilities::BeginPropertyRow( "Bone", "Bone on the target's skeleton to follow" );
            const std::string bonePreview = c.BoneName.empty() ? "None" : c.BoneName;
            if ( U::ImGuiUtilities::AssetSlot( "socketbone", bonePreview.c_str(), c.BoneName.empty() ) )
                ImGui::OpenPopup( "socket_bone" );
            if ( ImGui::BeginPopup( "socket_bone" ) )
            {
                if ( !skeleton )
                {
                    ImGui::TextDisabled( "Pick a target with a skeleton first" );
                }
                else
                {
                    static ImGuiTextFilter boneFilter;
                    boneFilter.Draw( "##bonesearch", 180.0f );
                    ImGui::Separator();
                    for ( const auto& bone : skeleton->GetBones() )
                    {
                        if ( !boneFilter.PassFilter( bone.Name.c_str() ) )
                            continue;
                        if ( ImGui::Selectable( bone.Name.c_str(), bone.Name == c.BoneName ) )
                            c.BoneName = bone.Name;
                    }
                }
                ImGui::EndPopup();
            }
            U::ImGuiUtilities::EndPropertyRow();

            // --- Grip offset (the weapon almost never sits on the bone origin) -----------------------
            U::ImGuiUtilities::BeginPropertyRow( "Offset Location", "Relative to the bone, in centimetres" );
            U::ImGuiUtilities::VectorField( "sockloc", &c.OffsetTranslation.x, 3, 0.5f, "%.1f" );
            U::ImGuiUtilities::EndPropertyRow();

            // Stored in radians like every other rotation in the engine; shown in degrees like every other
            // rotation in the editor.
            U::ImGuiUtilities::BeginPropertyRow( "Offset Rotation", "Relative to the bone, in degrees" );
            glm::vec3 socketDegrees = glm::degrees( c.OffsetRotation );
            if ( U::ImGuiUtilities::VectorField( "sockrot", &socketDegrees.x, 3, 0.5f, "%.1f\xc2\xb0" ) )
                c.OffsetRotation = glm::radians( socketDegrees );
            U::ImGuiUtilities::EndPropertyRow();

            U::ImGuiUtilities::BeginPropertyRow( "Offset Scale" );
            U::ImGuiUtilities::VectorField( "sockscale", &c.OffsetScale.x, 3, 0.01f, "%.3f" );
            U::ImGuiUtilities::EndPropertyRow();

            if ( targetEntity && !skeleton )
                ImGui::TextDisabled( ICON_MDI_ALERT "  The target has no built skeleton yet" );
        };
        return e;
    }

    // Locomotion: the state -> clip mapping LocomotionSystem reads. The clip names are picked from the
    // animation library rather than typed, because a typo here is a character that simply never walks.
    static ComponentEditorEntry MakeLocomotionEntry()
    {
        using C = ::Desert::ECS::LocomotionComponent;
        ComponentEditorEntry e;
        e.Name      = "Locomotion";
        e.CanRemove = true;
        e.Has       = []( ::Desert::ECS::Entity& en ) { return en.HasComponent<C>(); };
        e.Add       = []( ::Desert::ECS::Entity& en ) { en.AddComponent<C>(); };
        e.Remove    = []( ::Desert::ECS::Entity& en ) { en.RemoveComponent<C>(); };
        e.Draw      = []( ::Desert::ECS::Entity& en, ::Desert::Core::Scene*, const ComponentEditContext& ctx )
        {
            namespace U = ::Desert::Editor::Utils;
            auto& c     = en.GetComponent<C>();

            U::ImGuiUtilities::ResetPropertyRows();

            // The clips that fit THIS character's skeleton, if it has one; otherwise the field is still
            // editable as free text through the same popup (the library may load later).
            std::vector<std::string> clipNames;
            if ( ctx.AnimationLibrary && en.HasComponent<::Desert::ECS::SkinnedMeshComponent>() )
            {
                const auto&     smc = en.GetComponent<::Desert::ECS::SkinnedMeshComponent>();
                ::Desert::Mesh* mesh =
                     smc.RuntimeMesh
                          ? static_cast<::Desert::Mesh*>( smc.RuntimeMesh.get() )
                          : ::Desert::Runtime::ResourceRegistry::GetMeshService()->Get( smc.MeshHandle );
                if ( mesh && mesh->IsSkinned() )
                {
                    const auto& skeleton = static_cast<::Desert::SkinnedMesh*>( mesh )->GetSkeleton();
                    for ( const auto& asset : ctx.AnimationLibrary->GetForSkeleton( skeleton ) )
                        if ( asset )
                            clipNames.push_back( asset->GetClip().AnimationName );
                }
            }

            const auto clipRow = [&clipNames]( const char* label, std::string& value, const char* id )
            {
                U::ImGuiUtilities::BeginPropertyRow( label );
                if ( U::ImGuiUtilities::AssetSlot( id, value.empty() ? "None" : value.c_str(), value.empty() ) )
                    ImGui::OpenPopup( id );
                if ( ImGui::BeginPopup( id ) )
                {
                    if ( clipNames.empty() )
                        ImGui::TextDisabled( "No clips for this skeleton" );
                    for ( const auto& name : clipNames )
                        if ( ImGui::Selectable( name.c_str(), name == value ) )
                            value = name;
                    ImGui::EndPopup();
                }
                U::ImGuiUtilities::EndPropertyRow();
            };

            clipRow( "Idle Clip", c.IdleClip, "loco_idle" );
            clipRow( "Walk Clip", c.WalkClip, "loco_walk" );
            clipRow( "Run Clip", c.RunClip, "loco_run" );
            clipRow( "Jump Clip", c.JumpClip, "loco_jump" );

            U::ImGuiUtilities::BeginPropertyRow( "Walk Speed", "Planar speed above which the walk clip plays" );
            ImGui::DragFloat( "##walkspeed", &c.WalkSpeed, 0.01f, 0.0f, 100.0f, "%.2f" );
            U::ImGuiUtilities::EndPropertyRow();

            U::ImGuiUtilities::BeginPropertyRow( "Run Speed", "Planar speed above which the run clip plays" );
            ImGui::DragFloat( "##runspeed", &c.RunSpeed, 0.01f, 0.0f, 100.0f, "%.2f" );
            U::ImGuiUtilities::EndPropertyRow();
        };
        return e;
    }

    // Projectile: integrated by ProjectileSystem in Play. Everything here is authored data except Owner,
    // which the firing script stamps at spawn — shown read-only so a stray hit can be traced back.
    static ComponentEditorEntry MakeProjectileEntry()
    {
        using C = ::Desert::ECS::ProjectileComponent;
        ComponentEditorEntry e;
        e.Name      = "Projectile";
        e.CanRemove = true;
        e.Has       = []( ::Desert::ECS::Entity& en ) { return en.HasComponent<C>(); };
        e.Add       = []( ::Desert::ECS::Entity& en ) { en.AddComponent<C>(); };
        e.Remove    = []( ::Desert::ECS::Entity& en ) { en.RemoveComponent<C>(); };
        e.Draw      = []( ::Desert::ECS::Entity& en, ::Desert::Core::Scene*, const ComponentEditContext& )
        {
            namespace U = ::Desert::Editor::Utils;
            auto& c     = en.GetComponent<C>();

            U::ImGuiUtilities::ResetPropertyRows();

            U::ImGuiUtilities::BeginPropertyRow( "Velocity", "World units per second" );
            U::ImGuiUtilities::VectorField( "projvel", &c.Velocity.x, 3, 1.0f, "%.0f" );
            U::ImGuiUtilities::EndPropertyRow();

            U::ImGuiUtilities::BeginPropertyRow( "Gravity Scale", "0 = straight line, 1 = full gravity (arc)" );
            ImGui::SliderFloat( "##projgrav", &c.GravityScale, 0.0f, 2.0f, "%.2f" );
            U::ImGuiUtilities::EndPropertyRow();

            U::ImGuiUtilities::BeginPropertyRow( "Life Remaining", "Seconds before it despawns on its own" );
            ImGui::DragFloat( "##projlife", &c.LifeRemaining, 0.1f, 0.0f, 600.0f, "%.1f s" );
            U::ImGuiUtilities::EndPropertyRow();

            U::ImGuiUtilities::BeginPropertyRow( "Damage" );
            ImGui::DragFloat( "##projdmg", &c.Damage, 0.5f, 0.0f, 10000.0f, "%.1f" );
            U::ImGuiUtilities::EndPropertyRow();

            U::ImGuiUtilities::BeginPropertyRow( "Owner", "The shooter, stamped by the script that fired it "
                                                          "(self-hits are skipped)" );
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled(
                 "%s", c.Owner.IsNull() ? "None" : std::to_string( static_cast<uint64_t>( c.Owner ) ).c_str() );
            U::ImGuiUtilities::EndPropertyRow();
        };
        return e;
    }

    // Foliage type: the scatter parameters the paint brush reads. They lived ONLY in the viewport's paint
    // overlay, so a type could not be tuned without holding the brush.
    static ComponentEditorEntry MakeFoliageEntry()
    {
        using C = ::Desert::ECS::FoliageComponent;
        ComponentEditorEntry e;
        e.Name      = "Foliage Type";
        e.CanRemove = true;
        e.Has       = []( ::Desert::ECS::Entity& en ) { return en.HasComponent<C>(); };
        e.Add       = []( ::Desert::ECS::Entity& en ) { en.AddComponent<C>(); };
        e.Remove    = []( ::Desert::ECS::Entity& en ) { en.RemoveComponent<C>(); };
        e.Draw      = []( ::Desert::ECS::Entity& en, ::Desert::Core::Scene*, const ComponentEditContext& )
        {
            namespace U = ::Desert::Editor::Utils;
            auto& f     = en.GetComponent<C>();

            U::ImGuiUtilities::ResetPropertyRows();

            U::ImGuiUtilities::BeginPropertyRow( "Density", "Instances scattered per paint dab" );
            ImGui::SliderFloat( "##foldensity", &f.Density, 1.0f, 80.0f, "%.0f / dab" );
            U::ImGuiUtilities::EndPropertyRow();

            U::ImGuiUtilities::BeginPropertyRow( "Scale Range", "Random uniform scale per instance" );
            ImGui::DragFloatRange2( "##folscale", &f.ScaleMin, &f.ScaleMax, 0.01f, 0.02f, 10.0f, "%.2f", "%.2f" );
            U::ImGuiUtilities::EndPropertyRow();

            U::ImGuiUtilities::BeginPropertyRow( "Z Offset", "Sink (-) / raise (+) along world up" );
            ImGui::DragFloatRange2( "##folz", &f.ZOffsetMin, &f.ZOffsetMax, 0.5f, -500.0f, 500.0f, "%.0f",
                                    "%.0f" );
            U::ImGuiUtilities::EndPropertyRow();

            U::ImGuiUtilities::BeginPropertyRow( "Max Pitch", "Random tilt off the up/normal axis" );
            ImGui::SliderFloat( "##folpitch", &f.MaxPitchDeg, 0.0f, 90.0f, "%.0f deg" );
            U::ImGuiUtilities::EndPropertyRow();

            U::ImGuiUtilities::BeginPropertyRow( "Slope Range", "Only paint where the surface slope fits" );
            ImGui::DragFloatRange2( "##folslope", &f.SlopeMinDeg, &f.SlopeMaxDeg, 0.5f, 0.0f, 90.0f, "%.0f",
                                    "%.0f deg" );
            U::ImGuiUtilities::EndPropertyRow();

            U::ImGuiUtilities::BeginPropertyRow( "Align to Normal" );
            ImGui::Checkbox( "##folalign", &f.AlignToNormal );
            U::ImGuiUtilities::EndPropertyRow();

            U::ImGuiUtilities::BeginPropertyRow( "Random Yaw" );
            ImGui::Checkbox( "##folyaw", &f.RandomYaw );
            U::ImGuiUtilities::EndPropertyRow();
        };
        return e;
    }

    // Character controller: the authored capsule (reflected) and, in Play, what the physics step is
    // actually reporting back. "Why does he not jump" is answered by On Ground, which the component has
    // always carried and the panel never showed.
    static ComponentEditorEntry MakeCharacterControllerEntry()
    {
        using C = ::Desert::ECS::CharacterControllerComponent;
        ComponentEditorEntry e;
        e.Name              = "Character Controller";
        e.CanRemove         = true;
        e.ReflectedTypeName = "CharacterControllerData";
        e.Has               = []( ::Desert::ECS::Entity& en ) { return en.HasComponent<C>(); };
        e.Add               = []( ::Desert::ECS::Entity& en ) { en.AddComponent<C>(); };
        e.Remove            = []( ::Desert::ECS::Entity& en ) { en.RemoveComponent<C>(); };
        e.DataPtr           = []( ::Desert::ECS::Entity& en ) -> void* { return &en.GetComponent<C>().Data; };
        e.Draw = []( ::Desert::ECS::Entity& en, ::Desert::Core::Scene*, const ComponentEditContext& ctx )
        {
            namespace U = ::Desert::Editor::Utils;
            auto& c     = en.GetComponent<C>();

            PropertyEditorBuilder::Draw( &c.Data, "CharacterControllerData", ctx.AssetMgr(), ctx.UIHelper,
                                         ctx.FieldFilter );
            if ( ctx.FieldFilter )
                return;

            // Only meaningful while the physics step is running — outside Play these are the last values
            // from the previous run, which would read as live state.
            const bool running = c.RuntimeCharacter != ::Desert::Physics::kInvalidCharacter;

            ImGui::Dummy( ImVec2( 0.0f, 4.0f ) );
            if ( !U::ImGuiUtilities::SectionHeader( ICON_MDI_PULSE "  Runtime", false ) )
                return;

            U::ImGuiUtilities::ResetPropertyRows();
            if ( !running )
            {
                ImGui::TextDisabled( "Live while playing." );
                return;
            }

            const auto readOnlyRow = []( const char* label, const std::string& value )
            {
                U::ImGuiUtilities::BeginPropertyRow( label );
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted( value.c_str() );
                U::ImGuiUtilities::EndPropertyRow();
            };

            char buf[64];
            readOnlyRow( "On Ground", c.OnGround ? "Yes" : "No" );
            std::snprintf( buf, sizeof( buf ), "%.0f cm/s", c.CurrentSpeed );
            readOnlyRow( "Planar Speed", buf );
            std::snprintf( buf, sizeof( buf ), "%.0f cm/s", c.VerticalVelocity );
            readOnlyRow( "Vertical Velocity", buf );
            readOnlyRow( "Swimming", c.Swimming ? "Yes" : "No" );
            std::snprintf( buf, sizeof( buf ), "%.2f, %.2f", c.MoveInput.x, c.MoveInput.y );
            readOnlyRow( "Move Input", buf );
        };
        return e;
    }

    // Blendshape / morph-target editor: one slider per morph target of the entity's mesh (static or skinned).
    // Names + count come from the mesh asset; the sliders write MorphComponent::Weights (index-aligned).
    static ComponentEditorEntry MakeMorphEntry()
    {
        using MC = ::Desert::ECS::MorphComponent;
        ComponentEditorEntry e;
        e.Name      = "Morph Targets";
        e.CanRemove = true;
        e.Has       = []( ::Desert::ECS::Entity& en ) { return en.HasComponent<MC>(); };
        e.Add       = []( ::Desert::ECS::Entity& en ) { en.AddComponent<MC>(); };
        e.Remove    = []( ::Desert::ECS::Entity& en ) { en.RemoveComponent<MC>(); };
        e.Draw      = []( ::Desert::ECS::Entity& en, ::Desert::Core::Scene*, const ComponentEditContext& )
        {
            namespace ImGui = ::ImGui;
            auto& mc        = en.GetComponent<MC>();

            ::Desert::Assets::AssetHandle meshHandle;
            if ( en.HasComponent<::Desert::ECS::SkinnedMeshComponent>() )
                meshHandle = en.GetComponent<::Desert::ECS::SkinnedMeshComponent>().MeshHandle;
            else if ( en.HasComponent<::Desert::ECS::StaticMeshComponent>() )
                meshHandle = en.GetComponent<::Desert::ECS::StaticMeshComponent>().MeshHandle;

            const ::Desert::Assets::MeshAsset* meshAsset =
                 meshHandle ? ::Desert::Runtime::ResourceRegistry::GetMeshService()->GetAsset( meshHandle )
                            : nullptr;

            if ( !meshAsset || meshAsset->GetMorphTargets().empty() )
            {
                ImGui::TextDisabled( "This entity's mesh has no blendshapes (morph targets)." );
                return;
            }

            const auto& targets = meshAsset->GetMorphTargets();

            // Keep the component's arrays in step with the mesh's targets (the mesh may have been swapped).
            if ( mc.Weights.size() != targets.size() )
                mc.Weights.resize( targets.size(), 0.0f );
            mc.TargetNames.resize( targets.size() );
            for ( size_t i = 0; i < targets.size(); ++i )
                mc.TargetNames[i] = targets[i].Name;

            ImGui::TextDisabled( "%d blendshape(s)", static_cast<int>( targets.size() ) );
            for ( size_t i = 0; i < targets.size(); ++i )
            {
                ImGui::PushID( static_cast<int>( i ) );
                const char* name = mc.TargetNames[i].empty() ? "<unnamed>" : mc.TargetNames[i].c_str();
                ImGui::SliderFloat( name, &mc.Weights[i], 0.0f, 1.0f );
                ImGui::PopID();
            }
            if ( ImGui::Button( "Reset All", ImVec2( -1.0f, 0.0f ) ) )
                std::fill( mc.Weights.begin(), mc.Weights.end(), 0.0f );
        };
        return e;
    }
} // namespace Desert::Editor

namespace
{
    const int _desert_emitter_component_reg =
         ::Desert::Editor::ComponentWidgetRegistry::Get().Register( ::Desert::Editor::MakeEmitterEntry() );

    const int _desert_dirlight_component_reg = ::Desert::Editor::ComponentWidgetRegistry::Get().Register(
         ::Desert::Editor::MakeDirectionalLightEntry() );
    const int _desert_camera_component_reg =
         ::Desert::Editor::ComponentWidgetRegistry::Get().Register( ::Desert::Editor::MakeCameraEntry() );

    const int _desert_collider_component_reg =
         ::Desert::Editor::ComponentWidgetRegistry::Get().Register( ::Desert::Editor::MakeColliderEntry() );
    const int _desert_landscape_material_component_reg = ::Desert::Editor::ComponentWidgetRegistry::Get().Register(
         ::Desert::Editor::MakeLandscapeMaterialEntry() );

    const int _desert_volumetric_cloud_component_reg =
         ::Desert::Editor::ComponentWidgetRegistry::Get().Register( ::Desert::Editor::MakeVolumetricCloudEntry() );

    const int _desert_ism_component_reg = ::Desert::Editor::ComponentWidgetRegistry::Get().Register(
         ::Desert::Editor::MakeInstancedStaticMeshEntry() );

    const int _desert_morph_component_reg =
         ::Desert::Editor::ComponentWidgetRegistry::Get().Register( ::Desert::Editor::MakeMorphEntry() );

    const int _desert_charctrl_component_reg = ::Desert::Editor::ComponentWidgetRegistry::Get().Register(
         ::Desert::Editor::MakeCharacterControllerEntry() );
    const int _desert_socket_component_reg =
         ::Desert::Editor::ComponentWidgetRegistry::Get().Register( ::Desert::Editor::MakeSocketEntry() );
    const int _desert_locomotion_component_reg =
         ::Desert::Editor::ComponentWidgetRegistry::Get().Register( ::Desert::Editor::MakeLocomotionEntry() );
    const int _desert_projectile_component_reg =
         ::Desert::Editor::ComponentWidgetRegistry::Get().Register( ::Desert::Editor::MakeProjectileEntry() );
    const int _desert_foliage_component_reg =
         ::Desert::Editor::ComponentWidgetRegistry::Get().Register( ::Desert::Editor::MakeFoliageEntry() );

    const int _desert_uicanvas_component_reg =
         ::Desert::Editor::ComponentWidgetRegistry::Get().Register( ::Desert::Editor::MakeUICanvasEntry() );
    const int _desert_uistyle_component_reg =
         ::Desert::Editor::ComponentWidgetRegistry::Get().Register( ::Desert::Editor::MakeUIStyleEntry() );
    const int _desert_uilayout_component_reg =
         ::Desert::Editor::ComponentWidgetRegistry::Get().Register( ::Desert::Editor::MakeUILayoutEntry() );

    // ── THE SCRIPT PICKER'S LIST, and the two things it used to get wrong at once ──────────────────
    //
    // It walked the LITERAL relative path "Resources" with a raw recursive_directory_iterator. That is
    // two defects in one expression, and they fail in different directions:
    //
    //   1. it bypassed the shared enumeration, so a project served from a mounted .dpak offered an
    //      empty dropdown — indistinguishable from a project with no scripts (§1.4);
    //   2. it bypassed the PROJECT PATH census entirely. "Resources" is resolved against the PROCESS's
    //      working directory and is never remapped by SetProjectRoot, so opening a second project still
    //      listed the FIRST tree's scripts and wrote a path pointing outside the open project into the
    //      scene. That is the same sentence four other defects in this engine were: a path resolved
    //      from where the process is standing rather than from the content it is about.
    //
    // The list now comes from Constants::Path::SCRIPT_PATH — the census row for a project's scripts —
    // through Common::Utils::FileSystem::ListFilesRecursive, which returns the loose files and the
    // mounted ones together. Moving the project moves the list; there is nothing here to forget.
    //
    // The examples that used to sit in the ENGINE resource tree (Resources/Scripts/Examples) were moved
    // under the assets root by the same change, because that is where this census row says a project's
    // scripts live — and the engine tree is not one of the five trees a package is built from, so
    // nothing under it could ever have shipped with a game.
    // Returns KEYS, not paths (I9). What a slot stores is the root-tagged key
    // `Common::AssetHandle::StableKeyForPath` mints, because a rooted path does not survive packaging —
    // see ECS::ScriptSlot::ScriptKey. Minting it HERE, at the one place the picker turns a file into a
    // reference, is what keeps every stored value in that form: a widget that offered paths and left the
    // tagging to whoever assigned them would be one forgotten call site away from the old defect.
    std::vector<std::string> ProjectScriptKeys()
    {
        std::vector<std::string> scripts;
        for ( const std::filesystem::path& file :
              Common::Utils::FileSystem::ListFilesRecursive( Common::Constants::Path::SCRIPT_PATH ) )
        {
            if ( file.extension() == ".lua" )
                scripts.push_back( Common::AssetHandle::StableKeyForPath( file ) );
        }
        // Neither half of the enumeration promises an order, and a dropdown that reshuffles between
        // sessions is one nobody can learn.
        std::sort( scripts.begin(), scripts.end() );
        return scripts;
    }

    // What to CALL a script in the dropdown. The file name alone was ambiguous the moment two folders
    // held a `Main.lua`, and the walk was recursive even before this change — so the label is the path
    // relative to the scripts root, which is unique by construction. Takes a KEY and resolves it first:
    // the label has to name a place on this machine, and the key deliberately does not.
    std::string ScriptLabel( const std::string& scriptKey )
    {
        const std::filesystem::path file = Common::AssetHandle::PathForStableKey( scriptKey );

        std::error_code   ec;
        const std::string relative =
             std::filesystem::relative( file, Common::Constants::Path::SCRIPT_PATH, ec ).generic_string();
        return ( ec || relative.empty() ) ? file.filename().generic_string() : relative;
    }
} // namespace

// SINGLE SOURCE OF TRUTH: materials (shader + params + textures) are authored ONLY in the Material Editor
// window, on a `.demat`. A mesh entity names one per slot; a landscape root names one, in LandscapeMaterialData.
// MaterialComponent is no longer authored ANYWHERE in the editor: it remains only (a) the RUNTIME override
// channel for scripts (Lua setMaterialParam) — surfaced by the PBR Materials banner with one-click clear —
// and (b) legacy-scene compatibility. The terrain was the last thing authoring it, and the v6 -> v7 scene
// migration takes it off terrain entities in the files as well.
// Script component: an entity can run SEVERAL scripts (like UE ActorComponents), shown as a list of slots.
// Per slot: pick the .lua from a dropdown OR drag one from the File Explorer, Reload (hot-reload), and edit
// the script's exposed Properties. "+ Add Script" appends a slot; the X removes one.
DESERT_REGISTER_CUSTOM_COMPONENT(
     ::Desert::ECS::ScriptComponent, "Script", true,
     (
          []( ::Desert::ECS::Entity& e, ::Desert::Core::Scene*, const ::Desert::Editor::ComponentEditContext& )
          {
              namespace fs = std::filesystem;
              auto& sc     = e.GetComponent<::Desert::ECS::ScriptComponent>();

              int removeIndex = -1;
              for ( size_t i = 0; i < sc.Scripts.size(); ++i )
              {
                  ImGui::PushID( static_cast<int>( i ) );
                  auto& slot = sc.Scripts[i];

                  const std::string preview =
                       slot.ScriptKey.empty() ? "Select script..." : slot.ResolvedPath().filename().string();

                  ImGui::SetNextItemWidth( -60.0f ); // leave room for the remove button
                  if ( ImGui::BeginCombo( "##ScriptSel", preview.c_str() ) )
                  {
                      const std::vector<std::string> scripts = ProjectScriptKeys();
                      for ( const std::string& script : scripts )
                      {
                          if ( ImGui::Selectable( ScriptLabel( script ).c_str(), slot.ScriptKey == script ) )
                          {
                              slot.ScriptKey = script;
                              slot.Started   = false;
                              slot.Properties.clear(); // re-seed from the new script's schema below
                          }
                      }
                      // AN EMPTY LIST SAYS SO, and says where it looked. An empty popup is the shape
                      // §1.4 forbids: "this project has no scripts" and "the picker is looking in the
                      // wrong place" used to render as the same three blank pixels.
                      if ( scripts.empty() )
                          ImGui::TextDisabled( "no .lua under %s",
                                               Common::Constants::Path::SCRIPT_PATH.generic_string().c_str() );
                      ImGui::EndCombo();
                  }

                  // Drag a .lua from the File Explorer onto the combo.
                  if ( ImGui::BeginDragDropTarget() )
                  {
                      if ( const ImGuiPayload* payload = ImGui::AcceptDragDropPayload( "AssetFile" ) )
                      {
                          const std::string dropped( static_cast<const char*>( payload->Data ) );
                          if ( dropped.size() > 4 && dropped.substr( dropped.size() - 4 ) == ".lua" )
                          {
                              // Through the SAME minting as the dropdown: the payload is a path the File
                              // Explorer is standing on, and storing it verbatim is precisely how the
                              // rooted spelling got into the three scenes I9 had to migrate.
                              slot.ScriptKey = Common::AssetHandle::StableKeyForPath( fs::path( dropped ) );
                              slot.Started   = false;
                              slot.Properties.clear();
                          }
                      }
                      ImGui::EndDragDropTarget();
                  }

                  ImGui::SameLine();
                  if ( ImGui::Button( "X" ) )
                      removeIndex = static_cast<int>( i );

                  if ( !slot.ScriptKey.empty() )
                  {
                      ImGui::TextDisabled( "%s", slot.ScriptKey.c_str() );

                      if ( ImGui::Button( "Reload" ) )
                          slot.Started = false; // re-read on the next Play frame (hot-reload)

                      // ---- Exposed properties (the script's `Properties` table) ----
                      if ( slot.Properties.empty() )
                          slot.Properties =
                               ::Desert::Scripting::ReadScriptProperties( slot.ResolvedPath().generic_string() );

                      ImGui::SameLine();
                      if ( ImGui::Button( "Refresh Props" ) )
                      {
                          // Re-read the schema, keeping existing values for properties that still exist.
                          auto schema =
                               ::Desert::Scripting::ReadScriptProperties( slot.ResolvedPath().generic_string() );
                          for ( auto& s : schema )
                          {
                              auto old = std::find_if( slot.Properties.begin(), slot.Properties.end(),
                                                       [&]( const auto& p )
                                                       { return p.Name == s.Name && p.Type == s.Type; } );
                              if ( old != slot.Properties.end() )
                                  s = *old;
                          }
                          slot.Properties = std::move( schema );
                      }

                      for ( auto& p : slot.Properties )
                      {
                          switch ( p.Type )
                          {
                              case ::Desert::Scripting::PropertyType::Number:
                              {
                                  float v = static_cast<float>( p.Number );
                                  if ( ImGui::DragFloat( p.Name.c_str(), &v, 0.01f ) )
                                      p.Number = v;
                                  break;
                              }
                              case ::Desert::Scripting::PropertyType::Bool:
                                  ImGui::Checkbox( p.Name.c_str(), &p.Bool );
                                  break;
                              case ::Desert::Scripting::PropertyType::String:
                              {
                                  char buf[256] = { 0 };
                                  std::strncpy( buf, p.Str.c_str(), sizeof( buf ) - 1 );
                                  if ( ImGui::InputText( p.Name.c_str(), buf, sizeof( buf ) ) )
                                      p.Str = buf;
                                  break;
                              }
                          }
                      }
                  }

                  ImGui::Separator();
                  ImGui::PopID();
              }

              if ( removeIndex >= 0 )
                  sc.Scripts.erase( sc.Scripts.begin() + removeIndex );

              if ( ImGui::Button( "+ Add Script" ) )
                  sc.Scripts.emplace_back();
          } ) )

// Text (SDF world-space label). Simple field editor; the mesh rebuilds automatically when Text/
// Font/Size change (TextECSSystem compares against its Built* cache).
DESERT_REGISTER_CUSTOM_COMPONENT(
     ::Desert::ECS::TextComponent, "Text", true,
     (
          []( ::Desert::ECS::Entity& e, ::Desert::Core::Scene*, const ::Desert::Editor::ComponentEditContext& )
          {
              auto& tc = e.GetComponent<::Desert::ECS::TextComponent>();

              ::Desert::Editor::Utils::ImGuiUtilities::ResetPropertyRows();

              char buf[512] = { 0 };
              std::strncpy( buf, tc.Text.c_str(), sizeof( buf ) - 1 );
              ::Desert::Editor::Utils::ImGuiUtilities::BeginPropertyRow( "Text", nullptr, 60.0f );
              if ( ImGui::InputTextMultiline( "##text", buf, sizeof( buf ), ImVec2( -1.0f, 56.0f ) ) )
                  tc.Text = buf;
              ::Desert::Editor::Utils::ImGuiUtilities::EndPropertyRow();

              // Font: an ASSET HANDLE (never a raw path) — pick one of the preloaded fonts from the dropdown
              // or drag a .ttf from the Content Browser. FontService owns the handle<->path registry and the
              // preloaded set; "Default" (null handle) falls back to the engine's built-in font.
              auto*             fs      = ::Desert::Runtime::ResourceRegistry::GetFontService();
              const uint64_t    curHnd  = static_cast<uint64_t>( tc.Font );
              const std::string curPath = fs ? fs->PathForHandle( curHnd ) : "";
              const std::string preview =
                   curHnd == 0
                        ? "Default"
                        : ( curPath.empty() ? "(missing)" : std::filesystem::path( curPath ).stem().string() );
              ::Desert::Editor::Utils::ImGuiUtilities::BeginPropertyRow( "Font" );
              if ( ImGui::BeginCombo( "##textfont", preview.c_str() ) )
              {
                  if ( ImGui::Selectable( "Default", curHnd == 0 ) )
                      tc.Font = ::Desert::Assets::AssetHandle();
                  if ( fs )
                  {
                      for ( const auto& f : fs->AvailableFonts() )
                      {
                          const uint64_t h   = fs->RegisterFont( f );
                          const bool     sel = ( h == curHnd );
                          if ( ImGui::Selectable( std::filesystem::path( f ).stem().string().c_str(), sel ) )
                              tc.Font = ::Desert::Assets::AssetHandle( h );
                          if ( sel )
                              ImGui::SetItemDefaultFocus();
                      }
                  }
                  ImGui::EndCombo();
              }
              ::Desert::Editor::Utils::ImGuiUtilities::EndPropertyRow();
              if ( ImGui::BeginDragDropTarget() )
              {
                  if ( const ImGuiPayload* pl =
                            ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::FontFile ) )
                  {
                      const std::string path( static_cast<const char*>( pl->Data ),
                                              pl->DataSize > 0 ? pl->DataSize - 1 : 0 );
                      if ( fs && !path.empty() )
                          tc.Font = ::Desert::Assets::AssetHandle( fs->RegisterFont( path ) );
                  }
                  ImGui::EndDragDropTarget();
              }
              if ( ImGui::IsItemHovered() )
                  ImGui::SetTooltip( "Pick a preloaded font or drag a .ttf here from the Content Browser" );

              namespace TU = ::Desert::Editor::Utils;

              TU::ImGuiUtilities::BeginPropertyRow( "Color" );
              ImGui::ColorEdit4( "##textcolor", &tc.Color.x );
              TU::ImGuiUtilities::EndPropertyRow();

              TU::ImGuiUtilities::BeginPropertyRow( "Size", "World units per em" );
              ImGui::DragFloat( "##textsize", &tc.Size, 1.0f, 1.0f, 10000.0f, "%.1f cm" );
              TU::ImGuiUtilities::EndPropertyRow();

              TU::ImGuiUtilities::BeginPropertyRow( "Emissive Intensity", "Above ~1 the text blooms" );
              ImGui::DragFloat( "##textemissive", &tc.EmissiveIntensity, 0.05f, 0.0f, 20.0f, "%.2f" );
              TU::ImGuiUtilities::EndPropertyRow();
              if ( ImGui::IsItemHovered() )
                  ImGui::SetTooltip( "> ~1 makes the text bloom (it renders into the HDR scene)" );
              TU::ImGuiUtilities::BeginPropertyRow( "Billboard", "Always face the camera" );
              ImGui::Checkbox( "##textbillboard", &tc.Billboard );
              TU::ImGuiUtilities::EndPropertyRow();
          } ) )
