#include <Engine/Physics/PhysicsAssetFormat.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <istream>
#include <span>

namespace Desert::Physics
{
    namespace
    {
        namespace CC = Common::Content;

        class Writer
        {
        public:
            std::vector<unsigned char> Bytes;

            void U8( uint8_t v )
            {
                Bytes.push_back( v );
            }
            void U32( uint32_t v )
            {
                for ( int i = 0; i < 4; ++i )
                    Bytes.push_back( static_cast<unsigned char>( v >> ( 8 * i ) ) );
            }
            void U64( uint64_t v )
            {
                for ( int i = 0; i < 8; ++i )
                    Bytes.push_back( static_cast<unsigned char>( v >> ( 8 * i ) ) );
            }
            void F32( float v )
            {
                U32( std::bit_cast<uint32_t>( v ) );
            }
            void V3( const glm::vec3& v )
            {
                F32( v.x ), F32( v.y ), F32( v.z );
            }
            void Q( const glm::quat& q )
            {
                F32( q.x ), F32( q.y ), F32( q.z ), F32( q.w );
            }
            void Str( const std::string& s )
            {
                U32( static_cast<uint32_t>( s.size() ) );
                Bytes.insert( Bytes.end(), s.begin(), s.end() );
            }
        };

        class Reader
        {
        public:
            explicit Reader( const std::vector<unsigned char>& bytes ) : m_Bytes( bytes )
            {
            }

            [[nodiscard]] bool Failed() const
            {
                return m_Failed;
            }
            [[nodiscard]] size_t Remaining() const
            {
                return m_Bytes.size() - m_At;
            }

            uint64_t Raw( int n )
            {
                if ( m_Failed || Remaining() < static_cast<size_t>( n ) )
                {
                    m_Failed = true;
                    return 0;
                }
                uint64_t v = 0;
                for ( int i = 0; i < n; ++i )
                    v |= static_cast<uint64_t>( m_Bytes[m_At + static_cast<size_t>( i )] ) << ( 8 * i );
                m_At += static_cast<size_t>( n );
                return v;
            }
            uint8_t U8()
            {
                return static_cast<uint8_t>( Raw( 1 ) );
            }
            uint32_t U32()
            {
                return static_cast<uint32_t>( Raw( 4 ) );
            }
            uint64_t U64()
            {
                return Raw( 8 );
            }
            float F32()
            {
                return std::bit_cast<float>( U32() );
            }
            glm::vec3 V3()
            {
                const float x = F32();
                const float y = F32();
                const float z = F32();
                return { x, y, z };
            }
            glm::quat Q()
            {
                const float x = F32();
                const float y = F32();
                const float z = F32();
                const float w = F32();
                return glm::quat( w, x, y, z );
            }
            std::string Str()
            {
                const uint32_t n = U32();
                if ( m_Failed || Remaining() < n )
                {
                    m_Failed = true;
                    return {};
                }
                std::string s( reinterpret_cast<const char*>( m_Bytes.data() + m_At ), n );
                m_At += n;
                return s;
            }
            /// A count of records of at least @p minRecordBytes each, refused when the payload cannot hold them.
            uint32_t Count( size_t minRecordBytes )
            {
                const uint32_t n = U32();
                if ( static_cast<uint64_t>( n ) * minRecordBytes > Remaining() )
                    m_Failed = true;
                return m_Failed ? 0u : n;
            }

        private:
            const std::vector<unsigned char>& m_Bytes;
            size_t                            m_At     = 0;
            bool                              m_Failed = false;
        };

        // The smallest record each count can stand for: a body with an empty bone name, a constraint with two.
        constexpr size_t kMinBodyBytes       = 4 + 1 + 12 + 16 + 4 + 4 + 12 + 4 + 4;
        constexpr size_t kMinConstraintBytes = 4 + 4 + 12 + 16 + 4 + 4 + 4;
    } // namespace

    const char* PhysicsBodyShapeName( PhysicsBodyShape shape )
    {
        switch ( shape )
        {
            case PhysicsBodyShape::Sphere:
                return "Sphere";
            case PhysicsBodyShape::Box:
                return "Box";
            case PhysicsBodyShape::Capsule:
                return "Capsule";
        }
        return "Unknown";
    }

