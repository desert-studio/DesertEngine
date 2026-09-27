#pragma once

#include <Engine/Desert.hpp>
#include <Engine/Assets/FoliageTypeAsset.hpp>
#include <Common/Core/Math/Ray.hpp>
#include <Editor/Panels/ViewportPanel/Tools/FoliageBrush.hpp>
#include <Editor/Core/Selection/FoliagePaint.hpp>

#include <glm/glm.hpp>

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Desert::Editor::UI
{
    class UIHelper;
}

namespace Desert::Editor::Tools
{
    // UE5-style foliage painting, extracted from ViewportPanel (god-object split). The tool holds only the
    // stroke in flight (its random stream, the fields as they were at press for undo) — brush/selection live in
    // Editor::Core::FoliagePaint; WHAT is painted is the `.defoliage` each foliage entity names
    // (Assets::FoliageTypeAsset, FO-1); the host supplies the scene, asset manager and ray.
    class FoliagePaintTool
    {
    public:
        // The Foliage panel (UE Foliage Mode, FO-UI1; Editor/Panels/Foliage/FoliagePanel.cpp): the tool bar,
        // Brush Options and Filters, the palette of types (from any source; grid or list, check, eye, count,
        // cost, search, context menu, presets) and the Details of the type being edited. viewportPos = the
        // scene-image top-left (for placing the floating window); @p uiHelper draws the grid's thumbnails.
        void DrawPanel( ::Desert::Core::Scene& scene, const Assets::AssetManager* assetManager,
                        const glm::vec2& viewportPos, UI::UIHelper* uiHelper );

        // One tick of the brush while @p pressing: every CHECKED type is topped up to its density under the
        // cursor (FoliageBrush, UE AddInstancesForBrush) or erased. A stroke runs from press to release, draws
        // from one seeded stream and is ONE undo step; releasing (pressing = false) ends it.
        // The tool is Core::FoliagePaint::Tool() at press: Paint tops up, Single places one per click, Select
        // picks the instance the ray enters first, Lasso selects under the brush, Remove clears the brush,
        // Reapply rebuilds the instances under it. @p shift: Select adds, Lasso deselects.
        void Update( ::Desert::Core::Scene& scene, const Assets::AssetManager* assetManager,
                     const Common::Math::Ray& ray, bool pressing, bool shift = false );

        // UE RemoveSelectedInstances / TransformSelectedInstances / deselect, each ONE undo step. An error when
        // nothing is selected.
        static Common::BoolResultStr RemoveSelected( ::Desert::Core::Scene& scene );
        static Common::BoolResultStr MoveSelected( ::Desert::Core::Scene& scene, const glm::vec3& offset );
        static Common::BoolResultStr SelectNone( ::Desert::Core::Scene& scene );

        // UE ApplyPaintBucket_Add: the static mesh of @p entity (its tool-target mesh, world-placed) covered by
        // every checked type (FoliageFill), ONE undo step. An error naming why when the entity has no readable
        // static mesh, no type is checked, or nothing was placed.
        static Common::BoolResultStr FillEntity( ::Desert::Core::Scene& scene, Assets::AssetManager& manager,
                                                 const Common::UUID& entity );

        // UE draws selected foliage instances highlighted; here a ring on the viewport overlay at each selected
        // instance's origin (no render pass: the ImGui draw list of the viewport window). @p viewProjection is
        // the view camera's projection * view; @p viewportPos / @p viewportSize the scene image on screen.
        static void DrawSelection( ::Desert::Core::Scene& scene, const glm::mat4& viewProjection,
                                   const glm::vec2& viewportPos, const glm::vec2& viewportSize );

        [[nodiscard]] bool IsStroking() const
        {
            return m_Stroke.has_value();
        }

        // The type a foliage entity names, loaded on demand through the AsyncAssetLoader. Null (and logged
        // once per handle) when the handle names no `.defoliage` or its file does not load.
        static Assets::Asset<Assets::FoliageTypeAsset> ResolveType( Assets::AssetManager&      manager,
                                                                    const Assets::AssetHandle& handle );

        // Opens a `.defoliage` named by path (absolute or relative to the assets root) and loads it.
        static Assets::Asset<Assets::FoliageTypeAsset> OpenTypeFile( Assets::AssetManager& manager,
                                                                     const std::string&    path );

        // The type's scatter numbers as widgets; an edit is written to the `.defoliage` and read back, so
        // every field painted with this type sees it. Shared by the paint panel and the Details row.
        static void DrawTypeSettings( Assets::AssetManager&                          manager,
                                      const Assets::Asset<Assets::FoliageTypeAsset>& type );

        // A collection dropped on the palette (UE: a folder of FoliageTypes): each item's recorded type, or
        // the one found or made for its mesh (recorded into the collection.json), each listed once.
        static Common::BoolResultStr AddCollection( ::Desert::Core::Scene& scene, Assets::AssetManager& manager,
                                                    const std::string& manifestPath );

        // A `.defoliage` named by path listed in the palette and checked for painting (the palette command's
        // twin of dropping the file on the panel).
        static Common::BoolResultStr AddTypeFile( ::Desert::Core::Scene& scene, Assets::AssetManager& manager,
                                                  const std::string& path );

