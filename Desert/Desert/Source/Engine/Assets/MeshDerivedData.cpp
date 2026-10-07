// Ported from UE 5.8 Engine/Source/Runtime/Engine/Private/StaticMesh.cpp:3678 (STATICMESH_DERIVEDDATA_VER),
// :3719-3981 (BuildStaticMeshDerivedDataKey) and :4172 (FStaticMeshRenderData::Cache), adapted: the key is
// Common::DDC::MakeKey over our SRCE hash + a fixed u32 settings image instead of FDerivedDataCacheInterface
// string keys over the MeshDescription id + FMeshBuildSettings per LOD + platform; the build step is a
// registered function (the editor's) instead of IMeshBuilderModule, so a packaged game carries no builder.
#include <Engine/Assets/MeshDerivedData.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/ImportRecord.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/PakFile.hpp>

#include <bit>
#include <mutex>
#include <optional>
#include <span>
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
        auto source = LoadMeshSourceAsset( asset );
        if ( !source.IsSuccess() )
            return Common::MakeError<std::string>( source.GetError() );
        const uint64_t key = MeshAssetDerivedDataKey( source.GetValue() );

        MeshPlatformDataBuilder builder;
        {
            const std::lock_guard<std::mutex> lock( s_BuilderMutex );
            builder = s_Builder;
        }
        if ( !builder )
        {
            if ( auto hit = Common::DDC::Get( kMeshDeriver, key ); hit.has_value() )
                return Common::MakeSuccess( std::move( *hit ) );
            return Common::MakeFormattedError<std::string>(
                 "mesh asset '{}' has no render data under DDC key {:016x} ({}), and this build cannot derive "
                 "it: the package was cooked without it",
                 asset.string(), key, Common::DDC::RelativePath( kMeshDeriver, key ).generic_string() );
        }

        // Single-flight per key: assets with identical source share the key and load on parallel threads.
        auto derived = Common::DDC::GetOrBuild( kMeshDeriver, key,
                                                [&builder, &source] { return builder( source.GetValue() ); } );
        if ( !derived.IsSuccess() )
            return Common::MakeFormattedError<std::string>( "mesh asset '{}': render data: {}", asset.string(),
                                                            derived.GetError() );
        return derived;
    }

    Common::ResultStr<uint64_t> HashMeshSourceFile( const std::filesystem::path& file )
    {
        const auto bytes = Common::Utils::FileSystem::ReadFileContent( file );
        if ( !bytes.IsSuccess() )
            return Common::MakeFormattedError<uint64_t>( "'{}' cannot be read: {}", file.string(),
                                                         bytes.GetError() );
        return Common::MakeSuccess(
             Common::Utils::PakContentHash( bytes.GetValue().data(), bytes.GetValue().size() ) );
    }

    uint64_t MeshSourceDerivedDataKey( const uint64_t sourceFileHash )
    {
        return Common::DDC::MakeKey( kMeshSourceDeriver, sourceFileHash, &kMeshSourceBuilderVersion,
                                     sizeof( kMeshSourceBuilderVersion ) );
    }

    bool IsEditedImportedMesh( const std::filesystem::path& assetPath )
    {
        std::error_code ec;
        if ( !std::filesystem::is_regular_file( assetPath, ec ) )
            return false;
        const auto companion = Common::Content::MeshSourceBeside( assetPath );
        if ( !companion.has_value() )
            return false;
        const std::filesystem::path record = Common::Content::ImportRecordPathFor( *companion );
        if ( !std::filesystem::is_regular_file( record, ec ) )
            return false;
        const auto recordHeader = Common::Content::ReadAssetHeaderIfStated( record, { {}, true } );
        const auto assetHeader =
             Common::Content::ReadAssetHeaderIfStated( assetPath, MeshAssetHeaderReadContext() );
        if ( !recordHeader.IsSuccess() || !assetHeader.IsSuccess() || !recordHeader.GetValue().has_value() ||
             !assetHeader.GetValue().has_value() )
            return false;
        const auto& guid = recordHeader.GetValue()->Guid;
        return !guid.IsNull() && assetHeader.GetValue()->Guid == guid;
    }

    Common::ResultStr<MeshSourceAsset> LoadMeshSourceAsset( const std::filesystem::path& assetPath )
    {
        const auto companion = Common::Content::MeshSourceBeside( assetPath );
        if ( !companion.has_value() || IsEditedImportedMesh( assetPath ) )
            return ReadMeshSourceAssetFile( assetPath ); // hand-authored (AF4h c) or an edited import (P9b)

        const auto hash = HashMeshSourceFile( *companion );
        if ( !hash.IsSuccess() )
            return Common::MakeError<MeshSourceAsset>( hash.GetError() );
        const uint64_t key = MeshSourceDerivedDataKey( hash.GetValue() );
        auto           hit = Common::DDC::Get( kMeshSourceDeriver, key );
        if ( !hit.has_value() )
            return Common::MakeFormattedError<MeshSourceAsset>(
                 "'{}' has no imported source under DDC key {:016x} ({}) for '{}': re-import it (Assets > "
                 "Rebuild Cooked Assets) - a stale file beside the source, if one exists, is never read in "
                 "its place",
                 assetPath.string(), key, Common::DDC::RelativePath( kMeshSourceDeriver, key ).generic_string(),
                 companion->string() );
        auto decoded = DecodeMeshSourceAsset( std::as_bytes( std::span<const char>( hit->data(), hit->size() ) ) );
        if ( !decoded.IsSuccess() )
            return Common::MakeFormattedError<MeshSourceAsset>( "'{}': cached imported source is corrupt: {}",
                                                                assetPath.string(), decoded.GetError() );
        return decoded;
    }
} // namespace Desert::Assets
