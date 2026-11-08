#pragma once

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/Materials/MaterialBackend.hpp>
#include <Engine/Graphic/Materials/Properties/PropertyVersion.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

namespace Desert::Graphic
{
    class MaterialProperty
    {
    public:
        virtual ~MaterialProperty() = default;

        // THERE IS NO `Clone()`, AND THERE MUST NOT BE ONE THAT LOOKS LIKE THIS. It used to sit here as a
        // second pure virtual, implemented by all four property kinds, called from nowhere in the tree
        // (М9). Each body was a commented-out sketch over `return nullptr`, and every one of those
        // sketches was an ALIAS rather than a copy: the texture properties passed the SAME
        // `UniformImage2D`/`UniformImageCube` to the new object, so writing the "clone" would have written
        // the original's descriptor, and the buffer properties passed the same `UniformBuffer`, so the two
        // would have shared one GPU allocation and each claimed the other's FillKind route
        // (ShaderResources/BufferFillKind.hpp records what a mis-claimed route did to a frame). Two of the
        // four did not even compile: Texture2DProperty's called a `SetTexture` this class does not have,
        // and StorageBufferProperty's constructed a `UniformBufferProperty`.
        //
        // The operation a caller actually wants is `Material::CreateInstance()` — a MaterialInstance holds
        // its own overrides over a shared parent — and an editor working copy is
        // `Assets::SurfaceMaterialAsset::CreateWorkingCopy`, which duplicates the ASSET data and lets the
        // factory build fresh properties from it. Both exist and both are used; a per-property copy is on
        // neither path.
        virtual void Apply( MaterialBackend* backend ) = 0;

        // The version of this property's latest write (PropertyVersion.hpp). Each consumer copy — a view's
        // descriptor sets, through ShaderResources::DescriptorCopyRecord — remembers the version it applied
        // and compares, so a view that first records long after a one-off write is still behind it and
        // takes it. There is no "clean": nothing is consumed by one view on another's behalf.
        [[nodiscard]] uint64_t GetVersion() const noexcept
        {
            return m_Version;
        }

        // CPU-BIND-PREPARE (MaterialExecutor::CheckRecordable in O(1)): the owning executor's write counter,
        // bumped by every write of this property or one of its fields. Shared, so a property a caller keeps past
        // its executor never writes into freed memory. False when the property counts into another executor's
        // counter already: that executor's writes would go uncounted here, so the caller checks in full.
        [[nodiscard]] bool AttachWriteSink( const std::shared_ptr<std::atomic<uint64_t>>& sink ) noexcept
        {
            if ( m_WriteSink == sink )
                return true;
            if ( m_WriteSink != nullptr )
                return false;
            m_WriteSink = sink;
            return true;
        }

    protected:
        // Every write calls this: a fresh version puts every copy that applied an older one behind.
        void NoteWritten() noexcept
        {
            m_Version = PropertyVersion::Next();
            NoteFieldWritten();
        }
        // A field of this property was written (UniformBufferProperty::WriteField): the version is the field's.
        void NoteFieldWritten() noexcept
        {
            if ( m_WriteSink != nullptr )
                m_WriteSink->fetch_add( 1, std::memory_order_relaxed );
        }

    private:
        // Born WRITTEN: a new property has been applied nowhere, so every set owes it one write.
        uint64_t                               m_Version = PropertyVersion::Next();
        std::shared_ptr<std::atomic<uint64_t>> m_WriteSink;
    };
} // namespace Desert::Graphic
