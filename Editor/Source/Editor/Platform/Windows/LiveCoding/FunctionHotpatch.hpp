#pragma once

#include "PatchImage.hpp"

#include <cstdint>
#include <span>

namespace Desert::Editor::LiveCoding
{
    /// The 5 bytes before @p entry are hotpatch padding (CC/90) or already an earlier redirect.
    [[nodiscard]] bool HasHotpatchRoom( std::uintptr_t entry ) noexcept;

    /// /FUNCTIONPADMIN hotpatch: `E9 rel32` into the 5 bytes before the entry, then `EB F9` (jmp -7) at the entry.
    /// Every other thread of the process is suspended (handles opened and room reserved BEFORE, nothing allocated
    /// while suspended), and snapshots are taken again until one shows no thread the set lacks; a thread whose RIP
    /// is inside [entry-5, entry+2) of any redirect, or whose context cannot be read, resumes the whole set for a
    /// retry after 1 ms, at most 50 times, then refused. Refuses an entry whose 5 bytes before are
    /// not CC/90 and not an earlier redirect (the image was linked without /FUNCTIONPADMIN).
    [[nodiscard]] Common::BoolResultStr ApplyRedirects( std::span<const Redirect> redirects ) noexcept;
} // namespace Desert::Editor::LiveCoding
