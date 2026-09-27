#pragma once

#include <Engine/Desert.hpp>
#include <Engine/Assets/FoliageTypeAsset.hpp>
#include <Common/Core/Math/Ray.hpp>

#include <glm/glm.hpp>

#include <string>

namespace Desert::Editor::Tools
{
    // UE5-style foliage painting, extracted from ViewportPanel (god-object split). The tool is stateless —
    // brush/selection live in Editor::Core::FoliagePaint; WHAT is painted is the `.defoliage` each foliage
    // entity names (Assets::FoliageTypeAsset, FO-1); the host supplies the scene, asset manager and ray.
    class FoliagePaintTool
    {
    public:
        // Floating panel: Paint/Erase tabs, brush sliders, type list (multi-select), per-type settings.
        // viewportPos = the scene-image top-left (for placing the floating window).
        void DrawPanel( ::Desert::Core::Scene& scene, const Assets::AssetManager* assetManager,
                        const glm::vec2& viewportPos );

        // Paint/erase every CHECKED foliage type under the cursor ray (one dab). Surface hit comes from the
        // engine-owned Scene::Raycast (no duplicated raycast / mesh resolution).
        void Paint( ::Desert::Core::Scene& scene, const Assets::AssetManager* assetManager,
                    const Common::Math::Ray& ray );

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

    private:
        // A dropped mesh becomes a new `.defoliage` beside the others (UE: dropping a mesh on the foliage
        // palette creates a FoliageType asset), then a field painted with it.
        void CreateTypeFromMesh( ::Desert::Core::Scene& scene, Assets::AssetManager& manager,
                                 const std::string& meshSourcePath );
        // A new foliage entity painted with @p type: FoliageComponent naming it, an ISM drawing its mesh.
        void AddField( ::Desert::Core::Scene& scene, Assets::AssetManager& manager,
                       const Assets::Asset<Assets::FoliageTypeAsset>& type );
    };
} // namespace Desert::Editor::Tools
