#include "CloudLayout.hpp"

#include <Engine/Assets/ContainerBytes.hpp>

#include <Common/Content/ContentKinds.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>

namespace Desert::Assets
{
    namespace
    {
        constexpr uint32_t kPatternBytesPerTexel = 4u;

        /// What the mask's neutral byte is, as a float for the arithmetic below. 128 rather than 0 because
        /// the stored table is UNSIGNED and an artist floods a painting with mid-grey, not with black — see
        /// the note on CloudLayoutData::Mask. DERIVED from the public constant rather than written again:
        /// the brush lays this byte down as an eraser's ink, and a neutral spelt twice is a mask that reads
        /// as very slightly positive everywhere it was erased.
        constexpr float kMaskNeutral = static_cast<float>( kCloudLayoutMaskNeutral );

        /// Which way the two table-presence bits sit in the header's flag word. Written out rather than
        /// implied so that a reader from another build cannot disagree about which bit is which.
        constexpr uint32_t kFlagHasPattern = 1u << 0;
        constexpr uint32_t kFlagHasMask    = 1u << 1;

        /// A texel index that wraps for negative and oversized coordinates alike.
        ///
        /// `%` ALONE IS NOT ENOUGH and that is the whole reason this is a function: C's remainder keeps the
        /// sign of its left operand, so `-1 % 512` is `-1` and indexing with it walks off the front of the
        /// vector. The painting tiles the world, so a negative world coordinate is not an error — it is
        /// ordinary, and it is what every camera west or south of the origin produces.
        uint32_t WrapTexel( int32_t index, uint32_t resolution )
        {
            const int32_t side = static_cast<int32_t>( resolution );
            int32_t       m    = index % side;
            if ( m < 0 )
                m += side;
            return static_cast<uint32_t>( m );
        }

        /// Bilinear over a wrapping table, given a fetch for one texel. One implementation, two tables:
        /// a pattern channel and the mask filter identically, and writing the filter twice is how the two
        /// come to disagree about half a texel.
        template <typename FetchFn>
        float BilinearWrapped( const glm::vec2& uv, uint32_t resolution, FetchFn fetch )
        {
            // HALF A TEXEL, and it is not decoration. `uv * resolution` puts u = 0 on the CENTRE of texel 0
            // only if the half-texel is subtracted first; without it the painting is displaced by half a
            // texel against the world, which at one repeat over 48 km is 47 metres of drift between what
            // the artist painted and where the cloud lands.
            const float x = uv.x * static_cast<float>( resolution ) - 0.5f;
            const float y = uv.y * static_cast<float>( resolution ) - 0.5f;

            const float x0f = std::floor( x );
            const float y0f = std::floor( y );

            const float fx = x - x0f;
            const float fy = y - y0f;

            const uint32_t x0 = WrapTexel( static_cast<int32_t>( x0f ), resolution );
            const uint32_t y0 = WrapTexel( static_cast<int32_t>( y0f ), resolution );
            const uint32_t x1 = WrapTexel( static_cast<int32_t>( x0f ) + 1, resolution );
            const uint32_t y1 = WrapTexel( static_cast<int32_t>( y0f ) + 1, resolution );

            const float v00 = fetch( x0, y0 );
            const float v10 = fetch( x1, y0 );
            const float v01 = fetch( x0, y1 );
            const float v11 = fetch( x1, y1 );

            const float top    = v00 + ( v10 - v00 ) * fx;
            const float bottom = v01 + ( v11 - v01 ) * fx;
            return top + ( bottom - top ) * fy;
        }
    } // namespace

    bool CloudLayoutPlacementEqual( const CloudLayoutPlacement& a, const CloudLayoutPlacement& b )
    {
        // WRITTEN OUT FIELD BY FIELD rather than defaulted, and it is the same safe direction
        // CloudProceduralParamsEqual takes: a defaulted comparison silently starts comparing whatever
        // somebody adds, which sounds right until the added field is one the bake does not read — and then
        // every frame re-bakes. Comparing named fields means a new one has to be considered.
        return a.RepeatsPerRegion == b.RepeatsPerRegion && a.QuarterTurns == b.QuarterTurns &&
               a.OffsetKm == b.OffsetKm && a.PatternStrength == b.PatternStrength &&
               a.MaskStrength == b.MaskStrength;
    }

