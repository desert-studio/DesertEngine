#pragma once

#include <Engine/Core/Formats/ShaderProgramMeta.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <Engine/World/Landscape/LandscapeData.hpp>

#include <glm/glm.hpp>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace Desert::Graphic
{
    class Image2D;
}

namespace Desert::Graphic::System
{
    // The kinds of map a landscape material input carries, in the order of the Terrain programs' four
    // `Texture2DArray` properties (LayerAlbedoTexture, LayerNormalTexture, LayerArmTexture, LayerHeightTexture) and of the
    // x/y/z/w of TerrainInstance::InputSlices.
    inline constexpr uint32_t kTerrainLayerMapKinds = 4;

    /**
     * @brief THE LANDSCAPE MATERIAL'S INPUTS, RESOLVED (UE: a LandscapeLayerBlend node's layer list, compiled).
     *
     * Input k is every `Element(<group>, k)` property of the material's template: its LandscapeLayerInfo (the
     * LayerName it is matched to tile channels by) and its four maps. The maps of one kind are copied into one
     * 2D texture array, slice by slice in input order, skipping inputs that have no map of that kind, so the
     * number of inputs is bounded by the weightmap's layer budget (kLandscapeMaxWeightLayers) and not by the
     * fragment stage's sampler count. Every slice of one array must have one size, format and mip count: a
     * material that mixes them is refused with the layer and the field named (Image2D::CopyLayersFrom), and
     * that map kind then draws its flat default for every input until the textures are re-cooked.
     *
     * Built from a material's overrides and kept between frames: it is rebuilt only when the images its
     * arrays were copied from change (a texture finishing its load, an edited `.demat`).
     */
    struct TerrainLayerInputs
    {
        /// Input k's LayerName (empty: the input names no layer, or its layer info is not loaded yet).
        std::array<std::string, World::Landscape::kLandscapeMaxWeightLayers> Names{};
        /// Input k's slice in each array (x albedo, y normal, z ARM, w height; -1 = none).
        std::array<glm::vec4, World::Landscape::kLandscapeMaxWeightLayers> Slices{};
        /// One array per map kind; a kind no input fills is a one-layer white array nothing samples (the
        /// sampler2DArray still needs an array view bound).
        std::array<std::shared_ptr<Image2D>, kTerrainLayerMapKinds> Arrays{};
        /// What each array was copied from, slice order: the rebuild key.
        std::array<std::vector<const Image2D*>, kTerrainLayerMapKinds> Sources{};
        /// Each source's Image2D::GetResourceGeneration when its array was copied: a streamed mip change swaps a
        /// source's levels inside the same Image2D (TEX-STREAMING), so the pointers alone miss it.
        std::array<std::vector<uint64_t>, kTerrainLayerMapKinds> Generations{};
        /// Whether each array holds its sources (false: none, or the copy was refused - the kind draws flat).
        std::array<bool, kTerrainLayerMapKinds> Filled{};
        /// The names of the four array properties, from the template.
        std::array<std::string, kTerrainLayerMapKinds> ArrayNames{};

        /// The input whose LayerName is @p name, or -1 (also for an empty name).
        [[nodiscard]] int32_t InputOf( const std::string& name ) const
        {
            if ( name.empty() )
                return -1;
            for ( size_t k = 0; k < Names.size(); ++k )
                if ( Names[k] == name )
                    return static_cast<int32_t>( k );
            return -1;
        }
    };

    /// The one-layer array bound to a kind no input fills: a sampler2DArray must see an array view, and this one
    /// is never sampled (every input's slice of the kind is -1). Owned by the renderer, so it dies with the
    /// device.
    std::shared_ptr<Image2D> CreateTerrainEmptyLayerArray();

    /// (Re)resolves @p inputs from @p schema's Element properties and @p overrides' values for them: names from
    /// the layer infos, arrays rebuilt only where a source image changed (a kind with no array gets
    /// @p emptyArray). @p materialTag names the material in an error. A layer info or texture still loading is
    /// simply absent this frame and picked up when it lands.
    void UpdateTerrainLayerInputs( const Core::Formats::ShaderProgramMeta& schema,
                                   const MaterialOverrides& overrides, const std::string& materialTag,
                                   const std::shared_ptr<Image2D>& emptyArray, TerrainLayerInputs& inputs );

    /**
     * @brief THE LANDSCAPE MATERIAL'S VALUES the ground reads (Terrain.shader / TerrainGBuffer.shader Properties).
     *
     * GLSL twin: Common/LandscapeSurface.glslh `LandscapeMaterial`, the struct the one statement of the ground
     * (EvaluateLandscapeSurface) takes - the raster builds it from its Material, the landscape's ray tracing hit
     * group reads it from its row. vec4 slots only, so one layout in std140, std430 and here.
     */
    struct LandscapeMaterialValues
    {
        /// FarTilingDistance (cm), FarTilingScale, MacroVariationSize (cm), MacroVariationStrength.
        glm::vec4 Tiling{ 0.0f };
        /// DetailNormalScale (times the input's tile size), DetailNormalStrength, MacroHueSize (cm), MacroHueStrength
        /// (radians).
        glm::vec4 Detail{ 0.0f };
        /// Input k's LayerKTint.
        std::array<glm::vec4, World::Landscape::kLandscapeMaxWeightLayers> LayerTint{};
        /// Input k's (LayerKTileSize cm, LayerKBlend, LayerKNormalStrength, 0).
        std::array<glm::vec4, World::Landscape::kLandscapeMaxWeightLayers> LayerParams{};
    };
    static_assert( World::Landscape::kLandscapeMaxWeightLayers == 8u,
                   "Common/LandscapeSurface.glslh LandscapeMaterial declares eight inputs" );
    static_assert( sizeof( LandscapeMaterialValues ) == 18u * sizeof( glm::vec4 ),
                   "LandscapeMaterialValues is GLSL LandscapeMaterial's 18 vec4 slots" );

    /**
     * @brief A tile's landscape material as the raster drew it this frame, outside the draw
     * (TerrainRenderer::GetFrameMaterials): what the landscape's ray tracing hit group needs to evaluate the SAME
     * ground (UE: the landscape material's ray tracing hit shaders read the material's own parameters and
     * textures).
     */
    struct TerrainFrameMaterial
    {
        /// The material's Tint (Terrain.shader / TerrainGBuffer.shader: the ground's albedo times it).
        glm::vec4               Tint{ 1.0f };
        LandscapeMaterialValues Values;
        /// Input k's maps in the bindless table (x albedo, y normal, z ARM, w height; 0 = none): the images
        /// the layer arrays copied slice by slice (TerrainLayerInputs::Sources), so a hit reads the raster's
        /// pixels. Zero exactly where the input's slice of that kind is -1 (TerrainInstance::InputSlices).
        std::array<glm::uvec4, World::Landscape::kLandscapeMaxWeightLayers> LayerMaps{};
        // No view data: the camera and the grass lens are the frame's ray tracing view block
        // (RayTracing::RayTracingViewBlock), one per frame, not one per tile.
    };
} // namespace Desert::Graphic::System
