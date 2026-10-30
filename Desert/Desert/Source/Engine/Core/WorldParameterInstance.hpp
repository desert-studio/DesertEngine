#pragma once

#include <Engine/Assets/Serialization/WorldParameters.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Core
{
    /// One live parameter of the world: the collection's row and the value the world holds now.
    struct WorldParameterValue
    {
        std::string                               Name;
        uint32_t                                  Key  = 0; // Serialization::WorldParameterKey( Name )
        Assets::Serialization::WorldParameterType Type = Assets::Serialization::WorldParameterType::Scalar;
        glm::vec4                                 Value{ 0.0f };
        /// A Texture parameter's texture, resolved from the collection's path when the world adopts it
        /// (Scene::SyncWorldParameters); each view writes its bindless slot into Value (SceneRenderer).
        Common::AssetHandle Texture{};
    };

    /// What a SurfaceLayer row's graph contributes to the instance: its name (the layer id is
    /// ShaderGraph::SurfaceLayerId of it) and the textures it samples. An empty Name = the graph did not resolve.
    struct SurfaceLayerSource
    {
        std::string              Name;
        std::vector<std::string> Textures;
    };

    /**
     * @brief The world's instance of its collection — UE's UMaterialParameterCollectionInstance: the scene's
     * collection (SceneSettings::WorldParameters) adopted with its defaults, then set by gameplay at runtime
     * (Luau `WorldParameters.set*`). Every view snapshots it once per frame into its WorldParameters buffer
     * (Graphic::BuildWorldParameterTable), so a set reaches every material that reads the name on the next frame.
     *
     * Values live here, not in the asset: the asset is the defaults, shared by every world that names it; a
     * Play world is its own Scene and so its own instance, so what a script sets in Play never reaches the file
     * or the edit world.
     */
    class WorldParameterInstance
    {
    public:
        /// Takes the collection @p data stands for (its defaults; null = the scene names none). Re-adopting the
        /// same handle with equal data keeps the live values; a different handle or changed data (the asset was
        /// edited and saved) resets every value to the new defaults.
        void Adopt( const Common::AssetHandle& source, const Assets::Serialization::WorldParametersData* data,
                    const std::function<Common::AssetHandle( const std::string& )>& resolveTexture = {},
                    const std::function<SurfaceLayerSource( const std::string& )>&  resolveLayer   = {} );

        [[nodiscard]] const Common::AssetHandle& Source() const
        {
            return m_Source;
        }

        [[nodiscard]] std::span<const WorldParameterValue> Values() const
        {
            return m_Values;
        }

        /// Refused by name when the collection has no such parameter or it is a vector.
        Common::BoolResultStr SetScalar( std::string_view name, float value );
        /// Refused by name when the collection has no such parameter or it is a scalar.
        Common::BoolResultStr SetVector( std::string_view name, const glm::vec4& value );

        [[nodiscard]] Common::ResultStr<float>     GetScalar( std::string_view name ) const;
        [[nodiscard]] Common::ResultStr<glm::vec4> GetVector( std::string_view name ) const;

    private:
        /// The row named @p name of type @p type, or null with @p why naming what is wrong.
        [[nodiscard]] const WorldParameterValue*
        Find( std::string_view name, Assets::Serialization::WorldParameterType type, std::string& why ) const;

        Common::AssetHandle                        m_Source;
        Assets::Serialization::WorldParametersData m_Adopted; // the defaults the values were reset from
        std::vector<WorldParameterValue>           m_Values;
    };
} // namespace Desert::Core
