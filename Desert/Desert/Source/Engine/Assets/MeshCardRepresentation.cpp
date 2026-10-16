#include <Engine/Assets/MeshCardRepresentation.hpp>

#include <bit>
#include <cstring>

namespace Desert::Assets
{
    namespace
    {
        constexpr char kCardMagic[8] = { 'D', 'E', 'S', 'T', 'C', 'A', 'R', 'D' };

        std::mutex       s_CardBuilderMutex;
        MeshCardsBuilder s_CardBuilder;

        // Little-endian fixed width: the container is read on the platform that did not write it.
        void PutU32( std::string& out, const uint32_t v )
        {
            const char b[4] = { static_cast<char>( v ), static_cast<char>( v >> 8 ), static_cast<char>( v >> 16 ),
                                static_cast<char>( v >> 24 ) };
            out.append( b, 4 );
        }
        void PutF32( std::string& out, const float v )
        {
            PutU32( out, std::bit_cast<uint32_t>( v ) );
        }
        void PutVec3( std::string& out, const glm::vec3& v )
        {
            PutF32( out, v.x );
            PutF32( out, v.y );
            PutF32( out, v.z );
        }

        struct MeshCardReader
        {
            std::string_view Bytes;
            size_t           At = 0;
            bool             Ok = true;

            uint32_t U32()
            {
                if ( At + 4 > Bytes.size() )
                {
                    Ok = false;
                    return 0;
                }
                const auto* b = reinterpret_cast<const unsigned char*>( Bytes.data() + At );
                At += 4;
                return uint32_t( b[0] ) | uint32_t( b[1] ) << 8 | uint32_t( b[2] ) << 16 | uint32_t( b[3] ) << 24;
            }
            float F32()
            {
                return std::bit_cast<float>( U32() );
            }
            glm::vec3 Vec3()
            {
                const float x = F32(), y = F32(), z = F32();
                return glm::vec3( x, y, z );
            }
        };
    } // namespace

    std::string EncodeMeshCards( const MeshCardsBuildData& data )
    {
        std::string out( kCardMagic, sizeof( kCardMagic ) );
        PutU32( out, kMeshCardsFormatVersion );
        PutVec3( out, data.BoundsMin );
        PutVec3( out, data.BoundsMax );
        PutU32( out, data.MostlyTwoSided ? 1u : 0u );
        PutU32( out, static_cast<uint32_t>( data.Cards.size() ) );
        for ( const CardBuildData& card : data.Cards )
        {
            PutVec3( out, card.Center );
            PutVec3( out, card.Extent );
            PutU32( out, static_cast<uint32_t>( card.Direction ) );
        }
        return out;
    }

    Common::ResultStr<MeshCardsBuildData> DecodeMeshCards( const std::string_view bytes )
    {
        if ( bytes.size() < sizeof( kCardMagic ) ||
             std::memcmp( bytes.data(), kCardMagic, sizeof( kCardMagic ) ) != 0 )
            return Common::MakeError<MeshCardsBuildData>( "not a DESTCARD container" );
        MeshCardReader r{ bytes, sizeof( kCardMagic ) };
        const uint32_t version = r.U32();
        if ( version != kMeshCardsFormatVersion )
            return Common::MakeFormattedError<MeshCardsBuildData>( "DESTCARD version {} (this build reads {})",
                                                                   version, kMeshCardsFormatVersion );
        MeshCardsBuildData data;
        data.BoundsMin       = r.Vec3();
        data.BoundsMax       = r.Vec3();
        data.MostlyTwoSided  = r.U32() != 0;
        const uint32_t count = r.U32();
        if ( !r.Ok || count > kMaxCardsPerMesh )
            return Common::MakeFormattedError<MeshCardsBuildData>( "DESTCARD header truncated or {} cards",
                                                                   count );
        for ( uint32_t i = 0; i < count; ++i )
        {
            CardBuildData card;
            card.Center        = r.Vec3();
            card.Extent        = r.Vec3();
            const uint32_t dir = r.U32();
            if ( dir >= kCardDirections )
                return Common::MakeFormattedError<MeshCardsBuildData>( "DESTCARD card {} faces direction {}", i,
                                                                       dir );
            card.Direction = static_cast<CardDirection>( dir );
            data.Cards.push_back( card );
        }
        if ( !r.Ok || r.At != bytes.size() )
            return Common::MakeError<MeshCardsBuildData>( "DESTCARD truncated or trailing bytes" );
        return Common::MakeSuccess( data );
    }

