#pragma once

#include "../IPanel.hpp"

#include <Engine/Destruction/FractureEdit.hpp>

#include <array>
#include <memory>
#include <string>

namespace Desert::Editor
{
    /**
     * @brief The Fracture mode's tool panel (DST-02) — UE's Fracture Mode toolkit: the target `.dfrac`, the
     *        Generate section (seed, one row per fracture level: Uniform / Clustered Voronoi, Planar, Brick), the
     *        interior material, and the View section (Explode Amount, Fracture Level) with its preview.
     *
     * Contextual like the Landscape and Modeling panels: it exists while the viewport is in Fracture mode. Every
     * edit of the asset goes through Engine/Destruction/FractureEdit.hpp and FractureAsset::WriteStep, one undo
     * step each; the View section is preview state (UE UFractureSettings) and is never written to the asset.
     */
    class FracturePanel final : public IPanel
    {
    public:
        explicit FracturePanel( const std::shared_ptr<Desert::Core::Scene>& scene );

        void OnUIRender() override;
        bool IsContextual() const override
        {
            return true;
        }
        bool IsRelevant() const override;
        void SetScene( const std::shared_ptr<Desert::Core::Scene>& scene ) override
        {
            m_Scene = scene;
        }

    private:
        void DrawTarget();
        void DrawGenerate();
        void DrawLevel( size_t index, Destruction::FractureLevelSettings& level );
        void DrawInteriorMaterial();
        void DrawView();

        void Load();
        void Generate();
        /// Writes @p next as the target file through one undo step, then re-reads it.
        void Commit( const Destruction::FractureData& next, const char* label );

        std::weak_ptr<Desert::Core::Scene> m_Scene;
        /// The target `.dfrac`, relative to the project's Assets folder.
        std::array<char, 512> m_Path{ "Fractures/Fracture.dfrac" };
        /// The fracture as the file holds it (empty until Load or Generate).
        Destruction::FractureData m_Fracture;
        bool                      m_Loaded = false;
        /// The settings the next Generate bakes with; Load takes them from the file.
        Destruction::FractureSettings m_Settings;
        /// Preview only (UE UFractureSettings::ExplodeAmount / FractureLevel).
        Destruction::FractureViewSettings m_View;
        std::array<char, 40>              m_InteriorGuid{};
        std::string                       m_Status;
    };
} // namespace Desert::Editor
