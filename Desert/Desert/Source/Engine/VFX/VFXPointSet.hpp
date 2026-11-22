#pragma once

#include <Common/Core/Serialization/GlmReflection.hpp> // rfl writes the vec4 elements of Points
#include <CoreReflection/ReflectionMacros.hpp>

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace Desert::ECS
{
    /// A set of points a PLACED system is born from (VFX-15; UE UNiagaraDataInterfaceArrayPosition, an array of
    /// positions on the component's User parameter): painted onto the level's surfaces in VFX Paint mode and read
    /// by a stack input bound to "PointSet.<Name>" (engine:ShapePointSet). Level data, so it lives on the
    /// VFXComponent and two placements of one system have their own sets. Each point is xyz = position in the
    /// placement's local space (cm), w = its weight (> 0): a particle picks a point in proportion to it.
    struct VFXPointSet
    {
        REFLECT( ScriptStruct )

        PROPERTY( DisplayName( "Name" ), Tooltip( "What a PointSet.<name> stack binding reads" ) )
        std::string Name;

        PROPERTY( DisplayName( "Points" ), Tooltip( "xyz local position (cm), w weight" ) )
        std::vector<glm::vec4> Points;

        [[nodiscard]] bool operator==( const VFXPointSet& ) const = default;
    };
} // namespace Desert::ECS
