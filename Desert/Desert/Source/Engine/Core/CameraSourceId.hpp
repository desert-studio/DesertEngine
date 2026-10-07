#pragma once

#include <atomic>
#include <cstdint>

// TAA1 step 3 — WHICH CAMERA A VIEW LOOKS THROUGH. Every camera source has a stable id that is never an address:
//   * a camera that follows a scene entity's CameraComponent: the entity id (entt id incl. version), bit 32 clear;
//   * a camera driven from outside the scene's entities (the EditorCamera, a pinned preview's orbit): an id ISSUED
//     once when the camera object is constructed, from one process-wide monotonic counter, bit 32 set — so it can
//     never equal an entity id, and a camera destroyed and another constructed at the same address differ.
// With the scene generation it is the view's camera identity (Graphic::MakeViewCameraIdentity): switching a
// renderer between two editor cameras is a camera cut without anyone calling ResetTemporalHistory.
namespace Desert::Core
{
    using CameraSourceId = uint64_t;

    inline constexpr CameraSourceId kIssuedCameraSourceBit = CameraSourceId{ 1 } << 32u;

    [[nodiscard]] constexpr CameraSourceId EntityCameraSource( const uint32_t entity )
    {
        return entity;
    }

    // Never returns the same value twice in a process (the 32-bit counter would need 2^32 camera constructions to
    // wrap); never 0; always has kIssuedCameraSourceBit.
    [[nodiscard]] inline CameraSourceId IssueCameraSourceId()
    {
        static std::atomic<uint32_t> counter{ 0 };
        return kIssuedCameraSourceBit | ( counter.fetch_add( 1, std::memory_order_relaxed ) + 1u );
    }

    // The issued id a camera object carries. A copy is another camera, so it is issued its own id; assigning one
    // camera's state to another keeps the destination's id (the object, not its matrices, is the identity).
    class CameraSourceTicket
    {
    public:
        CameraSourceTicket() : m_Id( IssueCameraSourceId() )
        {
        }
        CameraSourceTicket( const CameraSourceTicket& /*other*/ ) : m_Id( IssueCameraSourceId() )
        {
        }
        CameraSourceTicket& operator=( const CameraSourceTicket& /*other*/ )
        {
            return *this;
        }

        [[nodiscard]] CameraSourceId Get() const
        {
            return m_Id;
        }

    private:
        CameraSourceId m_Id;
    };
} // namespace Desert::Core
