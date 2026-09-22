#pragma once

#include "../IPanel.hpp"

#include <Common/Core/UUID.hpp>

#include <memory>
#include <string>
#include <vector>

namespace Desert::Core
{
    class Scene;
}
namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    // Scans the scene for broken asset references (a component points at a mesh / skybox handle that no
    // longer resolves) and lists them, click-to-select. Catches "I deleted the .stmesh but the scene still
    // references it" before it becomes an invisible/again-default object. View -> Scene Validation.
    class SceneValidationPanel final : public IPanel
    {
    public:
        SceneValidationPanel( std::shared_ptr<::Desert::Core::Scene> scene, Assets::AssetManager* assets );

        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 420.0f, 300.0f };
        }
        void OnUIRender() override;

    private:
        struct Issue
        {
            Common::UUID Entity;
            std::string  Text;
        };

        void Validate();

        std::shared_ptr<::Desert::Core::Scene> m_Scene;
        Assets::AssetManager*                  m_Assets = nullptr;
        std::vector<Issue>                     m_Issues;
        bool                                   m_Ran = false;
    };
} // namespace Desert::Editor
