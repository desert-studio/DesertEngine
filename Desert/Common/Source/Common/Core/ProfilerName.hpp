#pragma once

// The profiler's interned row name, alone: what a cached plan keeps per pass (RDG CompiledPass) without pulling
// in the profiler (Profiler.hpp includes Optick).

#include <cstdint>

namespace Common::Profiling
{
    // THE INTERNED SCOPE NAME (UE: a stat's FName / TStatId). Every row of the profiler is keyed by a small id
    // the name table hands out once per distinct name; a closing scope adds to its row BY ID, so no scope end
    // hashes, compares or allocates a string. Text is the table's copy of the name: the table never frees or
    // moves one, so it is valid for the process' life (Optick's dynamic events and the published rows read it).
    // Ids are global (Profiler::InternScopeName), so one id is valid on every thread.
    inline constexpr uint32_t kNoScopeName = UINT32_MAX;
    struct InternedScopeName
    {
        uint32_t    Id   = kNoScopeName;
        const char* Text = nullptr;
        bool        operator==( const InternedScopeName& ) const = default;
    };
} // namespace Common::Profiling
