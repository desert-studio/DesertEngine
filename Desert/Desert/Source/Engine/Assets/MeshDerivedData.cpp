// Ported from UE 5.8 Engine/Source/Runtime/Engine/Private/StaticMesh.cpp:3678 (STATICMESH_DERIVEDDATA_VER),
// :3719-3981 (BuildStaticMeshDerivedDataKey) and :4172 (FStaticMeshRenderData::Cache), adapted: the key is
// Common::DDC::MakeKey over our SRCE hash + a fixed u32 settings image instead of FDerivedDataCacheInterface
// string keys over the MeshDescription id + FMeshBuildSettings per LOD + platform; the build step is a
// registered function (the editor's) instead of IMeshBuilderModule, so a packaged game carries no builder.
#include <Engine/Assets/MeshDerivedData.hpp>

#include <bit>
#include <mutex>
#include <utility>

namespace Desert::Assets
{
    namespace
    {
        void PutU32( std::vector<std::byte>& out, const uint32_t v )
        {
            for ( int i = 0; i < 4; ++i )
                out.push_back( static_cast<std::byte>( ( v >> ( 8 * i ) ) & 0xFFu ) );
        }

        std::mutex              s_BuilderMutex;
        MeshPlatformDataBuilder s_Builder;
    } // namespace

    std::vector<std::byte> SerializeMeshSettingsForKey( const MeshBuildSettings& settings )
    {
        std::vector<std::byte> out;
        PutU32( out, std::bit_cast<uint32_t>( settings.Import.UniformScale ) );
        PutU32( out, static_cast<uint32_t>( settings.Import.UpAxis ) );
        PutU32( out, static_cast<uint32_t>( settings.Import.LodPolicy ) );
        PutU32( out, settings.BuilderVersion );
        return out;
    }

    uint64_t MeshDerivedDataKey( const uint64_t sourceHash, const MeshBuildSettings& settings,
                                 const Common::DDC::Deriver& deriver )
    {
        const std::vector<std::byte> image = SerializeMeshSettingsForKey( settings );
        return Common::DDC::MakeKey( deriver, sourceHash, image.data(), image.size() );
    }

    uint64_t MeshAssetDerivedDataKey( const MeshSourceAsset& asset )
    {
        return MeshDerivedDataKey( MeshSourceHash( asset.Source ), MeshBuildSettings{ asset.Import.Settings } );
    }

    void SetMeshPlatformDataBuilder( MeshPlatformDataBuilder builder )
    {
        const std::lock_guard<std::mutex> lock( s_BuilderMutex );
        s_Builder = std::move( builder );
    }

    Common::ResultStr<std::string> LoadMeshPlatformData( const std::filesystem::path& asset )
    {
        auto source = ReadMeshSourceAssetFile( asset );
        if ( !source.IsSuccess() )
            return Common::MakeError<std::string>( source.GetError() );
        const uint64_t key = MeshAssetDerivedDataKey( source.GetValue() );
        if ( auto hit = Common::DDC::Get( kMeshDeriver, key ); hit.has_value() )
            return Common::MakeSuccess( std::move( *hit ) );

        MeshPlatformDataBuilder builder;
        {
            const std::lock_guard<std::mutex> lock( s_BuilderMutex );
            builder = s_Builder;
        }
        if ( !builder )
            return Common::MakeFormattedError<std::string>(
                 "mesh asset '{}' has no render data under DDC key {:016x} ({}), and this build cannot derive "
                 "it: the package was cooked without it",
                 asset.string(), key, Common::DDC::RelativePath( kMeshDeriver, key ).generic_string() );

        auto built = builder( source.GetValue() );
        if ( !built.IsSuccess() )
            return Common::MakeFormattedError<std::string>( "mesh asset '{}': {}", asset.string(),
                                                            built.GetError() );
        if ( auto put = Common::DDC::Put( kMeshDeriver, key, built.GetValue() ); !put.IsSuccess() )
            return Common::MakeFormattedError<std::string>( "mesh asset '{}': render data built but not cached: {}",
                                                            asset.string(), put.GetError() );
        return built;
    }
} // namespace Desert::Assets