    Common::BoolResultStr ValidateCloudLayoutPlacement( const CloudLayoutPlacement& placement )
    {
        // THE FLOOR IS ONE AND THE CEILING IS THE LATTICE. Zero repeats is a division by zero dressed as a
        // setting; above sixteen the painting's period at the shipped 48 km region is finer than the 3 km
        // placement cell, so a texel can no longer decide a cell and the painting reads as noise on the
        // coverage rather than as a shape.
        if ( placement.RepeatsPerRegion < 1u || placement.RepeatsPerRegion > 16u )
            return Common::MakeFormattedError<bool>( "Layout Repeats must lie in [1, 16], got {}",
                                                     placement.RepeatsPerRegion );

        if ( placement.QuarterTurns > 3u )
            return Common::MakeFormattedError<bool>(
                 "Layout Rotation is a count of QUARTER TURNS and must lie in [0, 3], got {}; a free angle "
                 "would break the modelling volume's periodicity, which is what the far field is",
                 placement.QuarterTurns );

        if ( !std::isfinite( placement.OffsetKm.x ) || !std::isfinite( placement.OffsetKm.y ) )
            return Common::MakeFormattedError<bool>( "Layout Offset must be finite, got ({}, {})",
                                                     placement.OffsetKm.x, placement.OffsetKm.y );

        if ( !std::isfinite( placement.PatternStrength ) || placement.PatternStrength < 0.0f ||
             placement.PatternStrength > 1.0f )
            return Common::MakeFormattedError<bool>( "Layout Pattern Strength must lie in [0, 1], got {}",
                                                     placement.PatternStrength );

        if ( !std::isfinite( placement.MaskStrength ) || placement.MaskStrength < 0.0f ||
             placement.MaskStrength > 1.0f )
            return Common::MakeFormattedError<bool>( "Layout Mask Strength must lie in [0, 1], got {}",
                                                     placement.MaskStrength );

        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr ValidateCloudLayoutData( const CloudLayoutData& data )
    {
        if ( data.Resolution < kCloudLayoutMinResolution || data.Resolution > kCloudLayoutMaxResolution )
            return Common::MakeFormattedError<bool>( "layout resolution must lie in [{}, {}], got {}",
                                                     kCloudLayoutMinResolution, kCloudLayoutMaxResolution,
                                                     data.Resolution );

        // A LAYOUT THAT CARRIES NEITHER TABLE IS REFUSED rather than tolerated. It would occupy the slot,
        // pass every other check and change nothing at all — which is a dead setting reached from the far
        // side, and it is indistinguishable from the slot being empty except that the artist believes it is
        // not.
        if ( !data.HasPattern() && !data.HasMask() )
            return Common::MakeFormattedError<bool>(
                 "layout carries neither a pattern nor a mask, so binding it could not change one cell" );

        const uint64_t texels = static_cast<uint64_t>( data.Resolution ) * data.Resolution;

        if ( data.HasPattern() && data.Pattern.size() != texels * kPatternBytesPerTexel )
            return Common::MakeFormattedError<bool>( "pattern is {} bytes, expected {} for {}x{} RGBA8",
                                                     data.Pattern.size(), texels * kPatternBytesPerTexel,
                                                     data.Resolution, data.Resolution );

        if ( data.HasMask() && data.Mask.size() != texels )
            return Common::MakeFormattedError<bool>( "mask is {} bytes, expected {} for {}x{} R8",
                                                     data.Mask.size(), texels, data.Resolution, data.Resolution );

        for ( uint32_t slot = 0; slot < kCloudLayoutChannels; ++slot )
        {
            const float mean = data.PatternMean[slot];
            if ( !std::isfinite( mean ) || mean < 0.0f || mean > 1.0f )
                return Common::MakeFormattedError<bool>(
                     "pattern mean for slot {} is {}, which is not a fraction; the zero-mean application "
                     "that keeps the Coverage slider honest cannot be performed with it",
                     slot, mean );
        }

        return Common::MakeSuccess( true );
    }

    glm::vec2 CloudLayoutUv( const CloudLayoutPlacement& placement, float regionSizeKm, const glm::vec2& worldKm )
    {
        const float region  = std::max( regionSizeKm, 1e-3f );
        const float repeats = static_cast<float>( std::max( placement.RepeatsPerRegion, 1u ) );

        const glm::vec2 shifted = worldKm - placement.OffsetKm;

        // THE RESTRICTION IS ON THE ANGLE, NOT ON THE ARITHMETIC, and a sabotage is what settled which.
        //
        // The first version of this comment claimed that writing the quarter turn as a `cos`/`sin` matrix
        // would break the periodicity by a texel at large world coordinates. It does not: replacing the
        // swap below with a float `cos`/`sin` at 90 degrees was measured at 3.8e-6 of a period against the
        // exact form's 1.9e-6, on probes out to 12 345 km, and
        // CloudPlacementSpectrum.ThePaintingRepeatsExactlyWithTheRegionAtEveryRotationAndOffset stayed
        // GREEN. The claim was wrong and is corrected rather than deleted, because the decision it was
        // offered in support of is the right one for a different reason.
        //
        // WHAT ACTUALLY BREAKS IS ANY OTHER ANGLE. A square lattice maps onto itself under a quarter turn
        // and under nothing else, so the periodicity the far field depends on is a property of the SET of
        // permitted rotations. The same sabotage at 45 degrees moves the departure to 0.414 of a period —
        // five orders of magnitude — and the test goes red at once. That is why `QuarterTurns` is a count
        // and not an angle.
        //
        // The exact swap is kept anyway: it is cheaper than two transcendentals and it is exact at every
        // magnitude, so it removes a residue that is small rather than one that was large.
        glm::vec2 turned = shifted;
        switch ( placement.QuarterTurns & 3u )
        {
            case 1:
                turned = glm::vec2( -shifted.y, shifted.x );
                break;
            case 2:
                turned = glm::vec2( -shifted.x, -shifted.y );
                break;
            case 3:
                turned = glm::vec2( shifted.y, -shifted.x );
                break;
            default:
                break;
        }

        return turned * ( repeats / region );
    }

    float SampleCloudLayoutPattern( const CloudLayoutData& data, uint32_t slot, const glm::vec2& uv )
    {
        if ( !data.HasPattern() || slot >= kCloudLayoutChannels || data.Resolution == 0u )
            return 0.0f;

        const uint32_t             resolution = data.Resolution;
        const unsigned char* const pixels     = data.Pattern.data();

        return BilinearWrapped(
             uv, resolution,
             [pixels, resolution, slot]( uint32_t x, uint32_t y )
             {
                 const size_t index = ( static_cast<size_t>( y ) * resolution + x ) * kPatternBytesPerTexel + slot;
                 return static_cast<float>( pixels[index] ) * ( 1.0f / 255.0f );
             } );
    }

    float SampleCloudLayoutMask( const CloudLayoutData& data, const glm::vec2& uv )
    {
        if ( !data.HasMask() || data.Resolution == 0u )
            return 0.0f;

        const uint32_t             resolution = data.Resolution;
        const unsigned char* const pixels     = data.Mask.data();

        const float raw = BilinearWrapped(
             uv, resolution, [pixels, resolution]( uint32_t x, uint32_t y )
             { return static_cast<float>( pixels[static_cast<size_t>( y ) * resolution + x] ); } );

        // THE TWO SIDES OF NEUTRAL ARE NOT THE SAME WIDTH — 128 has 128 values below it and 127 above — so
        // the two halves are normalised separately. Dividing by one number instead would make a fully white
        // mask reach 0.992 while a fully black one reached -1.000, and "paint it white" would then be
        // measurably weaker than "paint it black" for no reason an artist could ever see.
        const float centred = raw - kMaskNeutral;
        return centred >= 0.0f ? centred / ( 255.0f - kMaskNeutral ) : centred / kMaskNeutral;
    }

    Common::ResultStr<std::vector<unsigned char>> EncodeCloudLayout( const CloudLayoutData& data )
    {
        CloudLayoutData written = data;

        // THE MEANS ARE RECOMPUTED HERE AND NOWHERE ELSE. This is the one point where they can be made to
        // agree with the pixels they describe; a mean that arrived from a caller could belong to a
        // different painting, and the symptom would be a sky whose cover drifts from its slider with
        // nothing in the file to say why.
        for ( uint32_t slot = 0; slot < kCloudLayoutChannels; ++slot )
            written.PatternMean[slot] = 0.0f;

        if ( written.HasPattern() )
        {
            const uint64_t texels = static_cast<uint64_t>( written.Resolution ) * written.Resolution;
            if ( texels != 0u && written.Pattern.size() == texels * kPatternBytesPerTexel )
            {
                // Summed in double. At 1024 squared a float accumulator has already lost the low bits of
                // the sum by the time it reaches a million terms, and the error lands directly in the
                // number that keeps the Coverage mapping honest.
                double sums[kCloudLayoutChannels] = { 0.0, 0.0, 0.0, 0.0 };
                for ( uint64_t t = 0; t < texels; ++t )
                    for ( uint32_t slot = 0; slot < kCloudLayoutChannels; ++slot )
                        sums[slot] += static_cast<double>(
                             written.Pattern[static_cast<size_t>( t ) * kPatternBytesPerTexel + slot] );

                for ( uint32_t slot = 0; slot < kCloudLayoutChannels; ++slot )
                    written.PatternMean[slot] =
                         static_cast<float>( sums[slot] / ( static_cast<double>( texels ) * 255.0 ) );
            }
        }

        if ( auto valid = ValidateCloudLayoutData( written ); !valid )
            return Common::MakeFormattedError<std::vector<unsigned char>>( "cannot encode layout: {}",
                                                                           valid.GetError() );

        std::vector<unsigned char> payload;
        payload.reserve( written.Pattern.size() + written.Mask.size() );
        payload.insert( payload.end(), written.Pattern.begin(), written.Pattern.end() );
        payload.insert( payload.end(), written.Mask.begin(), written.Mask.end() );

        uint32_t flags = 0u;
        if ( written.HasPattern() )
            flags |= kFlagHasPattern;
        if ( written.HasMask() )
            flags |= kFlagHasMask;

        std::vector<unsigned char> out;
        out.reserve( kCloudLayoutHeaderSize + payload.size() );

        WriteU32( out, written.Resolution );
        WriteU32( out, flags );

        for ( uint32_t slot = 0; slot < kCloudLayoutChannels; ++slot )
            WriteF32( out, written.PatternMean[slot] );

        WriteU64( out, static_cast<uint64_t>( payload.size() ) );
        WriteU32( out, Crc32( payload.data(), payload.size() ) );

        // Four bytes of nothing so the header is a round 40 and the pixels start on an eight-byte
        // boundary. Named rather than left implicit, because a reader that computed the offset from the
        // fields above would drift the moment one of them moved.
        WriteU32( out, 0u );

        out.insert( out.end(), payload.begin(), payload.end() );

        namespace CC = Common::Content;
        CC::AssetEnvelope envelope;
        envelope.Asset.Kind       = CC::ContentKind::CloudLayout;
        envelope.Asset.Guid       = written.Guid.IsNull() ? CC::AssetGuid::Generate() : written.Guid;
        envelope.Asset.Subsystems = { { kCloudLayoutSubsystemTag, kCloudLayoutContainerVersion } };
        const auto* first         = reinterpret_cast<const std::byte*>( out.data() );
        envelope.Sections.push_back(
             { CC::EnvelopeSection::Payload, CC::EnvelopeCodec::Stored, { first, first + out.size() } } );

        auto file = CC::WriteAssetEnvelope( envelope );
        if ( !file )
            return Common::MakeFormattedError<std::vector<unsigned char>>( "cannot wrap layout: {}",
                                                                           file.GetError() );
        const auto* begin = reinterpret_cast<const unsigned char*>( file.GetValue().data() );
        return Common::MakeSuccess( std::vector<unsigned char>( begin, begin + file.GetValue().size() ) );
    }

    Common::ResultStr<CloudLayoutData> DecodeCloudLayout( const std::vector<unsigned char>& file )
    {
        namespace CC = Common::Content;

        // A VERSION-1 FILE IS REFUSED BY NAME, never read: it has no GUID, so reading it would hand the
        // layout a path-derived handle nothing else agrees with. The migrator wraps it once, in the file.
        if ( file.size() >= sizeof( kCloudLayoutVersion1Magic ) &&
             std::memcmp( file.data(), kCloudLayoutVersion1Magic, sizeof( kCloudLayoutVersion1Magic ) ) == 0 )
        {
            const uint32_t oldVersion = file.size() >= 8u ? ReadU32( file.data() + 4 ) : 0u;
            return Common::MakeFormattedError<CloudLayoutData>(
                 "a bare 'DCLY' container version {} (no asset envelope, no GUID); this build reads version {} "
                 "inside the envelope - run Tools/SceneMigrator --write over the file",
                 oldVersion, kCloudLayoutContainerVersion );
        }

        const CC::SubsystemVersion kKnown[] = { { kCloudLayoutSubsystemTag, kCloudLayoutContainerVersion } };
        auto                       envelope = CC::ReadAssetEnvelope(
             std::span<const std::byte>( reinterpret_cast<const std::byte*>( file.data() ), file.size() ),
             CC::AssetHeaderReadContext{ kKnown } );
        if ( !envelope )
            return Common::MakeFormattedError<CloudLayoutData>( "not a cloud layout envelope: {}",
                                                                envelope.GetError() );
        const CC::AssetEnvelope& e = envelope.GetValue();
        if ( e.Asset.Kind != CC::ContentKind::CloudLayout )
            return Common::MakeFormattedError<CloudLayoutData>( "the envelope's kind is {}, not CloudLayout",
                                                                CC::KindName( e.Asset.Kind ) );
        if ( e.Asset.Subsystems.size() != 1u || e.Asset.Subsystems[0].Tag != kCloudLayoutSubsystemTag )
            return Common::MakeFormattedError<CloudLayoutData>(
                 "the envelope states {} subsystem versions; a layout states exactly one, under 'DCLY'",
                 e.Asset.Subsystems.size() );
        const auto section = std::find_if( e.Sections.begin(), e.Sections.end(),
                                           []( const auto& s ) { return s.Tag == CC::EnvelopeSection::Payload; } );
        if ( section == e.Sections.end() )
            return Common::MakeFormattedError<CloudLayoutData>(
                 "the envelope has no {} section",
                 CC::FourCCToString( static_cast<uint32_t>( CC::EnvelopeSection::Payload ) ) );

        const std::vector<unsigned char> bytes( reinterpret_cast<const unsigned char*>( section->Bytes.data() ),
                                                reinterpret_cast<const unsigned char*>( section->Bytes.data() ) +
                                                     section->Bytes.size() );
        if ( bytes.size() < kCloudLayoutHeaderSize )
            return Common::MakeFormattedError<CloudLayoutData>(
                 "payload is {} bytes, shorter than the {}-byte header", bytes.size(), kCloudLayoutHeaderSize );

        const unsigned char* at = bytes.data();

        CloudLayoutData data;
        data.Resolution = ReadU32( at );
        at += 4;

        const uint32_t flags = ReadU32( at );
        at += 4;

        for ( uint32_t slot = 0; slot < kCloudLayoutChannels; ++slot )
        {
            data.PatternMean[slot] = ReadF32( at );
            at += 4;
        }

        const uint64_t payloadBytes = ReadU64( at );
        at += 8;

        const uint32_t storedCrc = ReadU32( at );
        at += 4;

        if ( data.Resolution < kCloudLayoutMinResolution || data.Resolution > kCloudLayoutMaxResolution )
            return Common::MakeFormattedError<CloudLayoutData>( "layout resolution {} lies outside [{}, {}]",
                                                                data.Resolution, kCloudLayoutMinResolution,
                                                                kCloudLayoutMaxResolution );

        const uint64_t texels = static_cast<uint64_t>( data.Resolution ) * data.Resolution;

        const bool hasPattern = ( flags & kFlagHasPattern ) != 0u;
        const bool hasMask    = ( flags & kFlagHasMask ) != 0u;

        const uint64_t expected = ( hasPattern ? texels * kPatternBytesPerTexel : 0u ) + ( hasMask ? texels : 0u );

        if ( payloadBytes != expected )
            return Common::MakeFormattedError<CloudLayoutData>(
                 "header says {} payload bytes but {}x{} with pattern={} mask={} needs {}", payloadBytes,
                 data.Resolution, data.Resolution, hasPattern, hasMask, expected );

        if ( bytes.size() != kCloudLayoutHeaderSize + payloadBytes )
            return Common::MakeFormattedError<CloudLayoutData>( "payload is {} bytes, expected {} for its header",
                                                                bytes.size(),
                                                                kCloudLayoutHeaderSize + payloadBytes );

        const unsigned char* payload = bytes.data() + kCloudLayoutHeaderSize;

        const uint32_t actualCrc = Crc32( payload, static_cast<size_t>( payloadBytes ) );
        if ( actualCrc != storedCrc )
            return Common::MakeFormattedError<CloudLayoutData>(
                 "payload checksum is {:08x} but the header says {:08x}; the file is corrupt, and a corrupt "
                 "layout renders as a sky with the wrong clouds in it rather than as an error",
                 actualCrc, storedCrc );

        if ( hasPattern )
        {
            const size_t count = static_cast<size_t>( texels * kPatternBytesPerTexel );
            data.Pattern.assign( payload, payload + count );
            payload += count;
        }

        if ( hasMask )
            data.Mask.assign( payload, payload + static_cast<size_t>( texels ) );

        data.ContentHash = storedCrc;
        data.Guid        = e.Asset.Guid;

        if ( auto valid = ValidateCloudLayoutData( data ); !valid )
            return Common::MakeFormattedError<CloudLayoutData>( "layout decoded but is unusable: {}",
                                                                valid.GetError() );

        return Common::MakeSuccess( std::move( data ) );
    }

    CloudLayoutStrokeStats MeasureCloudLayoutStrokes( const CloudLayoutData& data, uint32_t slot,
                                                      float limitTexels )
    {
        CloudLayoutStrokeStats stats;

        if ( !data.HasPattern() || slot >= kCloudLayoutChannels || data.Resolution == 0u )
            return stats;

        const uint32_t n      = data.Resolution;
        const size_t   texels = static_cast<size_t>( n ) * n;

        if ( data.Pattern.size() != texels * kPatternBytesPerTexel )
            return stats;

        // THE THRESHOLD IS THE CHANNEL'S OWN MIDPOINT, not a fixed half. A painting that runs from 40 to
        // 200 was still drawn with strokes, and thresholding it at 128 would measure the ink rather than
        // the shape — a soft figure would report half its strokes and a dark one none at all.
        unsigned char least    = 255u;
        unsigned char greatest = 0u;
        for ( size_t t = 0; t < texels; ++t )
        {
            const unsigned char v = data.Pattern[t * kPatternBytesPerTexel + slot];
            least                 = std::min( least, v );
            greatest              = std::max( greatest, v );
        }

        // A FLAT CHANNEL HAS NO STROKES AT ALL, and that is the honest answer rather than "every texel is
        // painted". A slot nobody drew on must not produce a legibility verdict.
        if ( greatest - least < 2u )
            return stats;

        const unsigned char threshold = static_cast<unsigned char>(
             ( static_cast<uint32_t>( least ) + static_cast<uint32_t>( greatest ) ) / 2u );

        const auto painted = [&]( uint32_t x, uint32_t y )
        { return data.Pattern[( static_cast<size_t>( y ) * n + x ) * kPatternBytesPerTexel + slot] > threshold; };

        // uint16 because the side is capped at kCloudLayoutMaxResolution — 1024 fits, and at the ceiling
        // the two tables are 4 MiB rather than 8.
        std::vector<uint16_t> runAlongX( texels, 0u );
        std::vector<uint16_t> runAlongY( texels, 0u );

        // ONE WALK, USED FOR BOTH AXES, parameterised by a fetch and a store. Writing the wrapping run
        // twice is how the two axes come to disagree about where a stroke ends.
        const auto walkLine = [&]( uint32_t line, bool alongX, std::vector<uint16_t>& into )
        {
            const auto at = [&]( uint32_t k ) -> size_t
            { return alongX ? static_cast<size_t>( line ) * n + k : static_cast<size_t>( k ) * n + line; };
            const auto on = [&]( uint32_t k ) { return alongX ? painted( k, line ) : painted( line, k ); };

            // THE WALK STARTS AT AN UNPAINTED TEXEL so that every run is met whole and written ONCE.
            //
            // The claim this comment used to make — that starting at 0 unconditionally would cut a
            // straddling run in two and halve its measured width — is FALSE, and a sabotage settled it:
            // forced to start at 0, the suite's straddling bar still measured 8 texels
            // (CloudPlacementSpectrum.TheStrokeMeasureJoinsABarThatStraddlesThePaintingsEdge stayed
            // GREEN). The wrapping run is walked LAST, and its write overwrites the half-run written at
            // index 0. What actually breaks the wrap is dropping the modulo, which splits the bar into two
            // runs nothing repairs; the same test goes red at once. Corrected rather than deleted, because
            // the search is still worth its four lines — it keeps "each texel is written once" an
            // invariant of the walk rather than a consequence of the order two writes happen to arrive in.
            uint32_t start = n;
            for ( uint32_t k = 0; k < n; ++k )
                if ( !on( k ) )
                {
                    start = k;
                    break;
                }

            if ( start == n )
            {
                // The whole line is painted: one stroke as wide as the world, because it wraps.
                for ( uint32_t k = 0; k < n; ++k )
                    into[at( k )] = static_cast<uint16_t>( n );
                return;
            }

            uint32_t k = 0;
            while ( k < n )
            {
                const uint32_t index = ( start + k ) % n;
                if ( !on( index ) )
                {
                    ++k;
                    continue;
                }

                uint32_t length = 0;
                while ( length < n && on( ( start + k + length ) % n ) )
                    ++length;

                for ( uint32_t i = 0; i < length; ++i )
                    into[at( ( start + k + i ) % n )] = static_cast<uint16_t>( length );

                k += length;
            }
        };

        for ( uint32_t y = 0; y < n; ++y )
            walkLine( y, /*alongX=*/true, runAlongX );
        for ( uint32_t x = 0; x < n; ++x )
            walkLine( x, /*alongX=*/false, runAlongY );

        // Histogram over widths 1..n, so the percentiles below are exact counts rather than a sort of a
        // million floats.
        std::vector<uint64_t> histogram( static_cast<size_t>( n ) + 1u, 0u );
        uint64_t              belowLimit = 0u;

        for ( size_t t = 0; t < texels; ++t )
        {
            const uint16_t width = std::min( runAlongX[t], runAlongY[t] );
            if ( width == 0u )
                continue;

            ++stats.PaintedTexels;
            ++histogram[width];

            if ( limitTexels > 0.0f && static_cast<float>( width ) < limitTexels )
                ++belowLimit;
        }

        if ( stats.PaintedTexels == 0u )
            return stats;

        const auto percentile = [&]( double fraction )
        {
            const uint64_t wanted = static_cast<uint64_t>(
                 std::max( 1.0, std::ceil( fraction * static_cast<double>( stats.PaintedTexels ) ) ) );
            uint64_t seen = 0u;
            for ( uint32_t width = 1; width <= n; ++width )
            {
                seen += histogram[width];
                if ( seen >= wanted )
                    return static_cast<float>( width );
            }
            return static_cast<float>( n );
        };

        stats.ThinnestTenthTexels = percentile( 0.10 );
        stats.MedianTexels        = percentile( 0.50 );
        stats.FractionBelowLimit =
             static_cast<float>( static_cast<double>( belowLimit ) / static_cast<double>( stats.PaintedTexels ) );

        return stats;
    }

    // -----------------------------------------------------------------------------------------------------
    // The brush
    // -----------------------------------------------------------------------------------------------------

    namespace
    {
        /// The dab's radial profile, 0..1. Flat out to `hardness * radius`, then a straight ramp to nothing
        /// at `radius`. Linear rather than a smoothstep on purpose: the width the stroke measures at is
        /// wanted in closed form (CloudLayoutBrushWidthTexels), and a curve whose half-value point needs a
        /// root-finder is a relation nobody can state.
        float BrushFalloff( float distance, float radius, float hardness )
        {
            if ( distance >= radius )
                return 0.0f;

            const float core = hardness * radius;
            if ( distance <= core )
                return 1.0f;

            // radius - core > 0 here: distance < radius and distance > core together forbid core == radius.
            return ( radius - distance ) / ( radius - core );
        }

        /// Distance from a point to a segment, with the degenerate segment (a click that never moved)
        /// falling out as the distance to the point rather than as a division by zero.
        float DistanceToSegment( const glm::vec2& p, const glm::vec2& a, const glm::vec2& b )
        {
            const glm::vec2 ab       = b - a;
            const float     lengthSq = ab.x * ab.x + ab.y * ab.y;

            float t = 0.0f;
            if ( lengthSq > 1e-12f )
            {
                t = ( ( p.x - a.x ) * ab.x + ( p.y - a.y ) * ab.y ) / lengthSq;
                t = std::clamp( t, 0.0f, 1.0f );
            }

            const glm::vec2 closest = a + ab * t;
            const float     dx      = p.x - closest.x;
            const float     dy      = p.y - closest.y;
            return std::sqrt( dx * dx + dy * dy );
        }

        Common::BoolResultStr CheckPlane( const std::vector<unsigned char>& plane, uint32_t side, uint32_t channel,
                                          uint32_t channelCount )
        {
            if ( side < kCloudLayoutMinResolution || side > kCloudLayoutMaxResolution )
                return Common::MakeFormattedError<bool>( "canvas side {} lies outside [{}, {}]", side,
                                                         kCloudLayoutMinResolution, kCloudLayoutMaxResolution );

            if ( channelCount == 0u )
                return Common::MakeError<bool>( "a plane with no channels cannot be painted" );

            const size_t wanted = static_cast<size_t>( side ) * side * channelCount;
            if ( plane.size() != wanted )
                return Common::MakeFormattedError<bool>(
                     "plane is {} bytes, expected {} for {}x{} with {} channels", plane.size(), wanted, side, side,
                     channelCount );

            if ( channel >= channelCount )
                return Common::MakeFormattedError<bool>( "channel {} does not exist; this plane has {}", channel,
                                                         channelCount );

            return Common::MakeSuccess( true );
        }

        /// The side and shape checks every picture arriving from outside has to pass, whichever table it is
        /// destined for. One statement of them, so a pattern and a mask cannot come to be judged by
        /// different rules and an artist cannot be told two different things about the same file.
        Common::BoolResultStr CheckSourceImage( const std::vector<unsigned char>& pixels, uint32_t width,
                                                uint32_t height )
        {
            if ( width != height )
                return Common::MakeFormattedError<bool>(
                     "layout source is {}x{}; it must be square, because the painting tiles the world on a "
                     "square period and resampling it would be an opinion about the artist's image",
                     width, height );

            if ( width < kCloudLayoutMinResolution || width > kCloudLayoutMaxResolution )
                return Common::MakeFormattedError<bool>( "layout source is {}x{}; the side must lie in [{}, {}]",
                                                         width, height, kCloudLayoutMinResolution,
                                                         kCloudLayoutMaxResolution );

            const uint64_t texels = static_cast<uint64_t>( width ) * height;
            if ( pixels.size() != texels * kPatternBytesPerTexel )
                return Common::MakeFormattedError<bool>( "layout source is {} bytes, expected {} for {}x{} RGBA8",
                                                         pixels.size(), texels * kPatternBytesPerTexel, width,
                                                         height );

            return Common::MakeSuccess( true );
        }
    } // namespace

    Common::ResultStr<CloudLayoutCanvas> MakeCloudLayoutCanvas( uint32_t side )
    {
        if ( side < kCloudLayoutMinResolution || side > kCloudLayoutMaxResolution )
            return Common::MakeFormattedError<CloudLayoutCanvas>( "canvas side {} lies outside [{}, {}]", side,
                                                                  kCloudLayoutMinResolution,
                                                                  kCloudLayoutMaxResolution );

        CloudLayoutCanvas canvas;
        canvas.Side = side;
        canvas.Pattern.assign( static_cast<size_t>( side ) * side * kPatternBytesPerTexel, 0u );

        return Common::MakeSuccess( std::move( canvas ) );
    }

    Common::ResultStr<CloudLayoutCanvas> MakeCloudLayoutCanvasFromLayout( const CloudLayoutData& data )
    {
        if ( auto valid = ValidateCloudLayoutData( data ); !valid )
            return Common::MakeFormattedError<CloudLayoutCanvas>( "this painting cannot be opened for "
                                                                  "painting: {}",
                                                                  valid.GetError() );

        // BOTH TABLES SURVIVE THE TRIP, which they did not before O-4. The mask has its own plane now, so a
        // painting that uses all four species slots AND adds and removes cloud opens exactly as it was
        // saved. There is nothing left for this function to refuse beyond a layout that was never valid.
        //
        // AND AN ABSENT TABLE STAYS ABSENT. Handing back a blank canvas and filling in what the layout has
        // would give a mask-only painting a flat pattern table it never carried, and saving it again would
        // write that table to disk — a file that changed because somebody opened it, which is the silent
        // kind of change §1.4 forbids. The canvas is a copy of the layout and of nothing else.
        CloudLayoutCanvas canvas;
        canvas.Side    = data.Resolution;
        canvas.Pattern = data.Pattern;
        canvas.Mask    = data.Mask;

        return Common::MakeSuccess( std::move( canvas ) );
    }

    Common::ResultStr<CloudLayoutData> MakeCloudLayoutFromCanvas( const CloudLayoutCanvas& canvas )
    {
        const size_t texels = static_cast<size_t>( canvas.Side ) * canvas.Side;

        if ( canvas.Side < kCloudLayoutMinResolution || canvas.Side > kCloudLayoutMaxResolution )
            return Common::MakeFormattedError<CloudLayoutData>( "canvas side {} lies outside [{}, {}]",
                                                                canvas.Side, kCloudLayoutMinResolution,
                                                                kCloudLayoutMaxResolution );

        // AN EMPTY PATTERN IS A STATE AND NOT A FAULT — a painting that only adds and removes cloud,
        // Unreal's mask texture with its pattern slot left alone. Both tables empty is refused, but by
        // ValidateCloudLayoutData inside the encoder, which is where that rule already lives.
        if ( !canvas.Pattern.empty() && canvas.Pattern.size() != texels * kPatternBytesPerTexel )
            return Common::MakeFormattedError<CloudLayoutData>(
                 "canvas pattern is {} bytes, expected {} for {}x{} RGBA8", canvas.Pattern.size(),
                 texels * kPatternBytesPerTexel, canvas.Side, canvas.Side );

        if ( canvas.HasMask() && canvas.Mask.size() != texels )
            return Common::MakeFormattedError<CloudLayoutData>( "canvas mask is {} bytes, expected {} for {}x{}",
                                                                canvas.Mask.size(), texels, canvas.Side,
                                                                canvas.Side );

        CloudLayoutData data;
        data.Resolution = canvas.Side;
        data.Pattern    = canvas.Pattern;
        data.Mask       = canvas.Mask;

        // Round-tripped rather than returned raw, so that the means and the content hash a caller receives
        // are the ones the FILE will carry. Two paths to a CloudLayoutData — one through the encoder and
        // one around it — is how the mean in memory comes to differ from the mean on disk.
        auto encoded = EncodeCloudLayout( data );
        if ( !encoded )
            return Common::MakeFormattedError<CloudLayoutData>( "layout built from the canvas is unusable: {}",
                                                                encoded.GetError() );

        return DecodeCloudLayout( encoded.GetValue() );
    }

    Common::BoolResultStr SetCloudLayoutCanvasMask( CloudLayoutCanvas& canvas, bool present )
    {
        if ( !present )
        {
            canvas.Mask.clear();
            return Common::MakeSuccess( true );
        }

        if ( canvas.Side < kCloudLayoutMinResolution || canvas.Side > kCloudLayoutMaxResolution )
            return Common::MakeFormattedError<bool>( "canvas side {} lies outside [{}, {}]", canvas.Side,
                                                     kCloudLayoutMinResolution, kCloudLayoutMaxResolution );

        const size_t texels = static_cast<size_t>( canvas.Side ) * canvas.Side;

        // ALREADY PRESENT MEANS KEEP WHAT IS PAINTED. Re-ticking a box that was already ticked must not be
        // an erase, and a `resize` on a mask of the right length is a no-op — which is exactly the
        // behaviour wanted, with no branch to get the wrong way round.
        canvas.Mask.resize( texels, kCloudLayoutMaskNeutral );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr
    SetCloudLayoutCanvasPatternFromImage( CloudLayoutCanvas& canvas, const std::vector<unsigned char>& pixels,
                                          uint32_t width, uint32_t height,
                                          const uint32_t channelForSlot[kCloudLayoutChannels] )
    {
        if ( auto valid = CheckSourceImage( pixels, width, height ); !valid )
            return valid;

        for ( uint32_t slot = 0; slot < kCloudLayoutChannels; ++slot )
            if ( channelForSlot[slot] >= kPatternBytesPerTexel )
                return Common::MakeFormattedError<bool>(
                     "slot {} is mapped to source channel {}, and an RGBA image has four", slot,
                     channelForSlot[slot] );

        // THE TWO TABLES OF ONE LAYOUT SHARE A RESOLUTION, and this is where that relation is kept. The
        // file format states it once (CloudLayoutData::Resolution, one number for both), so a pattern
        // arriving at a different side than the mask already on the canvas has to be refused rather than
        // resized: resizing either is an opinion about somebody's painting, and the sky it produced would
        // not be the sky either picture describes.
        if ( canvas.HasMask() && width != canvas.Side )
            return Common::MakeFormattedError<bool>(
                 "this pattern is {}x{} and the mask already on this canvas is {}x{}; both tables of one "
                 "layout share a resolution. Import a {}x{} pattern, or remove the mask first",
                 width, height, canvas.Side, canvas.Side, canvas.Side, canvas.Side );

        const size_t texels = static_cast<size_t>( width ) * height;

        canvas.Side = width;
        canvas.Pattern.resize( texels * kPatternBytesPerTexel );
        for ( size_t t = 0; t < texels; ++t )
            for ( uint32_t slot = 0; slot < kCloudLayoutChannels; ++slot )
                canvas.Pattern[t * kPatternBytesPerTexel + slot] =
                     pixels[t * kPatternBytesPerTexel + channelForSlot[slot]];

        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr SetCloudLayoutCanvasMaskFromImage( CloudLayoutCanvas&                canvas,
                                                             const std::vector<unsigned char>& pixels,
                                                             uint32_t width, uint32_t height,
                                                             uint32_t sourceChannel )
    {
        if ( auto valid = CheckSourceImage( pixels, width, height ); !valid )
            return valid;

        if ( sourceChannel >= kPatternBytesPerTexel )
            return Common::MakeFormattedError<bool>(
                 "the mask is read from source channel {}, and an RGBA image has four", sourceChannel );

        // The same relation as above, from the other side. A canvas with no pattern yet takes the mask's
        // own side, so neither order of the two imports is privileged — and NO pattern table is invented
        // for it: a mask picture on its own makes a mask-only painting, which is a legal `.dclayout` and
        // exactly what Unreal's mask texture is with the pattern slot left alone.
        const bool hasPattern = canvas.Side > 0u && !canvas.Pattern.empty();
        if ( hasPattern && width != canvas.Side )
            return Common::MakeFormattedError<bool>(
                 "this mask is {}x{} and the pattern already on this canvas is {}x{}; both tables of one "
                 "layout share a resolution. Import a {}x{} mask, or replace the pattern first",
                 width, height, canvas.Side, canvas.Side, canvas.Side, canvas.Side );

        const size_t texels = static_cast<size_t>( width ) * height;

        canvas.Side = width;
        canvas.Mask.resize( texels );
        for ( size_t t = 0; t < texels; ++t )
            canvas.Mask[t] = pixels[t * kPatternBytesPerTexel + sourceChannel];

        return Common::MakeSuccess( true );
    }

    Common::ResultStr<CloudLayoutImage> EncodeCloudLayoutCanvasPatternToImage( const CloudLayoutCanvas& canvas )
    {
        const size_t texels = static_cast<size_t>( canvas.Side ) * canvas.Side;

        if ( canvas.Side == 0u || canvas.Pattern.size() != texels * kPatternBytesPerTexel )
            return Common::MakeFormattedError<CloudLayoutImage>(
                 "canvas pattern is {} bytes at side {}, which is not a picture", canvas.Pattern.size(),
                 canvas.Side );

        CloudLayoutImage image;
        image.Side   = canvas.Side;
        image.Pixels = canvas.Pattern;
        return Common::MakeSuccess( std::move( image ) );
    }

    Common::ResultStr<CloudLayoutImage> EncodeCloudLayoutCanvasMaskToImage( const CloudLayoutCanvas& canvas )
    {
        const size_t texels = static_cast<size_t>( canvas.Side ) * canvas.Side;

        if ( canvas.Side == 0u || !canvas.HasMask() )
            return Common::MakeError<CloudLayoutImage>(
                 "this painting carries no add/remove mask, and writing a neutral one would claim a table it "
                 "does not have" );

        if ( canvas.Mask.size() != texels )
            return Common::MakeFormattedError<CloudLayoutImage>( "canvas mask is {} bytes at side {}, expected {}",
                                                                 canvas.Mask.size(), canvas.Side, texels );

        CloudLayoutImage image;
        image.Side = canvas.Side;
        image.Pixels.resize( texels * kPatternBytesPerTexel );

        // GREY IN R, G AND B AND AN OPAQUE ALPHA. The mask is one plane, but it leaves as an RGBA picture
        // so that it opens as the grey it was painted in rather than as a red channel, and so that no tool
        // reads the part of it that REMOVES cloud — the dark half — as transparency and composites it away.
        for ( size_t t = 0; t < texels; ++t )
        {
            const unsigned char value                   = canvas.Mask[t];
            image.Pixels[t * kPatternBytesPerTexel + 0] = value;
            image.Pixels[t * kPatternBytesPerTexel + 1] = value;
            image.Pixels[t * kPatternBytesPerTexel + 2] = value;
            image.Pixels[t * kPatternBytesPerTexel + 3] = 255u;
        }

        return Common::MakeSuccess( std::move( image ) );
    }

    float CloudLayoutBrushWidthTexels( const CloudLayoutBrush& brush )
    {
        const float radius   = std::max( brush.RadiusTexels, 0.0f );
        const float hardness = std::clamp( brush.Hardness, 0.0f, 1.0f );
        return radius * ( 1.0f + hardness );
    }

    Common::BoolResultStr BeginCloudLayoutStroke( CloudLayoutStroke&                stroke,
                                                  const std::vector<unsigned char>& plane, uint32_t side,
                                                  uint32_t channel, uint32_t channelCount )
    {
        if ( auto valid = CheckPlane( plane, side, channel, channelCount ); !valid )
            return valid;

        const size_t texels = static_cast<size_t>( side ) * side;

        stroke.Side         = side;
        stroke.Channel      = channel;
        stroke.ChannelCount = channelCount;
        stroke.Base.resize( texels );
        stroke.Coverage.assign( texels, 0u );

        for ( size_t t = 0; t < texels; ++t )
            stroke.Base[t] = plane[t * channelCount + channel];

        return Common::MakeSuccess( true );
    }

    uint64_t ExtendCloudLayoutStroke( CloudLayoutStroke& stroke, std::vector<unsigned char>& plane,
                                      const glm::vec2& fromTexels, const glm::vec2& toTexels,
                                      const CloudLayoutBrush& brush )
    {
        if ( !stroke.IsOpen() )
            return 0u;

        const uint32_t side   = stroke.Side;
        const uint32_t stride = stroke.ChannelCount;
        const size_t   texels = static_cast<size_t>( side ) * side;
        if ( plane.size() != texels * stride || stroke.Coverage.size() != texels )
            return 0u;

        const float radius = std::max( brush.RadiusTexels, 0.0f );
        if ( radius <= 0.0f )
            return 0u;

        const float hardness = std::clamp( brush.Hardness, 0.0f, 1.0f );
        const float inkByte  = std::clamp( brush.Ink, 0.0f, 1.0f ) * 255.0f;

        // THE BOX IS THE SEGMENT INFLATED BY THE RADIUS, then wrapped. Walking the whole canvas would be a
        // million distance evaluations for a stroke that touches a thousand texels, on every mouse move.
        const float minXf = std::min( fromTexels.x, toTexels.x ) - radius;
        const float maxXf = std::max( fromTexels.x, toTexels.x ) + radius;
        const float minYf = std::min( fromTexels.y, toTexels.y ) - radius;
        const float maxYf = std::max( fromTexels.y, toTexels.y ) + radius;

        int32_t x0 = static_cast<int32_t>( std::floor( minXf ) );
        int32_t x1 = static_cast<int32_t>( std::ceil( maxXf ) );
        int32_t y0 = static_cast<int32_t>( std::floor( minYf ) );
        int32_t y1 = static_cast<int32_t>( std::ceil( maxYf ) );

        // A footprint wider than the canvas would visit some texels twice. Harmless under the max below,
        // but pointless, so it is clipped to exactly one period.
        if ( x1 - x0 + 1 > static_cast<int32_t>( side ) )
        {
            x0 = 0;
            x1 = static_cast<int32_t>( side ) - 1;
        }
        if ( y1 - y0 + 1 > static_cast<int32_t>( side ) )
        {
            y0 = 0;
            y1 = static_cast<int32_t>( side ) - 1;
        }

        uint64_t changed = 0u;

        for ( int32_t iy = y0; iy <= y1; ++iy )
        {
            const uint32_t wy = WrapTexel( iy, side );

            for ( int32_t ix = x0; ix <= x1; ++ix )
            {
                const glm::vec2 centre( static_cast<float>( ix ) + 0.5f, static_cast<float>( iy ) + 0.5f );
                const float     alpha =
                     BrushFalloff( DistanceToSegment( centre, fromTexels, toTexels ), radius, hardness );
                if ( alpha <= 0.0f )
                    continue;

                const unsigned char coverage = static_cast<unsigned char>( std::lround( alpha * 255.0f ) );
                if ( coverage == 0u )
                    continue;

                const uint32_t wx    = WrapTexel( ix, side );
                const size_t   texel = static_cast<size_t>( wy ) * side + wx;

                // LAID ONCE, AT ITS STRONGEST. A dab weaker than what this drag has already put here must
                // not darken it back down, or dragging back over a stroke's soft rim would carve a groove
                // through the middle of it.
                if ( coverage <= stroke.Coverage[texel] )
                    continue;
                stroke.Coverage[texel] = coverage;

                const float         base  = static_cast<float>( stroke.Base[texel] );
                const unsigned char value = static_cast<unsigned char>(
                     std::lround( base + ( inkByte - base ) * ( static_cast<float>( coverage ) / 255.0f ) ) );

                unsigned char& target = plane[texel * stride + stroke.Channel];
                if ( target != value )
                {
                    target = value;
                    ++changed;
                }
            }
        }

        return changed;
    }

    Common::BoolResultStr PaintCloudLayoutPolyline( std::vector<unsigned char>& plane, uint32_t side,
                                                    uint32_t channel, uint32_t channelCount,
                                                    const std::vector<glm::vec2>& pointsTexels,
                                                    const CloudLayoutBrush&       brush )
    {
        if ( pointsTexels.size() < 2u )
            return Common::MakeFormattedError<bool>( "a stroke needs at least two points, got {}",
                                                     pointsTexels.size() );

        CloudLayoutStroke stroke;
        if ( auto opened = BeginCloudLayoutStroke( stroke, plane, side, channel, channelCount ); !opened )
            return opened;

        for ( size_t i = 1; i < pointsTexels.size(); ++i )
            ExtendCloudLayoutStroke( stroke, plane, pointsTexels[i - 1u], pointsTexels[i], brush );

        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr FillCloudLayoutPlaneChannel( std::vector<unsigned char>& plane, uint32_t side,
                                                       uint32_t channel, uint32_t channelCount,
                                                       unsigned char value )
    {
        if ( auto valid = CheckPlane( plane, side, channel, channelCount ); !valid )
            return valid;

        for ( size_t t = channel; t < plane.size(); t += channelCount )
            plane[t] = value;

        return Common::MakeSuccess( true );
    }
} // namespace Desert::Assets
