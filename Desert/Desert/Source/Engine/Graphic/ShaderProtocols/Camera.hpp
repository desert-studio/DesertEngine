#pragma once

#include <glm/glm.hpp>

#include <string>

namespace Desert::Graphic::ShaderProtocols
{
    struct Camera
    {
        inline const static std::string Name = "CameraUB";

        glm::mat4 Projection;
        glm::mat4 View;
        glm::vec3 CameraPos;
    };
} // namespace Desert::Graphic::ShaderProtocols
