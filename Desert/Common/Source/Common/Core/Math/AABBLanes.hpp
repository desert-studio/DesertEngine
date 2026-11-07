#pragma once

#include <Common/Core/Math/AABB.hpp>

#include <cstddef>
#include <vector>

namespace Common::Math
{
    // BOXES AS SIX DENSE COLUMNS (UE FScene::PrimitiveBounds' SoA role; the batch cull's input): Min x, y, z and
    // Max x, y, z, one float array each, in the order the boxes were pushed. A loop that culls thousands of boxes
    // per view reads 24 bytes per box from six streams, four boxes per register (Core::Frustum::Intersects4,
    // Graphic::VisibleInViewBits), instead of the fat row each box was gathered into.
    //
    // PADDED: every column holds kPad floats past Size(), so a four-lane load at any index below Size() stays in
    // the allocation; the padding's values are whatever an earlier, longer fill left (finite floats) and never
    // vote - a batch masks every lane at or past its count. Clear keeps the storage, so a column refilled each
    // gather allocates only when it grows past its largest size.
    class AABBLanes final
    {
    public:
        static constexpr std::size_t kPad = 3;

        void Clear() noexcept
        {
            m_Size = 0;
        }

        void PushBack( const AABB& box )
        {
            const std::size_t at = m_Size;
            if ( MinX.size() < at + 1u + kPad )
            {
                const std::size_t columnSize = at + 1u + kPad;
                MinX.resize( columnSize, 0.0f );
                MinY.resize( columnSize, 0.0f );
                MinZ.resize( columnSize, 0.0f );
                MaxX.resize( columnSize, 0.0f );
                MaxY.resize( columnSize, 0.0f );
                MaxZ.resize( columnSize, 0.0f );
            }
            MinX[at] = box.Min.x;
            MinY[at] = box.Min.y;
            MinZ[at] = box.Min.z;
            MaxX[at] = box.Max.x;
            MaxY[at] = box.Max.y;
            MaxZ[at] = box.Max.z;
            m_Size   = at + 1u;
        }

        [[nodiscard]] std::size_t Size() const noexcept
        {
            return m_Size;
        }

        std::vector<float> MinX;
        std::vector<float> MinY;
        std::vector<float> MinZ;
        std::vector<float> MaxX;
        std::vector<float> MaxY;
        std::vector<float> MaxZ;

    private:
        std::size_t m_Size = 0;
    };
} // namespace Common::Math
