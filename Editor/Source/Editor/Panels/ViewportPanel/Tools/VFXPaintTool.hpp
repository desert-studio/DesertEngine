#pragma once

// VFX-15: VFX Paint mode's Painted Set brush (mockup Tools/UIMockup VfxPaintMode, wiki VFX-15 §3). UE has no
// "paint an effect" tool - its emission sources are data interfaces (ArrayPosition, Spline, Static Mesh) - so the
// brush is the ported Foliage brush (FoliageBrushAdd: the traced, density-limited dab), writing the points into
// the selected VFXComponent's point set (VFXComponentData::PointSets) instead of mesh instances. One press to
// release = one undo step. A stack input bound to PointSet.<set> (engine:ShapePointSet) spawns from it.

#include <Common/Core/Math/Ray.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>
#include <Engine/Core/Scene.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Editor
{
    struct PaletteCommand;
}

namespace Desert::Editor::Tools
{
    class VFXPaintTool final
    {
    public:
        /// The brush (mockup BRUSH section). Density is points per square metre of brush disc (UE PaintDensity
        /// times the type's Density, here one number).
        struct Settings
        {
            float       Radius  = 100.0f; ///< cm
            float       Density = 4.0f;   ///< points / m²
            std::string Set     = "PaintedPoints";
        };
        static Settings& Brush();
        static constexpr float kDensityLow  = 4.0f;
        static constexpr float kDensityHigh = 40.0f;

        /// One frame of the brush: a press over a surface starts a stroke on the selected VFX entity, each held
        /// frame adds a dab at the ray's hit, the release pushes the stroke as one undo step.
        void Update( ::Desert::Core::Scene& scene, const Common::Math::Ray& ray, bool pressing );
        [[nodiscard]] bool IsStroking() const { return m_Stroke.has_value(); }

        /// The mode panel (TARGET + BRUSH) at the viewport's top-left.
        void DrawPanel( ::Desert::Core::Scene& scene, const glm::vec2& viewportPos );
        /// Every point of the selected VFX entity's sets as a marker, and the brush ring at @p hover.
        static void DrawPoints( ::Desert::Core::Scene& scene, const glm::mat4& viewProjection,
                                const glm::vec2& viewportPos, const glm::vec2& viewportSize );

        /// The selected entity when it has a VFXComponent, else why not.
        static Common::ResultStr<Common::UUID> Target( ::Desert::Core::Scene& scene );

        static void AppendCommands( std::vector<PaletteCommand>& commands );

    private:
        struct Stroke
        {
            Common::UUID           Entity;
            std::vector<glm::vec4> Before;
            bool                   HadSet = false;
            uint64_t               Seed   = 0;
            uint32_t               Dabs   = 0;
        };
        std::optional<Stroke> m_Stroke;
        uint64_t              m_Strokes = 0;
    };
} // namespace Desert::Editor::Tools
