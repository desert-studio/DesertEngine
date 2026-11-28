#pragma once

#include <cstdint>
#include <string_view>

// THE RADIANCE CACHE'S MARK REQUESTS (NG-RT-REFRACTION3G) - how a reader that is not a G-buffer pixel asks the
// world radiance cache (WorldRadianceCache.hpp) for the cells it interpolates. UE marks from every reader of the
// cache (LumenRadianceCacheMarkCommon.ush: the screen probes, the translucency volume, rays); here the G-buffer
// pixels are marked by RadianceCacheMark itself, and every other GPU reader writes a REQUEST instead.
//
// The table is one persistent storage buffer per cache (so per view; nothing is shared between views or worlds),
// kMarkRequestSlots slots of one vec4 each, direct-mapped: a reader hashes its point's 50 cm cell to a slot and
// stores one vec4 there (Common/RadianceCacheMarkRequests.glslh RadianceCacheRequestMark; two readers of one slot:
// one whole request wins). The NEXT frame's Mark reads every occupied slot, stamps the eight cells around the
// request's read point in the covering clipmap and in the next coarser one, and zeroes the slot. A request lives
// one frame: a reader that stops asking stops marking.
//
// Owner: WorldRadianceCache creates, imports and publishes the buffer (RadianceCacheFrame::MarkRequests). A
// reader's pass declares a WRITE of it to the RDG under kMarkRequestsBinding; only Mark reads and clears it.
namespace Desert::Graphic::GlobalIllumination
{
    // Slots of the table. The shader side reads the count from the buffer's length, never from a constant of its
    // own. 4096 x 16 B = 64 KB a view.
    inline constexpr uint32_t kMarkRequestSlots = 4096u;

    // One slot: a vec4 (std430) - xyz the surface point (world, cm), w the request word as float bits
    // (RadianceCacheMarkRequests.glslh RC_MARK_REQUEST_*); a zero word is an empty slot.
    inline constexpr uint32_t kMarkRequestSlotBytes = 16u;
    inline constexpr uint64_t kMarkRequestBytes     = uint64_t{ kMarkRequestSlots } * kMarkRequestSlotBytes;
    static_assert( ( kMarkRequestSlots & ( kMarkRequestSlots - 1u ) ) == 0u, "the slot hash masks: a power of two" );

    // The block name both sides bind the table by (the writer's and Mark's shader parameter).
    inline constexpr std::string_view kMarkRequestsBinding = "RadianceCacheMarkRequestsUB";

    // The push constant of RadianceCacheMark that carries how many of its group rows are request rows (after the
    // pixel rows and the free rows): its name on both sides.
    inline constexpr std::string_view kMarkRequestRowsPush = "MarkRequestRows";
} // namespace Desert::Graphic::GlobalIllumination
