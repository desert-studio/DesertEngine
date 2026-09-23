#pragma once

#include <Engine/Graphic/Materials/Material.hpp>

#include <Engine/Assets/Skybox/SkyboxAsset.hpp>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/Environment/SceneEnvironment.hpp>
#include <Engine/Graphic/Environment/SkyLook.hpp>

#include <Engine/Core/Camera.hpp>

namespace Desert::Graphic
{
    struct UpdateMaterialSkyboxInfo
    {
        Core::Camera* Camera;
        // The look of the SCENE drawing this sky. It travels per draw rather than living on the material
        // because the material is one per `.hdr` and shared by every view that names it — two scenes at
        // two rotations used to rebake it alternately; now each simply binds its own value.
        SkyLook Look{};
    };

    class MaterialSkybox final : public Material
    {
    public:
        explicit MaterialSkybox( const std::shared_ptr<Assets::SkyboxAsset>& baseAsset );

        std::shared_ptr<Assets::SkyboxAsset> GetBaseMaterial() const
        {
            if ( auto material = m_BaseMaterial.lock() )
                return material;
            return nullptr;
        }

        const Environment& GetEnvironment() const { return m_Environment; }

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

        TextureCubeProperty* m_CubeMapTexture = nullptr;
    };
} // namespace Desert::Graphic
