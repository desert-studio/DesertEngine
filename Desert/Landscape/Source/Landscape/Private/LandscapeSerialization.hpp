#pragma once

// WHAT THE LANDSCAPE MODULE REGISTERS FOR THE SCENE FILE (Landscape::StartupModule calls both): its hand-mapped
// component serializers into Engine's component provider (LandscapeComponentSerializers.cpp) and its tile files
// beside a scene through the scene load/save hooks (LandscapeSceneFiles.cpp).

namespace Desert::Landscape
{
    void RegisterComponentSerializers();
    void RegisterSceneFiles();
} // namespace Desert::Landscape
