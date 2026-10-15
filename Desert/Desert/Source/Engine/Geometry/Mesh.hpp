#pragma once

#include <Common/Core/Math/AABB.hpp>
#include <Engine/Graphic/IndexBuffer.hpp>
#include <Engine/Graphic/VertexBuffer.hpp>
#include "Errors/MeshError.hpp"
#include "MeshTypes.hpp"
#include <Engine/Geometry/LODSelection.hpp>

#include <Common/Core/ResultWithCodes.hpp>
#include <Engine/Assets/MeshDistanceField.hpp>

#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

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

        // THE SCREEN SIZE AT WHICH EACH LEVEL OF THIS MESH BEGINS (UE FStaticMeshRenderData::ScreenSize): one
        // entry per level the mesh draws, resolved from its asset's authored values (MeshService, at build) or
        // the automatic rule. Every LOD pick of this mesh - renderer, ISM instances, shadows, Details - walks it.
        [[nodiscard]] const std::vector<float>& GetLODScreenSizes() const
        {
            return m_LODScreenSizes;
        }
        [[nodiscard]] const std::optional<std::vector<float>>& GetAuthoredLODScreenSizes() const
        {
            return m_AuthoredLODScreenSizes;
        }
        // The asset's per-LOD values (nullopt = computed); re-resolved against the levels this mesh has.
        void SetAuthoredLODScreenSizes( std::optional<std::vector<float>> authored )
        {
            m_AuthoredLODScreenSizes = std::move( authored );
            ResolveLODScreenSizes();
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
        // THE BUFFER WHOSE POSITIONS THESE VERTICES HAD LAST FRAME (binding 2, MeshVertexLayout.hpp; UE
        // FGPUSkinCache PreviousPositionBuffer): the mesh's own vertices unless something rewrites them per frame
        // - a morphed twin points it at its previous blend output for the frame it blended (MeshRendererMorph).
        // Same vertex format and count as GetVertexBuffer; only the position is read from it.
        [[nodiscard]] const std::shared_ptr<Graphic::VertexBuffer>& GetPreviousPositionBuffer() const
        {
            return m_PreviousPositions != nullptr ? m_PreviousPositions : m_VertexBuffer;
        }
        // null = the vertices did not move under the mesh since last frame (its own vertices are the previous).
        void SetPreviousPositionBuffer( std::shared_ptr<Graphic::VertexBuffer> previous )
        {
            m_PreviousPositions = std::move( previous );
        }
        [[nodiscard]] const std::shared_ptr<Graphic::IndexBuffer>& GetIndexBuffer() const
        {
            return m_IndexBuffer;
        }

        [[nodiscard]] virtual Common::BoolResultWithCodes<MeshError> Invalidate() = 0;

        // The mesh's signed distance field (Assets::MeshDistanceFieldData) once it has landed in the slot shared
        // with the asset it was loaded with (the field is derived asynchronously,
        // Assets::EnqueueMeshDistanceField); null before that and for a mesh that has none (skinned, procedural).
        // What the GPU scene's distance-field atlas uploads (Graphic/DistanceField), keyed by the field's pointer.
        [[nodiscard]] std::shared_ptr<const Assets::MeshDistanceFieldData> GetDistanceField() const
        {
            return m_DistanceField ? m_DistanceField->Get() : nullptr;
        }
        void SetDistanceField( std::shared_ptr<const Assets::MeshDistanceFieldSlot> slot )
        {
            m_DistanceField = std::move( slot );
        }

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

        // Called by every subclass that (re)builds its LOD chain: the resolved sizes follow the level count.
        void ResolveLODScreenSizes()
        {
            m_LODScreenSizes = Geometry::ResolveLODScreenSizes( Geometry::MaxAvailableLOD( m_Submeshes ) + 1,
                                                                m_AuthoredLODScreenSizes );
        }
        std::optional<std::vector<float>> m_AuthoredLODScreenSizes;
        std::vector<float>                m_LODScreenSizes;

        // Protected data accessible by derived classes
        std::shared_ptr<Graphic::VertexBuffer>  m_VertexBuffer;
        std::shared_ptr<Graphic::VertexBuffer>  m_StreamBuffer;
        std::shared_ptr<Graphic::VertexBuffer>  m_PreviousPositions; // null = m_VertexBuffer

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
        std::shared_ptr<const Assets::MeshDistanceFieldSlot> m_DistanceField;
    };
} // namespace Desert
