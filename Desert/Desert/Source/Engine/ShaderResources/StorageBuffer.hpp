#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Engine/Graphic/Shader.hpp>
#include "BaseBuffer.hpp"

namespace Desert::ShaderResources
{
    class StorageBuffer : public BaseBuffer
    {
    public:
        // The ledger row — see Engine/Graphic/ResourceLedger.hpp and the note on UniformBuffer. A
        // non-persistent storage buffer is one VkBuffer per (view x frame in flight) that used it; a persistent
        // one is a single device buffer, and this row does not distinguish them, which is why the ledger counts
        // OBJECTS and reports bytes only where somebody knew them.
        StorageBuffer() : m_Accounting( Graphic::ResourceOwnership::Take( Graphic::ResourceKind::StorageBuffer ) )
        {
        }

        virtual ~StorageBuffer() = default;

        void ClaimOwnership( const Graphic::ResourceOwner owner,
                             const Common::AssetHandle    asset = Common::AssetHandle{} )
        {
            m_Accounting.Claim( owner, asset );
        }

        // A storage buffer has NO fields, and that is what makes it a single-route buffer: there are no
        // per-field shadow copies to write, so StorageBufferProperty::SetRawData is the only way to fill
        // one and nothing can contradict it (ShaderResources::FillKind explains the uniform-buffer case,
        // where two routes do exist).
        //
        // Returned by reference from a static, not `return {}`: the previous body bound the reference to
        // a temporary that died at the closing brace, so any caller at all would have read freed memory.
        // Nothing called it, which is why it survived -- but BaseBuffer::GetFields() is virtual and
        // public, so "nothing calls it" is a property of today's callers, not of the type.
        virtual const std::vector<ShaderLayout::ShaderFieldLayout>& GetFields() const override
        {
            static const std::vector<ShaderLayout::ShaderFieldLayout> s_None;
            return s_None;
        }

        // Allocate a storage buffer of an explicit size. Public so systems (e.g. the auto-exposure
        // histogram) can create one directly — the reflection-driven path in ShaderResourcesManager
        // hardcodes a 36-byte size and is only suitable for shader-declared buffers.
        //
        // persistent=false (default): PER-FRAME-in-flight — one buffer copy per (view x frame), so the CPU can
        // write next frame's data while the GPU reads the current one (right for per-frame CPU uploads like the
        // histogram / instance transforms). persistent=true: ONE device buffer shared by every frame, so GPU
        // state written by a compute pass SURVIVES across frames (right for GPU particle simulation). A
        // persistent buffer is still host-mappable for CPU init/reset.
        static std::shared_ptr<StorageBuffer> Create( const std::string_view debugName, uint32_t size,
                                                      uint32_t binding, bool persistent = false );

        // READS WHAT THE GPU LEFT in this frame in flight's copy for the active view (GPU-count feedback: a
        // compute pass writes a count, a later frame sizes buffers from it). Per-frame buffers only: the copy
        // for the current frame index was last used FramesInFlight frames ago and the frames-in-flight fence has
        // finished that use, so its bytes are that frame's GPU writes - call it before this frame writes or binds
        // the copy. A persistent buffer is refused: its one device buffer may be written by a frame still in
        // flight, and a host read racing a device write has no defined value. GetData is the CPU image, not this.
        NO_DISCARD virtual Common::BoolResultStr ReadBack( void* destination, uint32_t size,
                                                           uint32_t offset = 0 ) = 0;

    private:
        Graphic::ResourceOwnership m_Accounting;
    };

} // namespace Desert::ShaderResources