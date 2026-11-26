#pragma once

#include "PatchImage.hpp"

#include <span>

namespace Desert::Editor::LiveCoding
{
    /// /FUNCTIONPADMIN hotpatch: `E9 rel32` into the 5 bytes before the entry, then `EB F9` (jmp -7) at the entry.
    /// Every other thread of the process is suspended (handles opened and the snapshot taken BEFORE, nothing
    /// allocated while suspended); a thread whose RIP is inside [entry-5, entry+2) of any redirect is resumed and
    /// the whole set retried after 1 ms, at most 50 times, then refused. Refuses an entry whose 5 bytes before are
    /// not CC/90 and not an earlier redirect (the image was linked without /FUNCTIONPADMIN).
    [[nodiscard]] Common::BoolResultStr ApplyRedirects( std::span<const Redirect> redirects ) noexcept;
} // namespace Desert::Editor::LiveCoding
