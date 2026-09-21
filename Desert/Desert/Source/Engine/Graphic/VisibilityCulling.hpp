#pragma once

#include <Common/Core/Math/AABB.hpp>

#include <Engine/Core/Frustum.hpp>
#include <Engine/Geometry/MeshBounds.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    // WHETHER A PLACED MESH IS WORTH SUBMITTING. One rule, in a header, so the DECISION is reachable by a
    // test: 85 % of this engine's translation units are compiled by no suite, and a rule left inline in
    // MeshRenderer.cpp would have been one of them (see scripts/CI/UnreachedSources.sh).
    //
    // THE DEFAULT IS TO DRAW. A mesh with no extent — no submeshes, or bounds that were never filled in —
    // answers "I do not know where this is", and that must never be answered with "so it is not there".
    // Every failure of this function has to land on the side that costs a draw call, because the other
    // side costs a hole in the picture that nothing reports and only some camera angles show.
    [[nodiscard]] inline bool IsVisibleInView( const Core::Frustum& frustum, const glm::mat4& transform,
                                               const Common::Math::AABB& localBounds )
    {
        if ( Geometry::IsEmpty( localBounds ) )
        {
            return true;
        }
        return frustum.Intersects( Geometry::TransformBounds( transform, localBounds ) );
    }
} // namespace Desert::Graphic
