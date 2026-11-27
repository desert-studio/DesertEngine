#pragma once

// NG-RT-REFRACTION contract (design F:/aftmp/NG-RT-REFRACTION-DESIGN.md). Owned by nobody while the packages run.
//
// RAY-TRACED TRANSLUCENCY (UE RayTracedTranslucency: refraction rays from the translucent front layer, lit at the
// hit): one graph pass, "Translucency: Ray Traced Refraction", that starts a ray at every pixel of the front layer
// G-buffer, lets the surface hit groups answer each dielectric interface (DesertRayPayload Transmission / Ior),
// follows Snell through at most `MaxInterfaces` of them, lights the first opaque hit as the hit-lit reflections do
// (Common/GIHitLighting.glslh) and writes what the glass sees THROUGH itself. The glass cell
// (Mesh/Surface/Pass_Forward_Translucent.glslh) reads the result where its fragment is the front layer.
//
// Package A implements the class (RayTracedTranslucency.cpp) and calls it from the frame.
// Package B writes the ray generation program and the glass cell's read.
// Package C binds `RefractionOutputs::Radiance` into the glass draw (MeshRenderer) by the NAME below.

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Systems/Scene/Shadows/RayTracedSunShadow.hpp> // ScreenShadowMaskInputs: depth, normal, TLAS

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string_view>

namespace Desert::Graphic::System
{
    // The program and its texture names: one statement for the C++ side and the shaders.
    inline constexpr std::string_view kRayTracedTranslucencyProgram = "Programs/Translucency/RayTracedTranslucency.shader";
    inline constexpr std::string_view kRayTracedTranslucencyPass    = "Translucency: Ray Traced Refraction";
    // The glass cell's texture (bound by name, the number is the program build's): rgb = the pre-exposed radiance
    // seen through the surface, already multiplied by the path's transmittance; a = 1 where the trace answered,
    // 0 where it did not (the cell then reads the scene copy, the ScreenSpace tier's statement).
    inline constexpr std::string_view kTranslucencyRefractionTexture = "TranslucencyRefractionTexture";
    // The output's format name in ViewTargetFormats terms is the scene colour's (linear HDR, pre-exposed).

    // Translucency.Refraction of Common::Scalability (package A adds the row): what answers the refraction.
    enum class TranslucencyRefractionMethod : uint8_t
    {
        ScreenSpace    = 0, // the scene copy bent on screen (every tier without a TLAS, and the Forward path)
        HardwareTraced = 1, // this pass
    };

    // The pass's push constants / uniform block, std140 order. Package B declares the same members in the program.
    struct RayTracedTranslucencyParams
    {
        glm::mat4 InvJitteredViewProjection{ 1.0f };
        glm::vec4 CameraPosition{ 0.0f };   // xyz world (cm), w = PreExposure
        float     MaxRayDistance  = 30000.0f; // cm, each segment
        float     MaxRoughness    = 0.4f;     // above it the surface is not traced (a = 0)
        uint32_t  MaxInterfaces   = 4;        // dielectric interfaces followed before the path stops (UE MaxPrimaryHitEvents)
        uint32_t  FrameIndex      = 0;        // the rough lobe's sequence
    };

    // What the pass reads. Depth / Normal of `Surface` are the FRONT LAYER's (FrontLayerSurface::Depth / ::Normal),
    // Tlas / Metadata the frame's ray tracing scene, Width / Height the render resolution.
    struct RayTracedTranslucencyInputs
    {
        ScreenShadowMaskInputs Surface;
        RDG::TextureRef        SceneCopy;  // read where a path leaves the TLAS range or the budget of interfaces
    };

    struct RefractionOutputs
    {
        RDG::TextureRef Radiance; // kTranslucencyRefractionTexture
    };

    // One per view (a member of the view's renderer, as RayTracedSunShadowRenderer). No state across frames.
    class RayTracedTranslucencyRenderer
    {
    public:
        RayTracedTranslucencyRenderer();
        ~RayTracedTranslucencyRenderer();
        RayTracedTranslucencyRenderer( const RayTracedTranslucencyRenderer& )            = delete;
        RayTracedTranslucencyRenderer& operator=( const RayTracedTranslucencyRenderer& ) = delete;

        // Builds the ray tracing pipeline over the scene's hit groups. The error names what is missing (no ray
        // tracing device, the program did not compile); the caller logs it once and the view stays ScreenSpace.
        [[nodiscard]] Common::BoolResultStr Init();

        // Adds the pass to @p graph. The error is the frame's reason for having no traced refraction.
        [[nodiscard]] Common::ResultStr<RefractionOutputs> AddPass( RDG::Builder& graph,
                                                                    const RayTracedTranslucencyInputs& in,
                                                                    const RayTracedTranslucencyParams& params );

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
} // namespace Desert::Graphic::System
