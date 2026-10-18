#include "TerrainLayerInputs.hpp"

#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Common/Core/Logger.hpp>

#include <format>
#include <set>

namespace Desert::Graphic::System
{
    namespace
    {
        // Said once per distinct message: the resolve runs every frame, and an authoring error (two map sizes
        // in one array) stays true until somebody re-cooks a texture.
        void LogOnce( const std::string& message )
        {
            static std::set<std::string> said;
            if ( said.insert( message ).second )
                LOG_ERROR( "{}", message );
        }

        uint64_t HandleOf( const MaterialOverrides& overrides, const std::string& name )
        {
            // The overrides are the material chain base first, so the LAST statement of a slot is the one in
            // force (MaterialService::ResolveOverrides appends the instances after their base).
            uint64_t handle = 0;
            for ( const auto& [slot, value] : overrides.Textures )
                if ( slot == name )
                    handle = value;
            return handle;
        }

        // The image a texture handle names, or null while it is loading / when it is not registered.
        const Image2D* ImageOf( uint64_t handle )
        {
            if ( handle == 0 )
                return nullptr;
            auto* tex = Runtime::ResourceRegistry::GetTextureService()->Get( Common::UUID( handle ) );
            if ( tex == nullptr )
                return nullptr;
            return dynamic_cast<const Image2D*>(
                 Runtime::ResourceRegistry::GetImageService()->Resolve( tex->GetImageHandle() ) );
        }

    } // namespace

    std::shared_ptr<Image2D> CreateTerrainEmptyLayerArray()
    {
        const ResourceAttributionScope      owned( ResourceOwner::SceneRenderer );
        Core::Formats::Image2DSpecification white = {
             .Tag        = "TerrainLayerArrayWhite",
             .Width      = 1,
             .Height     = 1,
             .Format     = Core::Formats::ImageFormat::RGBA8F,
             .Mips       = 1,
             .Samples    = 1,
             .Data       = Core::Formats::EmptyPixelData{},
             .Usage      = Core::Formats::Image2DUsage::Image2D,
             .Properties = Core::Formats::ImageProperties::Sample,
             .MipLevels  = {},
        };
        white.Data                                 = std::vector<unsigned char>{ 255, 255, 255, 255 };
        const auto                          source = Image2D::Create( white );
        Core::Formats::Image2DSpecification spec   = {
               .Tag         = "TerrainLayerArrayEmpty",
               .Width       = 1,
               .Height      = 1,
               .Format      = Core::Formats::ImageFormat::RGBA8F,
               .Mips        = 1,
               .Samples     = 1,
               .Data        = Core::Formats::EmptyPixelData{},
               .Usage       = Core::Formats::Image2DUsage::Image2D,
               .Properties  = Core::Formats::ImageProperties::Sample,
               .MipLevels   = {},
               .ArrayLayers = 1,
        };
        auto array = Image2D::Create( spec );
        if ( !source || !array )
            return nullptr;
        const Image2D* layers[] = { source.get() };
        if ( const auto copied = array->CopyLayersFrom( layers ); !copied.IsSuccess() )
        {
            LOG_ERROR( "[Terrain] the empty layer array could not be filled: {}", copied.GetError() );
            return nullptr;
        }
        return array;
    }

