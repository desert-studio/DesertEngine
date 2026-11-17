#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

namespace Desert::Graphic
{
    // FPS-SET-MEMO4, A PER-FRAME GRAPH BUFFER WITH A STABLE PHYSICAL IDENTITY PER FRAME-IN-FLIGHT SLOT (UE: a
    // buffer a cached draw command's bindings reference is a pooled FRDGPooledBuffer kept across graphs; transient
    // aliasing is for pass-local scratch). A graph transient is placed per (frame slot, heap offset, size), so
    // every plan shift or size change gave the per-frame light / tile buffers a new VkBuffer and the bound-sets
    // memo (keyed by what the sets hold) missed. This keeps, per frame-in-flight slot, ONE device-local buffer,
    // grow-only (power of two of the need), imported into the frame's graph (Renderer::ImportBuffer +
    // RegisterExternal) and declared by the passes exactly as the transient was. One slot per frame in flight:
    // frame N's async-compute writer and frame N+1's never share a buffer; a slot is reused only after the
    // frames-in-flight fence. Contents are undefined until the graph writes them (QueueBufferUpload or a pass), as
    // a transient's were. ONE instance per call site and view: two writers in one graph must not share it.
    class FrameKeptBuffer
    {
    public:
        // This frame's slot as a buffer of @p graph named @p name, at least @p bytes (min 4). Refused over a
        // buffer's 32-bit size or when the buffer cannot be made or imported.
        [[nodiscard]] Common::ResultStr<RDG::BufferRef> Register( RDG::Builder& graph, const uint64_t bytes,
                                                                  const std::string_view name ) noexcept
        {
            constexpr uint64_t kMaxBytes = std::numeric_limits<uint32_t>::max();
            const uint32_t     frame     = Engine::FrameManager::GetInstance().GetCurrentFrameIndex();
            while ( m_Slots.size() <= frame )
                m_Slots.push_back( std::make_unique<Slot>() ); // unique_ptr: a graph keeps &Slot::Import
            Slot&          slot = *m_Slots[frame];
            const uint64_t need = std::max<uint64_t>( bytes, 4u );
            if ( slot.Buffer == nullptr || need > slot.Bytes )
            {
                if ( need > kMaxBytes )
                    return Common::MakeError<RDG::BufferRef>( std::format(
                         "FrameKeptBuffer: '{}' needs {} bytes, over a buffer's 32-bit size", name, need ) );
                const uint64_t grown = std::min( std::bit_ceil( std::max<uint64_t>( need, 256u ) ), kMaxBytes );
                slot.Buffer =
                     ShaderResources::StorageBuffer::CreateDeviceLocal( name, static_cast<uint32_t>( grown ), 0u );
                slot.Import = RDG::ExternalBuffer{};
                if ( slot.Buffer == nullptr )
                {
                    slot.Bytes = 0u;
                    return Common::MakeError<RDG::BufferRef>(
                         std::format( "FrameKeptBuffer: '{}' ({} bytes) was not made", name, grown ) );
                }
                slot.Bytes = grown;
            }
            if ( const Common::BoolResultStr imported = Renderer::ImportBuffer( slot.Buffer, slot.Import );
                 !imported )
                return Common::MakeError<RDG::BufferRef>(
                     std::format( "FrameKeptBuffer: '{}': {}", name, imported.GetError() ) );
            return Common::MakeSuccess( graph.RegisterExternal( slot.Import, name ) );
        }

    private:
        struct Slot
        {
            std::shared_ptr<ShaderResources::StorageBuffer> Buffer;
            RDG::ExternalBuffer                             Import;
            uint64_t                                        Bytes = 0;
        };
        std::vector<std::unique_ptr<Slot>> m_Slots;
    };
} // namespace Desert::Graphic
