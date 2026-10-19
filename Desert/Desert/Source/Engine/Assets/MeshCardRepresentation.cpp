#include <Engine/Assets/MeshCardRepresentation.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <map>

namespace Desert::Assets
{
    namespace
    {
        constexpr char kCardMagic[8] = { 'D', 'E', 'S', 'T', 'C', 'A', 'R', 'D' };

        std::mutex       s_CardBuilderMutex;
        MeshCardsBuilder s_CardBuilder;

        std::mutex                                    s_CoverageMutex;
        std::map<std::string, MeshCardsCoverageEntry> s_Coverage; // by asset path

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
        PutU32( image, settings.MaxVoxelsLarge );
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

    Common::ResultStr<std::string> BuildMeshCardsBytes( const MeshDistanceFieldBuildInput& input,
                                                        const MeshDistanceFieldCancel&     cancel )
    {
        auto built = BuildMeshCards( input, MeshCardsBuildSettings{}, cancel );
        if ( !built.IsSuccess() )
            return Common::MakeError<std::string>( built.GetError() );
        return Common::MakeSuccess( EncodeMeshCards( built.GetValue() ) );
    }

    Common::ResultStr<std::shared_ptr<const MeshCardsBuildData>>
    LoadMeshCards( const std::filesystem::path& asset, const uint64_t renderDataKey,
                   const std::vector<uint8_t>& submeshTwoSided, const MeshDistanceFieldBuildInput& input,
                   const MeshDistanceFieldCancel& cancel )
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
            auto derived = Common::DDC::GetOrBuild( kMeshCardsDeriver, key, [&builder, &input, &cancel]()
                                                    { return builder( input, cancel ); } );
            if ( !derived.IsSuccess() )
                return Common::MakeFormattedError<Result>( "mesh asset '{}': cards: {}", asset.string(),
                                                           derived.GetError() );
            bytes = derived.ExtractValue();
        }
        auto decoded = DecodeMeshCards( bytes );
        if ( !decoded.IsSuccess() )
            return Common::MakeFormattedError<Result>( "mesh asset '{}': cached cards {:016x} are corrupt: {}",
                                                       asset.string(), key, decoded.GetError() );
        // A cancelled job measures nothing: nobody reads the report of a mesh that is unloading or a process
        // that is exiting, and the measurement casts thousands of rays.
        if ( builder && !cancel.IsCancelled() )
        {
            // The coverage report (the editor / the cook only: a packaged game has nothing to report to).
            std::string image;
            PutU32( image, kMeshCardsCoverageVersion );
            PutU32( image, kMeshCardsCoverageSamples );
            const uint64_t coverageKey =
                 Common::DDC::MakeKey( kMeshCardsCoverageDeriver, key, image.data(), image.size() );
            const MeshCardsBuildData& cards    = decoded.GetValue();
            auto                      measured = Common::DDC::GetOrBuild( kMeshCardsCoverageDeriver, coverageKey,
                                                                          [&input, &cards]() -> Common::ResultStr<std::string>
                                                                          {
                                                         std::string             bytes;
                                                         const MeshCardsCoverage coverage =
                                                              MeasureMeshCardsCoverage( input, cards );
                                                         PutF32( bytes, coverage.Held );
                                                         PutF32( bytes, coverage.BackOnly );
                                                         return Common::MakeSuccess( bytes );
                                                     } );
            if ( !measured.IsSuccess() || measured.GetValue().size() != 8 )
            {
                LOG_ERROR( "mesh asset '{}': card coverage {:016x}: {}", asset.string(), coverageKey,
                           measured.IsSuccess() ? std::string( "not 8 bytes" ) : measured.GetError() );
            }
            else
            {
                MeshCardReader    r{ measured.GetValue(), 0 };
                MeshCardsCoverage coverage;
                coverage.Held     = r.F32();
                coverage.BackOnly = r.F32();
                RecordMeshCardsCoverage( asset, coverage, cards );
            }
        }
        return Common::MakeSuccess<Result>( std::make_shared<const MeshCardsBuildData>( decoded.ExtractValue() ) );
    }

    void RecordMeshCardsCoverage( const std::filesystem::path& asset, const MeshCardsCoverage& coverage,
                                  const MeshCardsBuildData& cards )
    {
        MeshCardsCoverageEntry entry{ asset.generic_string(), coverage.Held, coverage.BackOnly,
                                      static_cast<uint32_t>( cards.Cards.size() ),
                                      cards.BoundsMax - cards.BoundsMin };
        if ( entry.Coverage < kMeshCardsLowCoverage )
        {
            LOG_WARN(
                 "[MeshCards] '{}' ({:.0f} x {:.0f} x {:.0f} cm): its {} card(s) hold {:.0f} % of its surface "
                 "(below {:.0f} %; {:.0f} % of its one-sided surface is seen only from behind); a ray hit on "
                 "the rest is lit in place, not from the surface cache",
                 entry.Asset, entry.Size.x, entry.Size.y, entry.Size.z, entry.Cards, entry.Coverage * 100.0f,
                 kMeshCardsLowCoverage * 100.0f, entry.BackOnly * 100.0f );
        }
        const std::lock_guard<std::mutex> lock( s_CoverageMutex );
        s_Coverage[entry.Asset] = std::move( entry );
    }

    MeshCardsCoverageReport MeshCardsCoverageSummary()
    {
        MeshCardsCoverageReport report;
        double                  sum = 0.0;
        {
            const std::lock_guard<std::mutex> lock( s_CoverageMutex );
            for ( const auto& [asset, entry] : s_Coverage )
            {
                ++report.Meshes;
                sum += entry.Coverage;
                if ( entry.Coverage < kMeshCardsLowCoverage )
                    report.Low.push_back( entry );
            }
        }
        report.MeanCoverage = report.Meshes != 0 ? static_cast<float>( sum / report.Meshes ) : 1.0f;
        std::sort( report.Low.begin(), report.Low.end(),
                   []( const MeshCardsCoverageEntry& a, const MeshCardsCoverageEntry& b )
                   { return a.Coverage != b.Coverage ? a.Coverage < b.Coverage : a.Asset < b.Asset; } );
        return report;
    }

    std::string FormatMeshCardsCoverage( const MeshCardsCoverageReport& report, const size_t listed )
    {
        std::string line =
             std::format( "{} mesh(es), mean {:.1f} %, {} below {:.0f} %", report.Meshes,
                          report.MeanCoverage * 100.0f, report.Low.size(), kMeshCardsLowCoverage * 100.0f );
        for ( size_t i = 0; i < std::min( listed, report.Low.size() ); ++i )
            line += std::format( "{}{} {:.0f} %", i == 0 ? ": " : ", ",
                                 std::filesystem::path( report.Low[i].Asset ).filename().string(),
                                 report.Low[i].Coverage * 100.0f );
        if ( report.Low.size() > listed )
            line += std::format( ", ... (+{})", report.Low.size() - listed );
        return line;
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
