#pragma once

#include <Engine/Graphic/Shader.hpp>
#include <Engine/Graphic/Pipeline.hpp>

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

        // ApplyPushConstants was declared here and its only implementation had an EMPTY BODY — the
        // second of two routes for the same bytes, and the one nobody took. The renderer pushes them
        // itself at draw time out of MaterialExecutor::GetPushConstantBuffer(). Г12 removed it, because
        // a pure virtual with an empty override is an instruction to write the next empty override.

    protected:
        const std::shared_ptr<Shader> m_Shader;
    };
} // namespace Desert::Graphic