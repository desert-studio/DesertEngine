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
#include <unordered_set>
#include <vector>

namespace Desert::Editor::Tools
{
    // UE5-style foliage painting, extracted from ViewportPanel (god-object split). The tool holds only the
    // stroke in flight (its random stream, the fields as they were at press for undo) — brush/selection live in
    // Editor::Core::FoliagePaint; WHAT is painted is the `.defoliage` each foliage entity names
    // (Assets::FoliageTypeAsset, FO-1); the host supplies the scene, asset manager and ray.
    class FoliagePaintTool
    {
    public:
        // Floating panel: Paint/Erase tabs, brush sliders, type list (multi-select), per-type settings.
        // viewportPos = the scene-image top-left (for placing the floating window).
        void DrawPanel( ::Desert::Core::Scene& scene, const Assets::AssetManager* assetManager,
                        const glm::vec2& viewportPos );

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

    private:
        void EndStroke( ::Desert::Core::Scene& scene );
        void PickAlongRay( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray, bool shift );
        static Common::BoolResultStr
        EditSelection( ::Desert::Core::Scene& scene, const std::string& label,
                       const std::function<void( std::vector<glm::mat4>&, FoliageSelection& )>& edit );

        std::optional<FoliageStroke>     m_Stroke;
        Core::FoliageTool                m_StrokeTool = Core::FoliageTool::Paint; ///< the tool at press
        bool                             m_Applied    = false; ///< Single / Select: this click already acted
        std::unordered_set<Common::UUID> m_Refused; ///< types already told why they cannot paint, this stroke

        // A dropped mesh finds the `.defoliage` already holding it with default numbers, or becomes a new one
        // (UE: dropping a mesh on the foliage palette creates a FoliageType asset), then lists it.
        static void CreateTypeFromMesh( ::Desert::Core::Scene& scene, Assets::AssetManager& manager,
                                        const std::string& meshSourcePath );
        // Lists @p type in the palette: the foliage entity already painting it, or a new one (FoliageComponent
        // naming it, an ISM drawing its mesh). A type is listed once, as in UE's palette.
        static void AddField( ::Desert::Core::Scene& scene, Assets::AssetManager& manager,
                              const Assets::Asset<Assets::FoliageTypeAsset>& type );
    };
} // namespace Desert::Editor::Tools
