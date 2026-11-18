#pragma once

#include <Engine/Graphic/Materials/Material.hpp>

#include <Engine/Assets/Skybox/SkyboxAsset.hpp>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/Environment/SceneEnvironment.hpp>
#include <Engine/Graphic/Environment/SkyLook.hpp>

#include <Engine/Core/Camera.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>

#include <cstdint>
#include <optional>

namespace Desert::Graphic
{
    class FrameTextures;
    namespace RDG
    {
        class Builder;
    }
    namespace System
    {
        class SkyCapture;
    }

    struct UpdateMaterialSkyboxInfo
    {
        // The view this sky is drawn for (SceneRenderer::GetViewFrame); its camera block is MakeCameraUB of it.
        const ViewFrame* View = nullptr;
        // The look of the SCENE drawing this sky. It travels per draw rather than living on the material
        // because the material is one per `.hdr` and shared by every view that names it — two scenes at
        // two rotations used to rebake it alternately; now each simply binds its own value.
        SkyLook Look{};
    };

    class MaterialSkybox final : public Material
    {
    public:
        explicit MaterialSkybox( const std::shared_ptr<Assets::SkyboxAsset>& baseAsset );
        ~MaterialSkybox() override;
        MaterialSkybox( const MaterialSkybox& )            = delete;
        MaterialSkybox& operator=( const MaterialSkybox& ) = delete;

        std::shared_ptr<Assets::SkyboxAsset> GetBaseMaterial() const
        {
            if ( auto material = m_BaseMaterial.lock() )
                return material;
            return nullptr;
        }

        const Environment& GetEnvironment() const { return m_Environment; }

        /// GRAPH BUILD, by every view that draws this skybox, at the head of its graph (SkyboxRenderer::
        /// AddSkyCapturePasses): a cache miss's conversion (EnvironmentConvolution) is recorded WHOLE by the first
        /// view's graph, ahead of every pass that samples the cubes, and the environment is set at once -- that
        /// view and every view after it read it. A later frame's call hands the converted cubes to the cache
        /// writer: the graph that wrote them has been submitted by then. A hit, or a finished conversion, adds
        /// nothing.
        void AddConvolutionPasses( RDG::Builder& graph, FrameTextures& textures );

        bool IsUsingBaseMaterial() const { return m_BaseMaterial.lock() != nullptr; }

        bool IsReady() const
        {
            if ( const auto& base = m_BaseMaterial.lock() )
                return base->IsReadyForUse();
            return false;
        }

        void BindInputs( const UpdateMaterialSkyboxInfo& data );

    private:
        std::weak_ptr<Assets::SkyboxAsset> m_BaseMaterial;
        std::shared_ptr<MaterialExecutor>  m_Material;
        Environment                        m_Environment;
        // A cache miss's conversion, until its cubes are handed to the cache writer; the capture that records it,
        // made for the miss only; and the frame whose graph recorded it (the writer begins in a later one).
        std::optional<EnvironmentConvolution> m_Convolution;
        std::unique_ptr<System::SkyCapture>   m_Capture;
        uint64_t                              m_ConvertedFrame = ~uint64_t{ 0 };

        TextureCubeProperty* m_CubeMapTexture = nullptr;
    };
} // namespace Desert::Graphic
