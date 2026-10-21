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
    // `Texture2DArray` properties (u_LayerAlbedo, u_LayerNormal, u_LayerARM, u_LayerHeight) and of the
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
} // namespace Desert::Graphic::System
