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

        // A collection dropped on the palette (UE: a folder of FoliageTypes): each item's recorded type, or
        // the one found or made for its mesh (recorded into the collection.json), each listed once.
        static Common::BoolResultStr AddCollection( ::Desert::Core::Scene& scene, Assets::AssetManager& manager,
                                                    const std::string& manifestPath );

    private:
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
