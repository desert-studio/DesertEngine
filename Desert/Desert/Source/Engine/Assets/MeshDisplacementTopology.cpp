#include <Engine/Assets/MeshDisplacementTopology.hpp>

#include <array>
#include <cstring>
#include <format>
#include <mutex>
#include <span>

namespace Desert::Assets
{
    namespace
    {
        std::mutex                  s_BuilderMutex;
        DisplacementTopologyBuilder s_Builder;
    } // namespace

    uint64_t DisplacementTopologyDerivedDataKey( const uint64_t renderDataKey )
    {
        std::array<uint32_t, 2> image = { kDisplacementTopologyBuilderVersion,
                                          Geometry::kDisplacementClusterTriangles };
        return Common::DDC::MakeKey( kDisplacementTopologyDeriver, renderDataKey, image.data(), sizeof( image ) );
    }

    void SetDisplacementTopologyBuilder( DisplacementTopologyBuilder builder )
    {
        const std::lock_guard<std::mutex> lock( s_BuilderMutex );
        s_Builder = std::move( builder );
    }

    Common::ResultStr<std::string> BuildDisplacementTopologyBytes( const DisplacementTopologyBuildInput& input )
    {
        auto built = Geometry::BuildDisplacementTopology( input.Vertices, input.Indices, input.Submeshes );
        if ( !built.IsSuccess() )
            return Common::MakeError<std::string>( built.GetError() );
        const std::vector<uint8_t> bytes = Geometry::EncodeDisplacementTopology( built.GetValue() );
        return Common::MakeSuccess( std::string( reinterpret_cast<const char*>( bytes.data() ), bytes.size() ) );
    }

    Common::ResultStr<std::shared_ptr<const Geometry::DisplacementTopology>>
    LoadDisplacementTopology( const std::filesystem::path& asset, const uint64_t renderDataKey,
                              const std::function<Common::ResultStr<DisplacementTopologyBuildInput>()>& input )
    {
        using Result                    = std::shared_ptr<const Geometry::DisplacementTopology>;
        const uint64_t              key = DisplacementTopologyDerivedDataKey( renderDataKey );
        DisplacementTopologyBuilder builder;
        {
            const std::lock_guard<std::mutex> lock( s_BuilderMutex );
            builder = s_Builder;
        }
        std::string bytes;
        if ( !builder )
        {
            auto hit = Common::DDC::Get( kDisplacementTopologyDeriver, key );
            if ( !hit.has_value() )
                return Common::MakeFormattedError<Result>(
                     "mesh asset '{}' has no displacement topology under DDC key {:016x} ({}), and this build "
                     "cannot "
                     "derive it: the package was cooked without it",
                     asset.string(), key,
                     Common::DDC::RelativePath( kDisplacementTopologyDeriver, key ).generic_string() );
            bytes = std::move( *hit );
        }
        else
        {
            auto derived = Common::DDC::GetOrBuild( kDisplacementTopologyDeriver, key,
                                                    [&builder, &input]() -> Common::ResultStr<std::string>
                                                    {
                                                        auto in = input();
                                                        if ( !in.IsSuccess() )
                                                            return Common::MakeError<std::string>( in.GetError() );
                                                        return builder( in.GetValue() );
                                                    } );
            if ( !derived.IsSuccess() )
                return Common::MakeFormattedError<Result>( "mesh asset '{}': displacement topology: {}",
                                                           asset.string(), derived.GetError() );
            bytes = derived.ExtractValue();
        }
        auto decoded = Geometry::DecodeDisplacementTopology(
             std::span( reinterpret_cast<const uint8_t*>( bytes.data() ), bytes.size() ) );
        if ( !decoded.IsSuccess() )
            return Common::MakeFormattedError<Result>(
                 "mesh asset '{}': cached displacement topology {:016x} is corrupt: {}", asset.string(), key,
                 decoded.GetError() );
        return Common::MakeSuccess<Result>(
             std::make_shared<const Geometry::DisplacementTopology>( decoded.ExtractValue() ) );
    }
} // namespace Desert::Assets