    std::vector<unsigned char> EncodePhysicsAssetPayload( const PhysicsAssetData& data )
    {
        Writer w;
        w.U64( data.Skeleton.Hi );
        w.U64( data.Skeleton.Lo );
        w.U32( static_cast<uint32_t>( data.Bodies.size() ) );
        for ( const PhysicsAssetBody& b : data.Bodies )
        {
            w.Str( b.Bone );
            w.U8( static_cast<uint8_t>( b.Shape ) );
            w.V3( b.Center );
            w.Q( b.Rotation );
            w.F32( b.Radius );
            w.F32( b.Length );
            w.V3( b.BoxExtents );
            w.F32( b.MassKg );
            w.F32( b.DensityGramsPerCm3 );
        }
        w.U32( static_cast<uint32_t>( data.Constraints.size() ) );
        for ( const PhysicsAssetConstraint& c : data.Constraints )
        {
            w.Str( c.ParentBone );
            w.Str( c.ChildBone );
            w.V3( c.Position );
            w.Q( c.Rotation );
            w.F32( c.Swing1LimitDegrees );
            w.F32( c.Swing2LimitDegrees );
            w.F32( c.TwistLimitDegrees );
        }
        return std::move( w.Bytes );
    }

    Common::ResultStr<PhysicsAssetData> DecodePhysicsAssetPayload( const std::vector<unsigned char>& bytes )
    {
        Reader           r( bytes );
        PhysicsAssetData d;
        d.Skeleton.Hi = r.U64();
        d.Skeleton.Lo = r.U64();

        const uint32_t bodyCount = r.Count( kMinBodyBytes );
        d.Bodies.resize( bodyCount );
        for ( uint32_t i = 0; i < bodyCount && !r.Failed(); ++i )
        {
            PhysicsAssetBody& b = d.Bodies[i];
            b.Bone              = r.Str();
            const uint8_t shape = r.U8();
            if ( shape > static_cast<uint8_t>( PhysicsBodyShape::Capsule ) )
                return Common::MakeFormattedError<PhysicsAssetData>(
                     "body {} ('{}') states shape {}; a physics asset body is a sphere (0), box (1) or capsule "
                     "(2)",
                     i, b.Bone, static_cast<unsigned>( shape ) );
            b.Shape              = static_cast<PhysicsBodyShape>( shape );
            b.Center             = r.V3();
            b.Rotation           = r.Q();
            b.Radius             = r.F32();
            b.Length             = r.F32();
            b.BoxExtents         = r.V3();
            b.MassKg             = r.F32();
            b.DensityGramsPerCm3 = r.F32();
        }

        const uint32_t constraintCount = r.Failed() ? 0u : r.Count( kMinConstraintBytes );
        d.Constraints.resize( constraintCount );
        for ( uint32_t i = 0; i < constraintCount && !r.Failed(); ++i )
        {
            PhysicsAssetConstraint& c = d.Constraints[i];
            c.ParentBone              = r.Str();
            c.ChildBone               = r.Str();
            c.Position                = r.V3();
            c.Rotation                = r.Q();
            c.Swing1LimitDegrees      = r.F32();
            c.Swing2LimitDegrees      = r.F32();
            c.TwistLimitDegrees       = r.F32();
        }

        if ( r.Failed() )
            return Common::MakeFormattedError<PhysicsAssetData>( "the 'DPHA' payload ({} bytes) is truncated",
                                                                 bytes.size() );
        if ( r.Remaining() != 0 )
            return Common::MakeFormattedError<PhysicsAssetData>(
                 "the 'DPHA' payload has {} bytes past its last constraint", r.Remaining() );
        return Common::MakeSuccess( std::move( d ) );
    }

