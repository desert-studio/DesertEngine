#pragma once

#include <Common/Core/Math/AABB.hpp>
#include <Engine/Graphic/IndexBuffer.hpp>
#include <Engine/Graphic/VertexBuffer.hpp>
#include "Errors/MeshError.hpp"
#include "MeshTypes.hpp"

#include <Common/Core/ResultWithCodes.hpp>

#include <unordered_map>

namespace Desert
{
    class Mesh
    {
    public:
        virtual ~Mesh() = default;

        // Common interface
        [[nodiscard]] virtual MeshType GetType() const = 0;
        [[nodiscard]] bool             IsSkinned() const
        {
            return GetType() == MeshType::Skinned;
        }

        [[nodiscard]] virtual const std::vector<Submesh>& GetSubmeshes() const
        {
            return m_Submeshes;
        }
        [[nodiscard]] const std::shared_ptr<Graphic::VertexBuffer>& GetVertexBuffer() const
        {
            return m_VertexBuffer;
        }
        // The optional vertex streams (MeshVertexStreams, binding 1); null when the mesh carries neither colours
        // nor a second UV set, and then the draw binds the shared default buffer (white, UV1 0,0) at the same
        // stride.
        [[nodiscard]] const std::shared_ptr<Graphic::VertexBuffer>& GetStreamBuffer() const
        {
            return m_StreamBuffer;
        }
        [[nodiscard]] const std::shared_ptr<Graphic::IndexBuffer>& GetIndexBuffer() const
        {
            return m_IndexBuffer;
        }

        [[nodiscard]] virtual Common::BoolResultWithCodes<MeshError> Invalidate() = 0;

        // THE GPU BUFFERS ARE THE RENDERER'S COPY OF THE CPU GEOMETRY, BUILT WHERE A RENDERER EXISTS. A mesh
        // whose data was set or edited without a device (headless, server, tests, a game-thread write) only
        // marks its GPU side stale; the renderer calls this when the mesh is submitted for drawing (the
        // pattern of UE's render proxy / InitResources). One upload per change: the flag drops whether or
        // not the upload succeeds, so a failure is reported once, not once per frame.
        [[nodiscard]] Common::BoolResultWithCodes<MeshError> EnsureGpuResources()
        {
            if ( !m_GpuStale )
                return Common::MakeSuccessWithCodes<bool, MeshError>( true );
            m_GpuStale = false;
            return Invalidate();
        }
        [[nodiscard]] bool IsGpuStale() const
        {
            return m_GpuStale;
        }

    protected:
        // The CPU data changed and the GPU buffers no longer hold it (or were never made).
        void MarkGpuStale()
        {
            m_GpuStale = true;
        }
        bool m_GpuStale = false;

        // Protected data accessible by derived classes
        std::shared_ptr<Graphic::VertexBuffer>  m_VertexBuffer;
        std::shared_ptr<Graphic::VertexBuffer>  m_StreamBuffer;

        void CreateStreamBuffer( const std::vector<MeshVertexStreams>& streams )
        {
            if ( !streams.empty() )
                m_StreamBuffer = Graphic::VertexBuffer::Create( streams.data(),
                                                                streams.size() * sizeof( MeshVertexStreams ) );
        }
        [[nodiscard]] Common::BoolResultWithCodes<MeshError> InvalidateStreamBuffer() const
        {
            if ( m_StreamBuffer != nullptr )
                if ( const auto uploaded = m_StreamBuffer->RT_Invalidate(); !uploaded.IsSuccess() )
                    return Common::MakeErrorWithCodes<bool, MeshError>( { MeshError::GpuUploadFailed },
                                                                        uploaded.GetError() );
            return Common::MakeSuccessWithCodes<bool, MeshError>( true );
        }
        std::shared_ptr<Graphic::IndexBuffer>   m_IndexBuffer;
        std::vector<std::vector<TriangleCache>> m_TriangleCache;
        std::vector<Submesh>                    m_Submeshes;
    };
} // namespace Desert
