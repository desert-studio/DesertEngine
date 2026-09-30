#pragma once

#include <imgui.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Editor
{
    /**
     * @brief THE ONE HOME OF BONES DRAWN OVER A PICTURE (UE's skeleton draw in the level viewport and in Persona).
     *
     * Octahedral parent->child links + sphere joints, from already-projected absolute-screen bone heads
     * (nullopt = behind the camera); `parents[i] < 0` marks a root. The level viewport's Skeleton Edit overlay,
     * the RigBuilder overlay and the Animation Editor's preview (Skeleton / Mesh / Animation modes) all draw
     * through it, so a bone looks and picks the same everywhere. When `pickRecord` is given it is refilled with
     * (bone, head) for PickBoneOverlay.
     */
    void DrawBoneOverlay( ImDrawList* drawList, const std::vector<std::optional<ImVec2>>& screen,
                          const std::vector<int>& parents, const std::vector<std::string>& names, int selectedBone,
                          bool showAllNames, std::vector<std::pair<int, ImVec2>>* pickRecord );

    /// The bone whose recorded head is nearest `absMouse` within `radiusPx`; -1 when none.
    [[nodiscard]] int PickBoneOverlay( const std::vector<std::pair<int, ImVec2>>& recorded, const ImVec2& absMouse,
                                       float radiusPx = 12.0f );
} // namespace Desert::Editor