    namespace
    {
        // An array of @p sources' layers in their order, sized and formatted as the first: CopyLayersFrom
        // refuses any source that differs, naming it.
        Common::ResultStr<std::shared_ptr<Image2D>> BuildArray( const std::string&                 tag,
                                                                const std::vector<const Image2D*>& sources )
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): reads the specification only
            auto& first = const_cast<Image2D*>( sources.front() )->GetImageSpecification();
            const ResourceAttributionScope      owned( ResourceOwner::SceneRenderer );
            Core::Formats::Image2DSpecification spec = {
                 .Tag         = tag,
                 .Width       = first.Width,
                 .Height      = first.Height,
                 .Format      = first.Format,
                 .Mips        = sources.front()->GetMipmapLevels(),
                 .Samples     = 1,
                 .Data        = Core::Formats::EmptyPixelData{},
                 .Usage       = Core::Formats::Image2DUsage::Image2D,
                 .Properties  = first.Properties,
                 .MipLevels   = {},
                 .ColorSpace  = first.ColorSpace,
                 .ArrayLayers = static_cast<uint32_t>( sources.size() ),
            };
            auto array = Image2D::Create( spec );
            if ( !array )
                return Common::MakeFormattedError<std::shared_ptr<Image2D>>( "'{}' could not be created", tag );
            if ( const auto copied = array->CopyLayersFrom( sources ); !copied.IsSuccess() )
                return Common::MakeError<std::shared_ptr<Image2D>>( copied.GetError() );
            return Common::MakeSuccess( array );
        }
    } // namespace

    void UpdateTerrainLayerInputs( const Core::Formats::ShaderProgramMeta& schema,
                                   const MaterialOverrides& overrides, const std::string& materialTag,
                                   const std::shared_ptr<Image2D>& emptyArray, TerrainLayerInputs& inputs )
    {
        constexpr size_t kInputs = World::Landscape::kLandscapeMaxWeightLayers;

        // The template's four arrays, in declaration order: that order is the map kind's index.
        uint32_t kinds = 0;
        for ( const auto& p : schema.Params )
            if ( p.IsArrayTexture && kinds < kTerrainLayerMapKinds )
                inputs.ArrayNames[kinds++] = p.Name;

        std::array<std::array<const Image2D*, kInputs>, kTerrainLayerMapKinds> maps{};
        inputs.Names.fill( {} );
        auto* layerInfos = Runtime::ResourceRegistry::GetLandscapeLayerInfoService();
        for ( const auto& p : schema.Params )
        {
            if ( p.ElementIndex < 0 )
                continue;
            if ( p.ElementIndex >= static_cast<int32_t>( kInputs ) )
            {
                LogOnce( std::format( "[Terrain] '{}': '{}' is Element({}, {}), past the {} layer inputs a "
                                      "landscape tile can carry; it is not drawn",
                                      materialTag, p.Name, p.ElementOf, p.ElementIndex, kInputs ) );
                continue;
            }
            const auto k = static_cast<size_t>( p.ElementIndex );
            if ( p.AssetKind == "LandscapeLayerInfoAsset" )
            {
                if ( const uint64_t handle = HandleOf( overrides, p.Name ); handle != 0 && layerInfos != nullptr )
                    if ( const auto* info = layerInfos->Get( Assets::AssetHandle( handle ) ) )
                        inputs.Names[k] = info->LayerName;
                continue;
            }
            for ( uint32_t kind = 0; kind < kinds; ++kind )
                if ( p.IsTexture && p.ElementOf == inputs.ArrayNames[kind] )
                    maps[kind][k] = ImageOf( HandleOf( overrides, p.Name ) );
        }

        // Two inputs naming one layer: the first wins, as UE's LayerBlend takes the first matching input.
        for ( size_t k = 0; k < kInputs; ++k )
            for ( size_t j = 0; j < k; ++j )
                if ( !inputs.Names[k].empty() && inputs.Names[k] == inputs.Names[j] )
                {
                    LogOnce( std::format( "[Terrain] '{}': layer inputs {} and {} both name layer '{}'; input {} "
                                          "draws it",
                                          materialTag, j, k, inputs.Names[k], j ) );
                    inputs.Names[k].clear();
                }

        for ( auto& s : inputs.Slices )
            s = glm::vec4( -1.0f );
        for ( uint32_t kind = 0; kind < kTerrainLayerMapKinds; ++kind )
        {
            std::vector<const Image2D*>  sources;
            std::array<int32_t, kInputs> slice{};
            slice.fill( -1 );
            for ( size_t k = 0; k < kInputs; ++k )
                if ( maps[kind][k] != nullptr )
                {
                    slice[k] = static_cast<int32_t>( sources.size() );
                    sources.push_back( maps[kind][k] );
                }

            // Rebuilt only when what it is copied from changes; a refused build keeps its sources too, so it is
            // retried when a texture changes and not on every frame.
            if ( sources != inputs.Sources[kind] || !inputs.Arrays[kind] )
            {
                inputs.Sources[kind] = sources;
                inputs.Filled[kind]  = false;
                inputs.Arrays[kind]  = emptyArray;
                if ( !sources.empty() )
                {
                    auto built = BuildArray( materialTag + "/" + inputs.ArrayNames[kind], sources );
                    if ( built.IsSuccess() )
                    {
                        inputs.Arrays[kind] = built.GetValue();
                        inputs.Filled[kind] = true;
                    }
                    else
                        LogOnce( std::format( "[Terrain] '{}': the {} array is not built, so every input draws "
                                              "that map kind's flat default: {}",
                                              materialTag, inputs.ArrayNames[kind], built.GetError() ) );
                }
            }
            for ( size_t k = 0; k < kInputs; ++k )
                inputs.Slices[k][static_cast<glm::length_t>( kind )] =
                     inputs.Filled[kind] ? static_cast<float>( slice[k] ) : -1.0f;
        }
    }
} // namespace Desert::Graphic::System
