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

        /// The Renderer records a cache miss's conversion and hands the cubes to the cache writer
        /// (Systems/Scene/Skybox/SkyboxConvolution.hpp): this material only holds the conversion's state.
        friend void AddSkyboxConvolutionPasses( MaterialSkybox& material, RDG::Builder& graph, FrameTextures& textures );

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
        // Made by the Renderer (SkyboxConvolution.cpp) with its deleter, so this Engine-side material never
        // names the capture's code.
        std::shared_ptr<System::SkyCapture>   m_Capture;
        uint64_t                              m_ConvertedFrame = ~uint64_t{ 0 };

        TextureCubeProperty* m_CubeMapTexture = nullptr;
    };
} // namespace Desert::Graphic
