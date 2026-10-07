#pragma once

#include <cstdint>

namespace Desert::Core
{
    /**
     * WHERE A SHADER GRAPH'S OWN RESOURCES LIVE IN DESCRIPTOR SET 0 — one window, for every domain.
     *
     * A graph's text is always compiled INTO a program the graph does not own. A Surface or PostProcess
     * graph becomes a generated `.shader` that includes the engine's own lighting headers; a Volume graph
     * becomes the cloud medium that four shipped programs are compiled against
     * (Docs/Clouds/O1_DESIGN.md §12.2). Either way the graph declares GLSL beside declarations it cannot
     * see, and a graph author cannot be asked to know what they are.
     *
     * TWO DECLARATIONS ON ONE DESCRIPTOR SLOT IS NOT A COMPILE ERROR. glslang emits both Binding
     * decorations with no diagnostic at all, `-Werror` included (measured with glslc 2026-09-08, Г17).
     * The engine refuses it after compilation — ShaderReflection::ReflectStage names both resources and
     * VulkanShader::Reflect drops the shader — but that refusal arrives when a material is applied, in
     * front of the person applying it. A reservation is what keeps it from arriving at all.
     *
     * WHY THE NUMBER CANNOT BE DERIVED IN C++, which is the first thing to reach for. The occupancy of a
     * program lives in GLSL text, spread across included headers and macros, and is only a set of numbers
     * after shaderc has run — Г17 established exactly that when it moved the collision check to
     * reflection. So the number here is a RESERVATION and the claim that it is free is a MEASUREMENT:
     * `Desert/Tests/Engine/ShaderCacheKey` compiles every pass of every stage of every shipped `.shader`
     * with shaderc, reflects the SPIR-V with the engine's own reflection, and asserts that not one set-0
     * binding falls at or above this value. It names no engine binding and counts nothing, so a slot
     * added tomorrow reddens it whatever spelled the number.
     *
     * 24 WAS ONE ABOVE THE HIGHEST BINDING THE TREE DECLARED, and that was not comfortable headroom —
     * it was none; the window now opens at 27, above the scene-read range below. The lit shader-graph surface layout reaches 23 (the fourth cascade shadow map in
     * Programs/Graph/MatLitConst.shader), so the next binding that layout grows lands here. That is what
     * the census is for: the collision becomes a red test rather than a wrong picture.
     *
     * MOVING THIS VALUE IS NOT FREE. The DSL spells a texture base as a literal inside the GENERATED
     * `.shader` (`Properties ... TextureBinding(24)`), so a `.shader` generated before a move keeps the
     * old number until its `.dgraph` is compiled again. No generated shader in this repository declares a
     * texture today, so there is no stale artifact to fix; whoever raises this value owns finding out
     * whether that is still true.
     */
    /**
     * THE RESERVED SCENE-READ RANGE: set-0 slots a VIEW-pass surface cell reads per-(frame x view) scene data at,
     * bound by name by Graphic::PBRSceneFrame::ApplyTo — the view's per-primitive motion rows (`ObjectMotions`)
     * and the view's bone palettes (`ObjectBones`), both declared by Common/ObjectMotion.glslh (TAA1 step 4).
     *
     * They need fixed numbers because one header declares them into EVERY view-pass cell of EVERY surface
     * template, beside a template's own hand-numbered textures and the forward pass's lighting slots, which
     * together already fill 0-24 (measured 2026-10-07: SpotLightsUB 16, InstanceTransforms 17, emissive 24).
     * So the range sits ABOVE the engine's last slot and BELOW the graph's window, which moved up to make room.
     *
     * A hope is not a guarantee, so two seams hold it: DShaderParser refuses a Properties `Binding(n)` /
     * `TextureBinding(n)` whose row or texture run reaches into this range (a named parse error), and
     * Tests/Engine/VelocityTarget asserts that over the whole shipped tree nothing but the two scene-read
     * resources is declared at these numbers, and that ObjectMotion.glslh spells exactly these numbers.
     */
    inline constexpr uint32_t kSceneReadBindingFirst  = 25u;
    inline constexpr uint32_t kObjectMotionsBinding   = kSceneReadBindingFirst;      // Common/ObjectMotion.glslh
    inline constexpr uint32_t kObjectBonesBinding     = kSceneReadBindingFirst + 1u; // Common/ObjectMotion.glslh
    inline constexpr uint32_t kSceneReadBindingCount  = 2u;

    [[nodiscard]] constexpr bool IsSceneReadBinding( uint32_t binding )
    {
        return binding >= kSceneReadBindingFirst && binding < kSceneReadBindingFirst + kSceneReadBindingCount;
    }

    // Raised from 24 (TAA1 step 4) to sit above the scene-read range; see the paragraph on moving it above.
    inline constexpr uint32_t kGraphOwnedBindingFirst = kSceneReadBindingFirst + kSceneReadBindingCount;
    static_assert( kGraphOwnedBindingFirst == 27u, "the graph's window opens right above the scene-read range" );
} // namespace Desert::Core
