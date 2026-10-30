#pragma once

#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Assets::Serialization
{
    // A `.dworldparams`: UE's UMaterialParameterCollection — named world values (wetness, snow cover, wind) with
    // their defaults, which every material can read and gameplay can set at runtime. The scene names ONE
    // collection (Core::SceneSettings::WorldParameters); its live values are the world's
    // (Core::WorldParameterInstance), snapshotted per view per frame into the WorldParameters buffer
    // (Common/WorldParameters.glslh).
    inline constexpr const char* kWorldParametersExtension = ".dworldparams";

    inline constexpr int32_t kWorldParametersVersion =
         static_cast<int32_t>( Assets::kWorldParametersSchemaVersion );

    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> WorldParametersTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ Assets::kWorldParametersSchemaTag,
                                                static_cast<uint32_t>( kWorldParametersVersion ) } };
        return versions;
    }

    /// UE's two parameter lists of a collection (ScalarParameters / VectorParameters), as one list with a type:
    /// a material reads a parameter as a vec4 either way (a scalar is its .x), so the type is what the editor
    /// shows and what Set/Get accept, not a second storage layout.
    enum class WorldParameterType
    {
        Scalar,
        Vector,
        /// A texture the world lends to every material that reads the name (the weather's snow set): the
        /// collection names the .detex, the world resolves it, and the GPU slot carries its bindless image and
        /// sampler (`DesertWorldTexture`, Common/WorldParameters.glslh). UE's collections hold no textures, so
        /// a weather layer there is a material function every master material repeats; here the layer is the
        /// engine's (Mesh/Surface/SurfaceWeather.glslh) and its look stays an asset the preset's world names.
        Texture,
    };

    struct WorldParameterData
    {
        std::string          Name;
        WorldParameterType   Type    = WorldParameterType::Scalar;
        std::array<float, 4> Default = { 0.0f, 0.0f, 0.0f, 0.0f }; // a scalar keeps yzw at zero
        /// A Texture parameter's .detex, relative to the project's Content (`Environments/x/y.detex`); empty for
        /// a scalar or a vector.
        std::string Texture;

        [[nodiscard]] bool operator==( const WorldParameterData& ) const = default;
    };

    struct WorldParametersData
    {
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;

        std::vector<WorldParameterData> Parameters;

        [[nodiscard]] bool operator==( const WorldParametersData& ) const = default;
    };

    /// The key a parameter is looked up by on the GPU: 32-bit FNV-1a of its name. A shader names a parameter by
    /// its string (`WorldParameter( "Wetness" )`), which DShaderParser lowers to this key, so a material is
    /// compiled against no particular collection. 0 is the empty slot of the GPU table and is never a key
    /// (ValidateWorldParametersData refuses a name that hashes to it).
    [[nodiscard]] constexpr uint32_t WorldParameterKey( const std::string_view name )
    {
        uint32_t hash = 2166136261u;
        for ( const char c : name )
        {
            hash ^= static_cast<uint8_t>( c );
            hash *= 16777619u;
        }
        return hash;
    }

    /// A parameter name: a C identifier (the graph node, the shader string and Luau all spell it).
    /// Inline because DShaderParser lowers names with it, and two tools compile that parser without the engine.
    [[nodiscard]] constexpr bool IsWorldParameterName( const std::string_view name )
    {
        const auto alpha = []( const char c )
        { return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || c == '_'; };
        if ( name.empty() || !alpha( name.front() ) )
            return false;
        for ( const char c : name )
            if ( !alpha( c ) && !( c >= '0' && c <= '9' ) )
                return false;
        return true;
    }

    /// Every name an identifier, unique, with a key no other name of the collection shares and that is not 0;
    /// every default finite; a scalar's yzw zero.
    Common::BoolResultStr ValidateWorldParametersData( const WorldParametersData& data );

    Common::ResultStr<WorldParametersData> ParseWorldParameters( const std::string& text );

    std::string WriteWorldParameters( const WorldParametersData& data );

    Common::BoolResultStr SaveWorldParametersFile( const std::filesystem::path& path,
                                                   const WorldParametersData&   data );
} // namespace Desert::Assets::Serialization
