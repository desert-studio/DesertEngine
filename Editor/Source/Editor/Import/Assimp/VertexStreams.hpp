#pragma once

#include <glm/glm.hpp>
#include <array>
#include <cstdint>

#include <vector>

struct aiMesh;
struct aiScene;

namespace Desert::Editor
{
    // THE OPTIONAL VERTEX STREAMS OF AN IMPORT (MAT1v, UE FColorVertexBuffer and the second UV channel): the
    // file's first colour set (glTF COLOR_0) and its second UV set (glTF TEXCOORD_1). A stream is kept for the
    // whole mesh asset when ANY of the scene's meshes carries it, and a mesh without it then contributes the
    // neutral value (white, UV 0,0) for each of its vertices, so the stream stays one entry per vertex.
    struct SceneVertexStreams
    {
        bool Colors = false;
        bool UV1    = false;
    };

    [[nodiscard]] SceneVertexStreams StreamsOf( const aiScene& scene );

    // Appends @p mesh's vertices to the streams @p streams says the asset keeps. Colours are LINEAR RGBA8: glTF
    // states COLOR_0 as linear, assimp hands it over as floats, and each channel is clamped and rounded.
    void AppendVertexStreams( const aiMesh& mesh, const SceneVertexStreams& streams,
                              std::vector<std::array<uint8_t, 4>>& colors, std::vector<glm::vec2>& uv1 );
} // namespace Desert::Editor
