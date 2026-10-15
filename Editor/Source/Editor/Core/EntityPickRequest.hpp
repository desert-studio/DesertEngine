#pragma once

#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <optional>

// THE EYEDROPPER — UE's "pick actor from scene" button beside an actor reference: a Details field arms it, the
// NEXT left click on a viewport's scene image picks the entity under the cursor for that field instead of
// selecting it, and the field takes the answer on its next frame. One request at a time, editor-wide: arming
// another field (or Esc, or a click that hits nothing) ends the previous one.
namespace Desert::Editor::EntityPickRequest
{
    /// Arms the eyedropper for `owner` (the field's ImGui id).
    void Arm( std::uint32_t owner );

    /// Ends any armed request without an answer.
    void Cancel();

    /// Whether a request is armed (the viewport then picks for it instead of selecting).
    [[nodiscard]] bool IsArmed();

    /// Whether the armed request is `owner`'s.
    [[nodiscard]] bool IsArmedFor( std::uint32_t owner );

    /// The viewport's answer: the entity the click hit, or nullopt for a click on nothing (which ends the
    /// request with no answer).
    void Deliver( std::optional<Common::UUID> picked );

    /// `owner`'s answer, once: the picked entity, after which the request is over.
    [[nodiscard]] std::optional<Common::UUID> TakeFor( std::uint32_t owner );
} // namespace Desert::Editor::EntityPickRequest
