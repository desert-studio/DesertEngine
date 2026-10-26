#pragma once

#include <ImGui/imgui.h>

#include <Engine/Desert.hpp>

namespace Desert::Editor
{
    class Widgets
    {
    public:
        static void DrawVec3Control( const std::string& label, glm::vec3& values, float resetValue = 0.0f );
    };
} // namespace Desert::Editor