    Common::ResultStr<std::vector<unsigned char>> EncodePhysicsAsset( const PhysicsAssetData& data )
    {
        if ( data.Guid.IsNull() )
            return Common::MakeError<std::vector<unsigned char>>(
                 "a .dephysasset needs a GUID; the null GUID names no asset" );
        const std::vector<unsigned char> payload = EncodePhysicsAssetPayload( data );

        CC::AssetEnvelope envelope;
        envelope.Asset.Kind       = CC::ContentKind::PhysicsAsset;
        envelope.Asset.Guid       = data.Guid;
        envelope.Asset.Subsystems = { { kPhysicsAssetSubsystemTag, kPhysicsAssetFormatVersion } };
        const std::span<const std::byte> payloadBytes = std::as_bytes( std::span( payload ) );
        envelope.Sections.push_back( { CC::EnvelopeSection::Payload, CC::EnvelopeCodec::Stored,
                                       std::vector<std::byte>( payloadBytes.begin(), payloadBytes.end() ) } );

        auto file = CC::WriteAssetEnvelope( envelope );
        if ( !file )
            return Common::MakeFormattedError<std::vector<unsigned char>>( "cannot wrap the physics asset: {}",
                                                                           file.GetError() );
        const std::vector<std::byte>& wrapped = file.GetValue();
        std::vector<unsigned char>    out( wrapped.size() );
        std::memcpy( out.data(), wrapped.data(), wrapped.size() );
        return Common::MakeSuccess( std::move( out ) );
    }

    Common::ResultStr<PhysicsAssetData> DecodePhysicsAsset( const std::vector<unsigned char>& file )
    {
        const CC::SubsystemVersion kKnown[] = { { kPhysicsAssetSubsystemTag, kPhysicsAssetFormatVersion } };
        auto                       envelope =
             CC::ReadAssetEnvelope( std::as_bytes( std::span( file ) ), CC::AssetHeaderReadContext{ kKnown } );
        if ( !envelope )
            return Common::MakeFormattedError<PhysicsAssetData>( "not a physics asset envelope: {}",
                                                                 envelope.GetError() );
        const CC::AssetEnvelope& e = envelope.GetValue();
        if ( e.Asset.Kind != CC::ContentKind::PhysicsAsset )
            return Common::MakeFormattedError<PhysicsAssetData>( "the envelope's kind is {}, not PhysicsAsset",
                                                                 CC::KindName( e.Asset.Kind ) );
        if ( e.Asset.Subsystems.size() != 1u || e.Asset.Subsystems[0].Tag != kPhysicsAssetSubsystemTag )
            return Common::MakeFormattedError<PhysicsAssetData>(
                 "the envelope states {} subsystem versions; a physics asset states exactly one, under 'DPHA'",
                 e.Asset.Subsystems.size() );
        const auto section = std::find_if( e.Sections.begin(), e.Sections.end(),
                                           []( const auto& s ) { return s.Tag == CC::EnvelopeSection::Payload; } );
        if ( section == e.Sections.end() )
            return Common::MakeError<PhysicsAssetData>( "the physics asset envelope has no PAYL section" );

        std::vector<unsigned char> payload( section->Bytes.size() );
        std::memcpy( payload.data(), section->Bytes.data(), section->Bytes.size() );
        auto decoded = DecodePhysicsAssetPayload( payload );
        if ( !decoded )
            return decoded;
        PhysicsAssetData data = decoded.ExtractValue();
        data.Guid             = e.Asset.Guid;
        return Common::MakeSuccess( std::move( data ) );
    }

    Common::ResultStr<CC::AssetGuid> ReadPhysicsAssetGuid( std::istream& in )
    {
        const CC::SubsystemVersion kKnown[] = { { kPhysicsAssetSubsystemTag, kPhysicsAssetFormatVersion } };
        auto                       header   = CC::ReadEnvelopeHeader( in, CC::AssetHeaderReadContext{ kKnown } );
        if ( !header )
            return Common::MakeFormattedError<CC::AssetGuid>( "not a physics asset envelope: {}",
                                                              header.GetError() );
        if ( header.GetValue().Asset.Kind != CC::ContentKind::PhysicsAsset )
            return Common::MakeFormattedError<CC::AssetGuid>( "the envelope's kind is {}, not PhysicsAsset",
                                                              CC::KindName( header.GetValue().Asset.Kind ) );
        return Common::MakeSuccess( header.GetValue().Asset.Guid );
    }
} // namespace Desert::Physics
