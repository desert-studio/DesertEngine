#pragma once

#include "../IPanel.hpp"

#include <Editor/Core/AssetReferences.hpp>

#include <memory>
#include <string>
#include <vector>

namespace Desert
{
    namespace Core
    {
        class Scene;
    }
    namespace Assets
    {
        class AssetManager;
    }
} // namespace Desert

namespace Desert::Editor
{
    // "Asset References" — a read-only usage browser over the open project. Rebuild scans the Assets
    // tree into an AssetReferenceIndex (file-level, token-based: any TEXT asset — scene, prefab,
    // material, graph, script — can be a referencer). On top of that the panel shows RUNTIME usage
    // the text scan can't see: mesh ASSETS whose embedded material list uses the asset (meshes are
    // binary) and the open-scene entities rendering with it right now. Also lists leaf assets
    // nothing references (cleanup candidates). Hidden by default; enable via View -> Asset References.
    class AssetReferencesPanel final : public IPanel
    {
    public:
        AssetReferencesPanel( const std::shared_ptr<::Desert::Core::Scene>& scene,
                              const std::shared_ptr<Assets::AssetManager>& assetManager )
             : IPanel( "Asset References", /*showPanel=*/false ), m_Scene( scene ),
               m_AssetManager( assetManager )
        {
        }

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 460.0f, 560.0f };
        }

        void OnUIRender() override;

    private:
        void Rebuild();
        // Runtime usage of the selected asset: mesh-asset material lists + open-scene entities.
        void DrawRuntimeUsage( const std::string& relPath );

        std::shared_ptr<::Desert::Core::Scene> m_Scene;
        std::shared_ptr<Assets::AssetManager> m_AssetManager;

        AssetReferenceIndex      m_Index;
        bool                     m_Built = false;
        std::vector<std::string> m_AllPaths;   // sorted entry paths for the lookup combo
        int                      m_Selected = -1;
        std::vector<std::string> m_Orphans;    // cached from the last Rebuild
    };
} // namespace Desert::Editor