        // ----- the palette (FO-UI1): every action of the Foliage panel, shared with the palette commands -----

        // A static mesh source dropped or picked (UE: a mesh dropped on the palette): the `.defoliage` already
        // holding it with default numbers, or a new one under FOLIAGE_TYPE_PATH, listed and checked.
        static Common::BoolResultStr AddMeshFile( ::Desert::Core::Scene& scene, Assets::AssetManager& manager,
                                                  const std::string& meshSourcePath );
        // The viewport selection as a type: a foliage field's type, or the mesh a static-mesh or instanced
        // entity draws, found or made as for a dropped mesh.
        static Common::BoolResultStr AddFromEntity( ::Desert::Core::Scene& scene, Assets::AssetManager& manager,
                                                    const Common::UUID& entity );
        // UE "Select All": every instance of every checked type.
        static Common::BoolResultStr SelectAllInstances( ::Desert::Core::Scene& scene );
        // The context menu's "Select all instances" of one field.
        static Common::BoolResultStr SelectTypeInstances( ::Desert::Core::Scene& scene,
                                                          const Common::UUID&    field );
        // The field and its instances leave the palette and the scene.
        static Common::BoolResultStr RemoveType( ::Desert::Core::Scene& scene, const Common::UUID& field );
        // UE "Replace": the field paints @p typePath from now on, its instances kept; when another field already
        // paints that type the instances move into it (UE merges into the existing FoliageInfo).
        static Common::BoolResultStr ReplaceType( ::Desert::Core::Scene& scene, Assets::AssetManager& manager,
                                                  const Common::UUID& field, const std::string& typePath );
        // UE "Save as asset": the field's type copied to a new `.defoliage` beside it (Foliage::
        // SaveFoliageTypeCopy) and the field switched to the copy, so its tuning can fork.
        static Common::BoolResultStr SaveTypeCopy( ::Desert::Core::Scene& scene, Assets::AssetManager& manager,
                                                   const Common::UUID& field );
        // UE "Show in Content Browser": the type's folder, through the asset fields' one route.
        static Common::BoolResultStr ShowTypeInBrowser( ::Desert::Core::Scene& scene, const Common::UUID& field );
        // The palette's eye: the field's entity hidden or shown (VisibilityComponent, what the outliner's eye
        // writes and the ISM collector honours); nothing is removed.
        static Common::BoolResultStr ToggleTypeVisible( ::Desert::Core::Scene& scene, const Common::UUID& field );
        // The palette as a preset (Foliage::SavePalettePreset): a collection under COLLECTIONS_PATH naming every
        // listed type; applying it is AddCollection.
        static Common::BoolResultStr SavePreset( ::Desert::Core::Scene& scene, Assets::AssetManager& manager,
                                                 const std::string& name );
        // The palette's rows: every entity with a FoliageComponent beside an ISM, in scene order.
        static std::vector<ECS::Entity> PaletteFields( ::Desert::Core::Scene& scene );

        // The footprint preview on the viewport overlay: the brush ring at Core::FoliagePaint::HoverPoint and
        // how many instances one dab would add (Foliage::PreviewFoliageFootprint over the checked types). Drawn
        // for Paint only; nothing when the cursor meets no surface.
        static void DrawFootprint( ::Desert::Core::Scene& scene, const Assets::AssetManager* assetManager,
                                   const glm::mat4& viewProjection, const glm::vec2& viewportPos,
                                   const glm::vec2& viewportSize );

    private:
        void EndStroke( ::Desert::Core::Scene& scene );
        void PickAlongRay( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray, bool shift );
        // Sets the selection of each field in @p wanted (UE SelectInstances), ONE undo step; an error when
        // there is nothing to select.
        static Common::BoolResultStr
        ReplaceSelection( ::Desert::Core::Scene& scene, const std::string& label,
                          const std::unordered_map<Common::UUID, FoliageSelection>& wanted );
        static Common::BoolResultStr
        EditSelection( ::Desert::Core::Scene& scene, const std::string& label,
                       const std::function<void( std::vector<glm::mat4>&, FoliageSelection& )>& edit );

        std::optional<FoliageStroke>     m_Stroke;
        Core::FoliageTool                m_StrokeTool = Core::FoliageTool::Paint; ///< the tool at press
        bool                             m_Applied    = false; ///< Single / Select: this click already acted
        std::unordered_set<Common::UUID> m_Refused; ///< types already told why they cannot paint, this stroke

        // A mesh (by its reference) as a type: the `.defoliage` already holding it with default numbers, or a
        // new `<stem>.defoliage` (UE: dropping a mesh on the palette creates a FoliageType), then listed.
        static Common::BoolResultStr AddMeshRef( ::Desert::Core::Scene& scene, Assets::AssetManager& manager,
                                                 const Assets::AssetGuidRef& mesh, const std::string& stem );
        // Lists @p type in the palette: the foliage entity already painting it, or a new one (FoliageComponent
        // naming it, an ISM drawing its mesh). A type is listed once, as in UE's palette.
        static void AddField( ::Desert::Core::Scene& scene, Assets::AssetManager& manager,
                              const Assets::Asset<Assets::FoliageTypeAsset>& type );
    };
} // namespace Desert::Editor::Tools
