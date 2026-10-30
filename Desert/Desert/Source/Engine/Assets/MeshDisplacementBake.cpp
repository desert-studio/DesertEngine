#include <Engine/Assets/MeshDisplacementBake.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/MaterialData.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/MeshDisplacementTopology.hpp>
#include <Engine/Assets/RegistryDiscovery.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Assets/Shader/ShaderAsset.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Engine/Core/ShaderCompiler/ShaderPreprocess/ShaderPreprocessor.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <mutex>

namespace Desert::Assets
{
    namespace
    {
        std::mutex           s_DisplacedMeshBuilderMutex;
        DisplacedMeshBuilder s_DisplacedMeshBuilder;

    } // namespace

        // The chain a material's values resolve through: the material first, its template-owning root last.

        Common::ResultStr<MaterialChain> ResolveMaterialChain( AssetManager&                     manager,
                                                       const Common::Content::AssetGuid& material )
        {
            // ResolveMaterialTwoSided and MaterialService::CreateRuntimeInstance walk the same chain, as deep.
            constexpr int              kMaxChainDepth = 16;
            Common::Content::AssetGuid current        = material;
            MaterialChain              chain;
            for ( int depth = 0; depth < kMaxChainDepth; ++depth )
            {
                auto asset = manager.FindByHandle<SurfaceMaterialAsset>(
                     Common::AssetHandle( static_cast<uint64_t>( Common::Content::HandleForGuid( current ) ) ) );
                if ( !asset )
                    asset = CreateFromRegistryGuid<SurfaceMaterialAsset>( manager, current,
                                                                          Common::Content::ContentKind::Material );
                if ( !asset )
                    return Common::MakeFormattedError<MaterialChain>(
                         "material {} is not a material this project has scanned",
                         Common::Content::AssetGuidToText( current ) );
                if ( const auto loaded = asset->EnsureLoaded( manager ); !loaded )
                    return Common::MakeFormattedError<MaterialChain>(
                         "material '{}' could not be read: {}", asset->GetMetadata().Filepath.generic_string(),
                         loaded.GetError() );
                chain.Links.push_back( asset );
                const MaterialData& data = asset->Data();
                if ( data.IsInstance() )
                {
                    const auto parent = data.ParentGuid();
                    if ( parent.IsNull() || parent == current )
                        return Common::MakeFormattedError<MaterialChain>(
                             "material '{}' names a malformed or its own parent '{}'",
                             asset->GetMetadata().Filepath.generic_string(), *data.ParentText() );
                    current = parent;
                    continue;
                }
                const auto shader = manager.FindByHandle<ShaderAsset>( asset->GetShaderHandle() );
                if ( !shader )
                    return Common::MakeFormattedError<MaterialChain>(
                         "material '{}' has no resolved template, so its Height is unknown",
                         asset->GetMetadata().Filepath.generic_string() );
                auto meta = Core::Preprocess::ShaderPreprocess::ParseProgramMetaForPass(
                     shader->GetShaderContent(), shader->GetMetadata().Filepath.parent_path(), "" );
                if ( !meta.IsSuccess() )
                    return Common::MakeFormattedError<MaterialChain>(
                         "template '{}' of material '{}': {}", shader->GetMetadata().Filepath.generic_string(),
                         asset->GetMetadata().Filepath.generic_string(), meta.GetError() );
                chain.Meta = meta.ExtractValue();
                return Common::MakeSuccess( std::move( chain ) );
            }
            return Common::MakeFormattedError<MaterialChain>(
                 "material {}: instance chain deeper than {} (a cycle?)",
                 Common::Content::AssetGuidToText( material ), kMaxChainDepth );
        }

        // Child first, then each parent, then the template's default (MaterialData.hpp: silence is answered by
        // the chain, then the shader).
        glm::vec4 MaterialChainParam( const MaterialChain& chain, const std::string& name )
        {
            for ( const auto& link : chain.Links )
                if ( const glm::vec4* value = link->Data().FindParam( name ) )
                    return *value;
            for ( const auto& param : chain.Meta.Params )
                if ( param.Name == name )
                    return param.Default;
            return glm::vec4( 0.0f );
        }

    namespace
    {

        template <typename T>
        void HashValue( uint64_t& h, const T& value )
        {
            std::array<unsigned char, sizeof( T )> bytes{};
            std::memcpy( bytes.data(), &value, sizeof( T ) );
            for ( const unsigned char b : bytes )
            {
                h ^= b;
                h *= 1099511628211ull;
            }
        }

