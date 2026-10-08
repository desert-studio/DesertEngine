#pragma once

#include "../IPanel.hpp"

#include <Engine/Destruction/FractureEdit.hpp>

#include <memory>

namespace Desert::Editor
{
    /**
     * @brief The Fracture mode's tool panel (DST-02) — UE's Fracture Mode toolkit: the target `.dfrac`, the
     *        Generate section (seed, one row per fracture level: Uniform / Clustered Voronoi, Planar, Brick), the
     *        interior material, and the View section (Explode Amount, Fracture Level) with its preview.
     *
     * Contextual like the Landscape and Modeling panels: it exists while the viewport is in Fracture mode. Every
     * edit of the asset goes through Engine/Destruction/FractureEdit.hpp and FractureAsset::WriteStep, one undo
     * step each, through FractureTool (the state the palette drives too); the View section is preview state (UE
     * UFractureSettings) and is never written to the asset.
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
        /// Every frame, open or not: the selected entity previews the tool's fracture while the mode is open
        /// (SyncFracturePreview), and nothing does once it closes or the selection moves.
        void OnPreUpdate() override;
        /// A scene the panel leaves keeps no preview.
        void SetScene( const std::shared_ptr<Desert::Core::Scene>& scene ) override;

    private:
        void DrawTarget();
        void DrawGenerate();
        void DrawLevel( size_t index, Destruction::FractureLevelSettings& level );
        void DrawPlanes( Destruction::FractureLevelSettings& level );
        void DrawInteriorMaterial();
        void DrawView();

        std::weak_ptr<Desert::Core::Scene> m_Scene;
    };
} // namespace Desert::Editor