    uint64_t MeshCardsDerivedDataKey( const uint64_t renderDataKey, const MeshCardsBuildSettings& settings,
                                      const std::vector<uint8_t>& submeshTwoSided )
    {
        std::string image;
        PutU32( image, kMeshCardsBuilderVersion );
        PutU32( image, kMeshCardsFormatVersion );
        PutF32( image, settings.TargetVoxelSize );
        PutF32( image, settings.MinVoxelSize );
        PutU32( image, settings.MinVoxelsAcrossThinAxis );
        PutU32( image, settings.MaxVoxels );
        PutU32( image, settings.TargetSurfels );
        PutF32( image, settings.MinDensity );
        PutF32( image, settings.NormalThreshold );
        PutU32( image, settings.MaxCards );
        PutU32( image, static_cast<uint32_t>( submeshTwoSided.size() ) );
        for ( const uint8_t twoSided : submeshTwoSided )
            PutU32( image, twoSided != 0 ? 1u : 0u );
        return Common::DDC::MakeKey( kMeshCardsDeriver, renderDataKey, image.data(), image.size() );
    }

    void SetMeshCardsBuilder( MeshCardsBuilder builder )
    {
        const std::lock_guard<std::mutex> lock( s_CardBuilderMutex );
        s_CardBuilder = std::move( builder );
    }

    Common::ResultStr<std::string> BuildMeshCardsBytes( const MeshDistanceFieldBuildInput& input )
    {
        auto built = BuildMeshCards( input, MeshCardsBuildSettings{} );
        if ( !built.IsSuccess() )
            return Common::MakeError<std::string>( built.GetError() );
        return Common::MakeSuccess( EncodeMeshCards( built.GetValue() ) );
    }

    Common::ResultStr<std::shared_ptr<const MeshCardsBuildData>>
    LoadMeshCards( const std::filesystem::path& asset, const uint64_t renderDataKey,
                   const std::vector<uint8_t>& submeshTwoSided, const MeshDistanceFieldBuildInput& input )
    {
        using Result         = std::shared_ptr<const MeshCardsBuildData>;
        const uint64_t   key = MeshCardsDerivedDataKey( renderDataKey, MeshCardsBuildSettings{}, submeshTwoSided );
        MeshCardsBuilder builder;
        {
            const std::lock_guard<std::mutex> lock( s_CardBuilderMutex );
            builder = s_CardBuilder;
        }
        std::string bytes;
        if ( !builder )
        {
            auto hit = Common::DDC::Get( kMeshCardsDeriver, key );
            if ( !hit.has_value() )
                return Common::MakeFormattedError<Result>(
                     "mesh asset '{}' has no cards under DDC key {:016x} ({}), and this build cannot derive them: "
                     "the package was cooked without them",
                     asset.string(), key, Common::DDC::RelativePath( kMeshCardsDeriver, key ).generic_string() );
            bytes = std::move( *hit );
        }
        else
        {
            auto derived = Common::DDC::GetOrBuild( kMeshCardsDeriver, key,
                                                    [&builder, &input]() { return builder( input ); } );
            if ( !derived.IsSuccess() )
                return Common::MakeFormattedError<Result>( "mesh asset '{}': cards: {}", asset.string(),
                                                           derived.GetError() );
            bytes = derived.ExtractValue();
        }
        auto decoded = DecodeMeshCards( bytes );
        if ( !decoded.IsSuccess() )
            return Common::MakeFormattedError<Result>( "mesh asset '{}': cached cards {:016x} are corrupt: {}",
                                                       asset.string(), key, decoded.GetError() );
        return Common::MakeSuccess<Result>( std::make_shared<const MeshCardsBuildData>( decoded.ExtractValue() ) );
    }

    std::shared_ptr<const MeshCardsBuildData> MeshCardsSlot::Get() const
    {
        const std::lock_guard<std::mutex> lock( m_Mutex );
        return m_Cards;
    }

    void MeshCardsSlot::Publish( std::shared_ptr<const MeshCardsBuildData> cards )
    {
        const std::lock_guard<std::mutex> lock( m_Mutex );
        m_Cards = std::move( cards );
    }
} // namespace Desert::Assets