        std::shared_ptr<const DisplacedMeshData> FromBinary( const Serialization::MeshAssetData& data,
                                                             uint64_t                            key )
        {
            auto out = std::make_shared<DisplacedMeshData>();
            out->Key = key;
            out->Vertices.reserve( data.StaticVertices.size() );
            for ( const auto& v : data.StaticVertices )
                out->Vertices.push_back( { v.Position, v.Normal, v.Tangent, v.Bitangent, v.TexCoord } );
            out->Indices.reserve( data.Indices.size() );
            for ( const auto& i : data.Indices )
                out->Indices.push_back( { i.V1, i.V2, i.V3 } );
            for ( const auto& s : data.Submeshes )
            {
                Submesh submesh{ s.Name,       s.VertexOffset, s.VertexCount, s.IndexOffset,
                                 s.IndexCount, s.Transform,    s.BoundingBox };
                submesh.LodChainCooked = s.LodChainCooked;
                for ( const auto& level : s.LODs )
                {
                    std::vector<Index> tris;
                    tris.reserve( level.size() );
                    for ( const auto& t : level )
                        tris.push_back( { t.V1, t.V2, t.V3 } );
                    submesh.BakedLODs.push_back( std::move( tris ) );
                }
                out->Submeshes.push_back( std::move( submesh ) );
            }
            out->Streams = PackMeshVertexStreams( data.Colors, data.UV1, out->Vertices.size() );
            return out;
        }

        Common::ResultStr<std::shared_ptr<const DisplacedMeshData>>
        Decode( const std::filesystem::path& asset, uint64_t key, const std::string& bytes )
        {
            using Result = std::shared_ptr<const DisplacedMeshData>;
            auto decoded = Serialization::DecodeMeshBinary( bytes, asset.generic_string() );
            if ( !decoded.IsSuccess() )
                return Common::MakeFormattedError<Result>(
                     "mesh asset '{}': displaced mesh {:016x} is corrupt: {}", asset.string(), key,
                     decoded.GetError() );
            return Common::MakeSuccess( FromBinary( decoded.GetValue(), key ) );
        }

        DisplacedMeshBuilder CurrentBuilder()
        {
            const std::lock_guard<std::mutex> lock( s_DisplacedMeshBuilderMutex );
            return s_DisplacedMeshBuilder;
        }
    } // namespace

    Common::ResultStr<std::optional<SubmeshHeightInput>>
    ResolveSubmeshHeight( AssetManager& manager, const Common::Content::AssetGuid& material )
    {
        using Result  = std::optional<SubmeshHeightInput>;
        auto resolved = ResolveMaterialChain( manager, material );
        if ( !resolved.IsSuccess() )
            return Common::MakeError<Result>( resolved.GetError() );
        const MaterialChain& chain = resolved.GetValue();
        if ( !chain.Meta.Height )
            return Common::MakeSuccess<Result>( std::nullopt );
        const Core::Formats::SurfaceHeight& height = chain.Meta.Height.value();

        // The Height texture as the chain assigns it (child first); none assigned = the section is not displaced.
        uint64_t handle = 0;
        for ( const auto& link : chain.Links )
            if ( ( handle = link->Data().GetTexture( height.Texture ) ) != 0 )
                break;
        if ( handle == 0 )
            return Common::MakeSuccess<Result>( std::nullopt );
        const auto row = ContentRegistry::RowOf( Common::Content::ContentKind::Texture, handle );
        if ( !row )
            return Common::MakeFormattedError<Result>(
                 "material '{}': its Height texture '{}' (handle {:016x}) is not a texture this project has "
                 "scanned",
                 chain.Links.front()->GetMetadata().Filepath.generic_string(), height.Texture, handle );
        auto key = ReadTextureAssetKey( row->Path );
        if ( !key.IsSuccess() )
            return Common::MakeFormattedError<Result>(
                 "material '{}': Height texture '{}': {}",
                 chain.Links.front()->GetMetadata().Filepath.generic_string(), row->Path.generic_string(),
                 key.GetError() );

        // Graphic::System::ResolveDisplacementMaterial reads the same parameters from the material row.
        SubmeshHeightInput input;
        input.TextureAsset = row->Path;
        input.TextureKey   = key.GetValue().DerivedDataKey;
        input.UVSet        = height.UVSet;
        input.Channel      = height.Channel;
        if ( !height.ChannelParam.empty() )
            input.Channel =
                 static_cast<uint32_t>( std::clamp( MaterialChainParam( chain, height.ChannelParam ).x, 0.0f, 3.0f ) );
        input.ScaleCm = MaterialChainParam( chain, height.ScaleParam ).x;
        if ( !height.CenterParam.empty() )
            input.Center = std::clamp( MaterialChainParam( chain, height.CenterParam ).x, 0.0f, 1.0f );
        if ( !height.TilingParam.empty() )
        {
            input.Tiling       = glm::vec2( MaterialChainParam( chain, height.TilingParam ) );
            input.Offset       = glm::vec2( MaterialChainParam( chain, height.OffsetParam ) );
            input.Rotation     = MaterialChainParam( chain, height.RotationParam ).x;
            input.HasTransform = true;
        }
        return Common::MakeSuccess<Result>( std::move( input ) );
    }

