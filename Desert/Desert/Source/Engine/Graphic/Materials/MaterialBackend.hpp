#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/Shader.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/ViewDescriptorSets.hpp>

namespace Desert::Graphic
{
    class MaterialExecutor;
    class MaterialProperty;

    class MaterialBackend
    {
    public:
        explicit MaterialBackend( const std::shared_ptr<Shader>& shader ) : m_Shader( shader )
        {
        }

        virtual ~MaterialBackend() = default;

        virtual void ApplyUniformBuffer( MaterialProperty* prop ) = 0;
        virtual void ApplyStorageBuffer( MaterialProperty* prop ) = 0;
        virtual void ApplyTexture2D( MaterialProperty* prop )     = 0;
        virtual void ApplyTextureCube( MaterialProperty* prop )   = 0;

        virtual void FlushUpdates() = 0;

        // RENDER-THREADS M1 (MaterialExecutor::PrepareForRecording). Makes the ACTIVE view's per-frame objects
        // of this material - its descriptor sets and the copies of its uniform / storage blocks - before the
        // graph records; MaterialExecutor::PrepareForRecording then writes them, and a pass's exec only binds
        // them (UE: a pass's uniform buffers and descriptor sets exist before the graph records).
        [[nodiscard]] virtual Common::BoolResultStr PrepareViewResources( const MaterialExecutor& executor ) = 0;

        // CPU-RDG-PREPARE (MaterialExecutor::PrepareForRecording skips a prepare that would write nothing).
        // The newest copy epoch (ViewCopiedBlock::GetCopyEpoch) of the executor's uniform and storage blocks.
        // A backend whose blocks have no per-view copies has none to change: 0.
        [[nodiscard]] virtual uint64_t NewestCopyEpoch( const MaterialExecutor& /*executor*/ ) const
        {
            return 0;
        }
        // Whether the ACTIVE (view, frame in flight) sets hold @p revision from a complete prepare. A backend that
        // keeps no record per copy answers false, and every prepare runs whole.
        [[nodiscard]] virtual bool IsPreparedAt( const PreparedRevision& /*revision*/ ) const
        {
            return false;
        }
        // Called after a whole prepare: records @p revision on the ACTIVE sets, unless an apply of that prepare
        // was refused (its write is still owed; the next frame's prepare runs whole and retries it).
        virtual void NotePreparedAt( const PreparedRevision& /*revision*/ )
        {
        }
        // CPU-BIND-PREPARE: called at the end of every prepare (skipped or whole), on the preparing thread. A
        // backend that resolves the ACTIVE sets through a lookup keeps them for the draws of this frame, so a draw
        // reads what the prepare left without one. Nothing kept by a backend that has no sets.
        virtual void NotePreparedSets()
        {
        }

        // ApplyPushConstants was declared here and its only implementation had an EMPTY BODY — the
        // second of two routes for the same bytes, and the one nobody took. The renderer pushes them
        // itself at draw time out of MaterialExecutor::GetPushConstantBuffer(). Г12 removed it, because
        // a pure virtual with an empty override is an instruction to write the next empty override.

    protected:
        const std::shared_ptr<Shader> m_Shader;
    };
} // namespace Desert::Graphic