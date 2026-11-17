#pragma once

#include <glm/glm.hpp>

namespace Desert::ECS
{
    // THE RECT LIGHT'S SHAPE IN THE WORLD, ONE ANSWER (UE URectLightComponent: FRectLightSceneProxy's rect and
    // the editor's DrawRectLight wireframe read the same SourceWidth/Height and BarnDoor fields). RectLightSystem
    // hands the renderer this frame and LightGizmoRenderer draws it, so the rectangle a person sees selected is
    // the rectangle Mesh/LightSources.glslh lights by - not a second derivation that could face another way.
    //
    // The face looks along the entity's forward (-Z of the world rotation), as a spot's cone; width runs along
    // the entity's X, and Y completes the frame so that X x Y is the face's normal (LightSources.glslh takes the
    // cross as the emitting side). The world transform's scale does not size the rect: SourceWidth/Height do.
    struct RectLightGeometry
    {
        glm::vec3 Centre{ 0.0f };
        glm::vec3 Forward{ 0.0f, 0.0f, -1.0f };
        glm::vec3 AxisX{ 1.0f, 0.0f, 0.0f };
        glm::vec3 AxisY{ 0.0f, 1.0f, 0.0f };
        float     HalfWidth  = 0.0f;
        float     HalfHeight = 0.0f;
        // UE barn doors: hinged on the rect's four edges, BarnDoorAngle from the normal (90 = folded flat into
        // the rect's plane, no effect), BarnDoorLength long. Their free edge is Reach outward in the plane and
        // Depth along the normal - the two numbers the shader clips by.
        float BarnReach = 0.0f;
        float BarnDepth = 0.0f;
    };

    [[nodiscard]] inline RectLightGeometry RectLightGeometryOf( const glm::mat4& worldTransform, float sourceWidth,
                                                                float sourceHeight, float barnDoorAngleDeg,
                                                                float barnDoorLength ) noexcept
    {
        RectLightGeometry g;
        g.Centre     = glm::vec3( worldTransform[3] );
        g.Forward    = glm::normalize( -glm::vec3( worldTransform[2] ) );
        g.AxisX      = glm::normalize( glm::vec3( worldTransform[0] ) );
        g.AxisY      = glm::cross( g.Forward, g.AxisX );
        g.HalfWidth  = 0.5f * glm::max( sourceWidth, 0.0f );
        g.HalfHeight = 0.5f * glm::max( sourceHeight, 0.0f );

        const float barnAngle  = glm::radians( glm::clamp( barnDoorAngleDeg, 0.0f, 90.0f ) );
        const float barnLength = glm::max( barnDoorLength, 0.0f );
        g.BarnReach            = barnLength * glm::sin( barnAngle );
        g.BarnDepth            = barnLength * glm::cos( barnAngle );
        return g;
    }
} // namespace Desert::ECS
