// UE Core shim for the ported GeometryCore (DynamicMesh3 and its containers).
// Not a port of a single UE file: it re-expresses, over std and glm, the subset of UE Core
// (Runtime/Core: Containers/Array.h, ArrayView.h, Set.h, Map.h) that the ported sources still call under UE
// names. It is being retired step by step (GC1: the integer aliases, FName, TFunction, FMath, MoveTemp and
// TNumericLimits are gone -- the sources spell std directly; GC2: the vectors are glm, members .x/.y/.z/.w;
// GC4: check/checkSlow/checkf are plain assert, ensure is DESERT_VERIFY_WARN or Common::EnsureOrWarn);
// do not grow it.
#pragma once

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtx/norm.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <type_traits>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Desert::Geometry
{
    inline constexpr int32_t INDEX_NONE = -1;

    enum class EAllowShrinking : uint8_t
    {
        No,
        Yes
    };

    // Templates/MemoryOps.h: element-wise equality of two runs.
    template <typename T>
    bool CompareItems( const T* A, const T* B, size_t Count )
    {
        return std::equal( A, A + Count, B );
    }

    // UE's ParallelFor (Async/ParallelFor.h), serial: UECore links no task system. Every ported caller writes
    // disjoint outputs per index or accumulates through std::atomic, so running the body in index order gives
    // the same results UE's threaded run does (float accumulation order aside).
    inline void ParallelFor( int32_t Num, const std::function<void( int32_t )>& Body,
                             bool bForceSingleThread = false )
    {
        (void)bForceSingleThread;
        for ( int32_t Index = 0; Index < Num; ++Index )
            Body( Index );
    }

} // namespace Desert::Geometry
