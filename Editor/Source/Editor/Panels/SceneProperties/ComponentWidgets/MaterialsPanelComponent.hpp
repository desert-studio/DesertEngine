#pragma once

#include "IComponentWidget.hpp"

#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Engine/Graphic/Materials/MaterialFactory.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <ImGui/imgui.h>
#include "Editor/Widgets/UIHelper/ImGuiUI.hpp"
#include <Editor/Widgets/ThumbnailCache.hpp>

#include <filesystem>
#include <unordered_map>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor
{
    class MaterialComponentWidget
    {
    public:
        MaterialComponentWidget( const Assets::AssetManager* assetManager );

        // @p scene supplies the active camera for the "drawing LOD n" readout; without it that line is
        // simply omitted.
        void Render( ECS::Entity& entity, ::Desert::Core::Scene* scene = nullptr );

    public:
        // The mesh whose slots are being edited, as a VIEW rather than a component type. A skinned mesh
        // carries the same MaterialSlots + RuntimeMaterialInstances pair as a static one and its renderer
        // maps them identically — the slot editor was simply hard-wired to StaticMeshComponent, so skinned
        // meshes had no material UI at all. Everything below works on this.
        struct MaterialHost
        {
            std::vector<Assets::AssetHandle>*          Slots            = nullptr;
            std::vector<Graphic::MaterialInstancePtr>* RuntimeInstances = nullptr;
            Assets::AssetHandle                        MeshHandle;
            ::Desert::Mesh*                            Mesh = nullptr; // the mesh actually drawn

            // Drops the cached runtime instances so the renderer rebuilds them from the slots next tick.
            void Invalidate() const
            {
                if ( RuntimeInstances )
                    RuntimeInstances->clear();
            }
        };

        // Resolves the entity's mesh component (static first, then skinned) into a host. Slots == nullptr
        // when the entity has neither.
        static MaterialHost HostOf( ECS::Entity& entity );

    private:
        // overriddenByShader: non-empty when the entity's Shader Override component routes the
        // mesh off the PBR path — the slots are shown collapsed with a notice.
        void RenderMaterialProperties( ECS::Entity& entity, const MaterialHost& host,
                                       const std::string& overriddenByShader );

        // Creates a child material-instance asset (.demat with a Parent GUID) next to the other
        // materials and registers its shell (lazy — instances have no runtime Material of their own).
        Assets::AssetHandle CreateAndRegisterMaterialInstance( const Assets::SurfaceMaterialAsset& parent );

        // Number of material slots the mesh expects (one per submesh; 1 for primitives).
        size_t GetSubmeshCount( const MaterialHost& host ) const;
        // Creates a fresh PBR material asset on disk, registers its runtime material, returns its handle.
        // baseName is sanitized into the filename ("M_<Entity>"); identity stays the in-file GUID.
        Assets::AssetHandle CreateAndRegisterMaterial( const std::string& baseName = "Material" );
        // Resolves an asset path to a material, registers it if needed, and assigns it to a slot.
        void AssignMaterialFromPath( const MaterialHost& host, size_t slot, const std::string& assetPath );
        // Grows MaterialSlots up to `slot` by repeating the effective (last) handle so an inherited
        // element row gains its own slot without changing the rendered look.
        static void MakeSlotExplicit( const MaterialHost& host, size_t slot );

        // A slot's identifying colour, read from the SHADER SCHEMA rather than from hardcoded PBR names:
        // the first Color parameter of whatever shader the material runs. For the standard shader that is
        // the albedo; a custom DSL shader gets the same treatment from its own Properties block, free.
        //
        // Deliberately JUST the colour: UE shows a material slot as a thumbnail and a name, not as a
        // readout of its parameters — those live in the editor below, where they can be changed.
        struct SlotSwatch
        {
            bool      HasColor = false;
            glm::vec4 Color    = glm::vec4( 1.0f );
        };

        // parentData: material-instance mode — the value falls back to the parent chain, then to the
        // schema default, exactly like the parameter editor.
        static SlotSwatch BuildSlotSwatch( const Assets::SurfaceMaterialAsset& asset,
                                           const Assets::MaterialData*         parentData );

        // What the user asked for on an element row this frame. The row only REPORTS; the caller owns
        // the component and the asset manager and is the only place allowed to change a slot.
        //
        // Every one of these answers "WHICH material does this element use" — which is the entity's
        // question. "What IS this material" is the Material Editor window's, and the actions that asked it
        // here (the parameter fold, Save, Reset Overrides) went there with the editor
        // (Docs/MaterialEditor/PLAN_STAGE3_ASSET_DOCUMENTS.md, M2).
        enum class SlotAction
        {
            None,
            Pick,           // open the material picker
            MakeExplicit,   // give an inherited element its own slot
            CreateMaterial, // fresh material asset for an empty slot
            CreateInstance, // child instance of the slot's material
            OpenEditor,     // open the Material Editor window on this slot's material
        };

        // Everything one element row needs to draw itself. A struct rather than nine parameters: the row
        // is a pure view, and the list of things it shows will keep growing.
        struct SlotRow
        {
            size_t                              Index = 0;
            const Assets::SurfaceMaterialAsset* Asset = nullptr;
            SlotSwatch                          Swatch;
            std::string SlotName;   // the mesh's own name for this element (UE's "Slot" field)
            std::string ShaderName; // what it RENDERS with (an instance's comes from its parent)
            std::string ParentName; // instance parent; empty for a base material
            bool        HasOwnSlot = false;
            bool        IsInstance = false;
        };

        // The UE-style element row: "Element N" in the label column; in the value column a framed
        // preview underlined with the material's identity colour, the material's name in a sunk asset
        // field, and a strip of flat icon actions beneath it. A material dropped on the preview or the
        // field is written to @p droppedPath for the caller to assign.
        SlotAction DrawSlotRow( const SlotRow& row, std::string& droppedPath );

        // The mesh's own name for element @p index ("Body", "Head"), empty when it has none.
        std::string SlotNameOf( const MaterialHost& host, size_t index ) const;

        // The slot preview: the shared rendered-thumbnail PNG when the asset browser has produced one,
        // the material's own colour otherwise, with a colour bar under it — UE's underline that lets you
        // tell two slots apart at a glance.
        void DrawSlotPreview( const Assets::SurfaceMaterialAsset* asset, const SlotSwatch& swatch, float size );

    private:
        std::unique_ptr<Editor::UI::UIHelper> m_UIHelper;
        const Assets::AssetManager*           m_AssetManager;
        // Decoded rendered-thumbnail PNGs (the shared on-disk cache the asset browser fills).
        ThumbnailCache m_Thumbnails;
        // The write time each of those PNGs had when it was decoded. ThumbnailCache is keyed by path only,
        // so a PNG regenerated on disk keeps serving the copy decoded from the old one — invisible while
        // Details was also the thing that edited materials (it dropped its own entry on Save), and a stale
        // swatch the moment the editing moved to a window that cannot reach this cache. Compared per frame
        // rather than invalidated by whoever wrote the file: any regeneration counts, not just ours.
        std::unordered_map<std::string, std::filesystem::file_time_type> m_ThumbnailStamps;
    };

} // namespace Desert::Editor