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

    // FOUR BOXES AS SIX COLUMNS ON THE STACK: one batch of a walk whose boxes are computed on the fly (terrain
    // sections and patches, the water quadtree's four children, a queue's optional bounds) rather than kept as
    // AABBLanes between frames. Same layout as one four-lane window of AABBLanes, so Core::Frustum::Intersects4
    // reads either; a lane never Set keeps the zero box and its verdict is the caller's to mask.
    struct AABBQuad final
    {
        void Set( const std::size_t lane, const AABB& box ) noexcept
        {
            MinX[lane] = box.Min.x;
            MinY[lane] = box.Min.y;
            MinZ[lane] = box.Min.z;
            MaxX[lane] = box.Max.x;
            MaxY[lane] = box.Max.y;
            MaxZ[lane] = box.Max.z;
        }

        float MinX[4] = {};
        float MinY[4] = {};
        float MinZ[4] = {};
        float MaxX[4] = {};
        float MaxY[4] = {};
        float MaxZ[4] = {};
    };

    // FOUR SPHERES AS FOUR COLUMNS (UE's light grid reads SoA positions and radii): centre x, y, z and radius per
    // lane, for Core::Frustum::IntersectsSphere4. A lane never Set is the zero sphere; the caller masks it.
    struct SphereQuad final
    {
        void Set( const std::size_t lane, const float x, const float y, const float z,
                  const float radius ) noexcept
        {
            X[lane] = x;
            Y[lane] = y;
            Z[lane] = z;
            R[lane] = radius;
        }

        float X[4] = {};
        float Y[4] = {};
        float Z[4] = {};
        float R[4] = {};
    };
} // namespace Common::Math
