#pragma once

// Compiles Engine/Content/Shaders/Common/CloudGeometry.glslh AS C++, so that this suite holds the
// PROCEDURAL GENERATOR against the march's own step schedule rather than against a copy of its numbers.
//
// WHY THE GENERATOR NEEDS THE SCHEDULE. What the generator places in the sky and what the march can find
// there are two quantities obliged to agree, and this programme has been bitten by that pair twice already
// — once when a cirrus' placement scale put three quarters of its chords inside one search step, and once
// when the shell's envelope moved the step underneath a library that had been calibrated against the old
// one. The generator now clamps every lump against CloudFinestResolvableChordKm, and this header is what
// lets the clamp be asserted against the SHADER's constant instead of against a number typed twice.
//
// Dialect shim as in Desert/Tests/Engine/CloudType/CloudScheduleReference.hpp: glm supplies the vector
// types and the built-ins, and the include sits inside an anonymous namespace because the shared text
// carries no `inline` — GLSL has none.

#include <glm/glm.hpp>
#include <Common/Core/GlslAsCpp.hpp>

namespace Desert::Tests::CloudProceduralScheduleRef
{
    namespace
    {
        using vec2 = glm::vec2;
        using vec3 = glm::vec3;

        using glm::clamp;
        using glm::dot;
        using glm::length;
        using glm::max;
        using glm::min;
        using glm::sqrt;

        DESERT_GLSL_AS_CPP_BEGIN // see the header: GLSL has no `inline`, so these are statics
#include <Common/CloudGeometry.glslh>
             DESERT_GLSL_AS_CPP_END

    } // namespace
} // namespace Desert::Tests::CloudProceduralScheduleRef