    uint64_t DisplacedMeshDerivedDataKey( const uint64_t renderDataKey, const std::optional<float> edgeLengthCm,
                                          const bool                                               bakeLODs,
                                          const std::span<const std::optional<SubmeshHeightInput>> heights )
    {
        uint64_t h = 1469598103934665603ull;
        HashValue( h, kDisplacedMeshBuilderVersion );
        HashValue( h, kDisplacementTopologyBuilderVersion );
        HashValue( h, Geometry::kDisplacementClusterTriangles );
        HashValue( h, edgeLengthCm.has_value() );
        HashValue( h, edgeLengthCm ? std::bit_cast<uint32_t>( *edgeLengthCm ) : 0u );
        HashValue( h, bakeLODs );
        HashValue( h, static_cast<uint64_t>( heights.size() ) );
        for ( const auto& height : heights )
        {
            HashValue( h, height.has_value() );
            if ( !height )
                continue;
            // The texture by content (its derived-data key), never by path: a moved file keeps the surface.
            HashValue( h, height->TextureKey );
            HashValue( h, height->Channel );
            HashValue( h, height->UVSet );
            HashValue( h, height->ScaleCm );
            HashValue( h, height->Center );
            HashValue( h, height->Tiling );
            HashValue( h, height->Offset );
            HashValue( h, height->Rotation );
            HashValue( h, height->HasTransform );
        }
        return Common::DDC::MakeKey( kDisplacedStaticMeshDeriver, renderDataKey, &h, sizeof( h ) );
    }

    void SetDisplacedMeshBuilder( DisplacedMeshBuilder builder )
    {
        const std::lock_guard<std::mutex> lock( s_DisplacedMeshBuilderMutex );
        s_DisplacedMeshBuilder = std::move( builder );
    }

    Common::ResultStr<std::shared_ptr<const DisplacedMeshData>>
    LoadDisplacedMesh( const std::filesystem::path& asset, const uint64_t key,
                       const std::function<Common::ResultStr<DisplacedMeshBuildInput>()>& input )
    {
        using Result                       = std::shared_ptr<const DisplacedMeshData>;
        const DisplacedMeshBuilder builder = CurrentBuilder();
        if ( !builder )
        {
            auto hit = Common::DDC::Get( kDisplacedStaticMeshDeriver, key );
            if ( !hit.has_value() )
                return Common::MakeFormattedError<Result>(
                     "mesh asset '{}' has no displaced mesh under DDC key {:016x} ({}), and this build cannot "
                     "derive "
                     "it: the package was cooked without it",
                     asset.string(), key,
                     Common::DDC::RelativePath( kDisplacedStaticMeshDeriver, key ).generic_string() );
            return Decode( asset, key, *hit );
        }
        auto derived = Common::DDC::GetOrBuild( kDisplacedStaticMeshDeriver, key,
                                                [&builder, &input]() -> Common::ResultStr<std::string>
                                                {
                                                    auto in = input();
                                                    if ( !in.IsSuccess() )
                                                        return Common::MakeError<std::string>( in.GetError() );
                                                    return builder( in.GetValue() );
                                                } );
        if ( !derived.IsSuccess() )
            return Common::MakeFormattedError<Result>( "mesh asset '{}': displaced mesh: {}", asset.string(),
                                                       derived.GetError() );
        return Decode( asset, key, derived.GetValue() );
    }

    Common::ResultStr<std::shared_ptr<const DisplacedMeshData>>
    BuildDisplacedMesh( const std::filesystem::path& asset, const uint64_t key,
                        const DisplacedMeshBuildInput& input )
    {
        using Result                       = std::shared_ptr<const DisplacedMeshData>;
        const DisplacedMeshBuilder builder = CurrentBuilder();
        if ( !builder )
            return Common::MakeFormattedError<Result>( "mesh asset '{}': the displaced surface {:016x} cannot be "
                                                       "built here (no builder: a packaged game "
                                                       "reads the products derived from it, never the surface)",
                                                       asset.string(), key );
        auto built = builder( input );
        if ( !built.IsSuccess() )
            return Common::MakeFormattedError<Result>( "mesh asset '{}': displaced surface: {}", asset.string(),
                                                       built.GetError() );
        return Decode( asset, key, built.GetValue() );
    }
} // namespace Desert::Assets
