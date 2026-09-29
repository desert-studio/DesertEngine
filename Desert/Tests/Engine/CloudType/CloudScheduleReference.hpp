#pragma once

// Compiles Engine/Content/Shaders/Common/CloudGeometry.glslh AS C++, so that this suite — which owns
// the shipped `.decloudtype` library — can hold that library against the march's OWN step schedule
// rather than against a copy of its numbers.
//
// WHY THE LIBRARY NEEDS THE SCHEDULE AT ALL. What a type places in the sky and what the march can find
// there are two quantities obliged to agree, each of them individually reasonable, and nothing checked
// that they did: a placement scale of 0.35 is a perfectly good cirrus and a saturation distance of 15 km
// is a perfectly good schedule, and together they put three quarters of that type's chords inside a
// single search step. That is the §2.3.1 defect class exactly, so it is asserted here, on both sides at
// once, in the suite that can see both.
//
// Dialect shim as in Desert/Tests/Engine/CloudGeometry/CloudGeometryReference.hpp: glm supplies the
// vector types and the built-ins, and the include sits inside an anonymous namespace because the shared
// text carries no `inline` — GLSL has none.

#include <glm/glm.hpp>
#include <Common/Core/GlslAsCpp.hpp>

namespace Desert::Tests::CloudScheduleRef
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
} // namespace Desert::Tests::CloudScheduleRef
