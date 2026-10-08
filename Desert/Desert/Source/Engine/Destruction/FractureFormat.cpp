#include <Engine/Destruction/FractureFormat.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <istream>
#include <span>
#include <string>

namespace Desert::Destruction
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
            void I32( int32_t v )
            {
                U32( static_cast<uint32_t>( v ) );
            }
            void F32( float v )
            {
                U32( std::bit_cast<uint32_t>( v ) );
            }
            void F64( double v )
            {
                U64( std::bit_cast<uint64_t>( v ) );
            }
            void D3( const glm::dvec3& v )
            {
                F64( v.x ), F64( v.y ), F64( v.z );
            }
            void Floats( const std::vector<float>& v )
            {
                U32( static_cast<uint32_t>( v.size() ) );
                for ( const float f : v )
                    F32( f );
            }
            void Ints( const std::vector<int>& v )
            {
                U32( static_cast<uint32_t>( v.size() ) );
                for ( const int i : v )
                    I32( i );
            }
            void Overlay( const Geometry::EditMeshOverlaySer& o )
            {
                Floats( o.Values );
                Ints( o.Triangles );
            }
            void OptionalOverlay( const std::optional<Geometry::EditMeshOverlaySer>& o )
            {
                U8( o ? 1 : 0 );
                if ( o )
                    Overlay( *o );
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
            int32_t I32()
            {
                return static_cast<int32_t>( U32() );
            }
            float F32()
            {
                return std::bit_cast<float>( U32() );
            }
            double F64()
            {
                return std::bit_cast<double>( U64() );
            }
            glm::dvec3 D3()
            {
                const double x = F64();
                const double y = F64();
                const double z = F64();
                return { x, y, z };
            }
            /// A count of records of @p recordBytes each, refused when the payload cannot hold them.
            uint32_t Count( size_t recordBytes )
            {
                const uint32_t n = U32();
                if ( recordBytes > 0 && static_cast<uint64_t>( n ) * recordBytes > Remaining() )
                    m_Failed = true;
                return m_Failed ? 0u : n;
            }
            std::vector<float> Floats()
            {
                std::vector<float> v( Count( 4 ) );
                for ( float& f : v )
                    f = F32();
                return v;
            }
            std::vector<int> Ints()
            {
                std::vector<int> v( Count( 4 ) );
                for ( int& i : v )
                    i = I32();
                return v;
            }
            Geometry::EditMeshOverlaySer Overlay()
            {
                Geometry::EditMeshOverlaySer o;
                o.Values    = Floats();
                o.Triangles = Ints();
                return o;
            }
            std::optional<Geometry::EditMeshOverlaySer> OptionalOverlay()
            {
                if ( U8() == 0 )
                    return std::nullopt;
                return Overlay();
            }

        private:
            const std::vector<unsigned char>& m_Bytes;
            size_t                            m_At     = 0;
            bool                              m_Failed = false;
        };

        void WriteLevel( Writer& w, const FractureLevelSettings& l )
        {
            w.U8( static_cast<uint8_t>( l.Method ) );
            w.I32( l.SiteCount );
            w.I32( l.Clusters );
            w.I32( l.SitesPerCluster );
            w.F64( l.MinRadius );
            w.F64( l.MaxRadius );
            w.U32( static_cast<uint32_t>( l.Planes.size() ) );
            for ( const CutPlane& p : l.Planes )
            {
                w.D3( p.Normal );
                w.D3( p.Point );
            }
            w.U8( static_cast<uint8_t>( l.Brick.Bond ) );
            w.F64( l.Brick.Length );
            w.F64( l.Brick.Height );
            w.F64( l.Brick.Depth );
            w.F32( l.DamageThreshold );
        }

        FractureLevelSettings ReadLevel( Reader& r )
        {
            FractureLevelSettings l;
            l.Method          = static_cast<FractureMethod>( r.U8() );
            l.SiteCount       = r.I32();
            l.Clusters        = r.I32();
            l.SitesPerCluster = r.I32();
            l.MinRadius       = r.F64();
            l.MaxRadius       = r.F64();
            l.Planes.resize( r.Count( 48 ) );
            for ( CutPlane& p : l.Planes )
            {
                p.Normal = r.D3();
                p.Point  = r.D3();
            }
            l.Brick.Bond      = static_cast<BrickBond>( r.U8() );
            l.Brick.Length    = r.F64();
            l.Brick.Height    = r.F64();
            l.Brick.Depth     = r.F64();
            l.DamageThreshold = r.F32();
            return l;
        }
    } // namespace

    std::vector<unsigned char> EncodeFracturePayload( const FractureData& data )
    {
        Writer w;
        w.U64( data.SourceMesh.Hi );
        w.U64( data.SourceMesh.Lo );

        const FractureSettings& s = data.Settings;
        w.U64( s.Seed );
        w.U32( static_cast<uint32_t>( s.Levels.size() ) );
        for ( const FractureLevelSettings& l : s.Levels )
            WriteLevel( w, l );
        w.U8( s.AutoCluster.Enabled ? 1 : 0 );
        w.I32( s.AutoCluster.GridX );
        w.I32( s.AutoCluster.GridY );
        w.I32( s.AutoCluster.GridZ );
        w.I32( s.AutoCluster.DriftIterations );
        w.F64( s.InteriorUVScale );

        w.I32( data.InteriorMaterialId );
        w.U32( static_cast<uint32_t>( data.Nodes.size() ) );
        for ( const FractureNode& n : data.Nodes )
        {
            w.I32( n.Parent );
            w.U32( n.Level );
            w.U8( static_cast<uint8_t>( n.Kind ) );
            w.F32( n.DamageThreshold );
            w.F64( n.Volume );
            w.D3( n.CenterOfMass );

            w.Floats( n.Mesh.Positions );
            w.Ints( n.Mesh.Triangles );
            w.Ints( n.Mesh.PolyGroups );
            w.Ints( n.Mesh.MaterialIds );
            w.OptionalOverlay( n.Mesh.Normals );
            w.OptionalOverlay( n.Mesh.Tangents );
            w.OptionalOverlay( n.Mesh.Colors );
            w.U32( static_cast<uint32_t>( n.Mesh.UVs.size() ) );
            for ( const auto& uv : n.Mesh.UVs )
                w.Overlay( uv );

            w.U32( static_cast<uint32_t>( n.HullVertices.size() ) );
            for ( const glm::vec3& v : n.HullVertices )
                w.F32( v.x ), w.F32( v.y ), w.F32( v.z );
            w.U32( static_cast<uint32_t>( n.HullFaces.size() ) );
            for ( const std::vector<int>& f : n.HullFaces )
                w.Ints( f );
        }
        return std::move( w.Bytes );
    }

    Common::ResultStr<FractureData> DecodeFracturePayload( const std::vector<unsigned char>& bytes )
    {
        Reader       r( bytes );
        FractureData d;
        d.SourceMesh.Hi = r.U64();
        d.SourceMesh.Lo = r.U64();

        FractureSettings& s = d.Settings;
        s.Seed              = r.U64();
        s.Levels.resize( r.Count( 1 ) );
        for ( FractureLevelSettings& l : s.Levels )
            l = ReadLevel( r );
        s.AutoCluster.Enabled         = r.U8() != 0;
        s.AutoCluster.GridX           = r.I32();
        s.AutoCluster.GridY           = r.I32();
        s.AutoCluster.GridZ           = r.I32();
        s.AutoCluster.DriftIterations = r.I32();
        s.InteriorUVScale             = r.F64();

        d.InteriorMaterialId = r.I32();
        d.Nodes.resize( r.Count( 1 ) );
        for ( FractureNode& n : d.Nodes )
        {
            n.Parent          = r.I32();
            n.Level           = r.U32();
            n.Kind            = static_cast<FractureNodeKind>( r.U8() );
            n.DamageThreshold = r.F32();
            n.Volume          = r.F64();
            n.CenterOfMass    = r.D3();

            n.Mesh.Positions   = r.Floats();
            n.Mesh.Triangles   = r.Ints();
            n.Mesh.PolyGroups  = r.Ints();
            n.Mesh.MaterialIds = r.Ints();
            n.Mesh.Normals     = r.OptionalOverlay();
            n.Mesh.Tangents    = r.OptionalOverlay();
            n.Mesh.Colors      = r.OptionalOverlay();
            n.Mesh.UVs.resize( r.Count( 8 ) );
            for ( auto& uv : n.Mesh.UVs )
                uv = r.Overlay();

            n.HullVertices.resize( r.Count( 12 ) );
            for ( glm::vec3& v : n.HullVertices )
            {
                const float x = r.F32();
                const float y = r.F32();
                const float z = r.F32();
                v = { x, y, z };
            }
            n.HullFaces.resize( r.Count( 4 ) );
            for ( std::vector<int>& f : n.HullFaces )
                f = r.Ints();
            if ( r.Failed() )
                break;
        }
        if ( r.Failed() )
            return Common::MakeFormattedError<FractureData>( "the 'DFRC' payload of {} bytes is truncated",
                                                             bytes.size() );
        if ( r.Remaining() != 0 )
            return Common::MakeFormattedError<FractureData>( "the 'DFRC' payload has {} bytes past its last node",
                                                             r.Remaining() );
        for ( size_t i = 0; i < d.Nodes.size(); ++i )
        {
            const int32_t p = d.Nodes[i].Parent;
            if ( i == 0 ? p != -1 : ( p < 0 || static_cast<size_t>( p ) >= i ) )
                return Common::MakeFormattedError<FractureData>(
                     "node {} names parent {}; the root is node 0 and every parent precedes its children", i, p );
        }
        return Common::MakeSuccess( std::move( d ) );
    }

    Common::ResultStr<std::vector<unsigned char>> EncodeFracture( const FractureData& data )
    {
        if ( data.Guid.IsNull() )
            return Common::MakeError<std::vector<unsigned char>>(
                 "a .dfrac needs a GUID; the null GUID names no asset" );
        const std::vector<unsigned char> payload = EncodeFracturePayload( data );

        CC::AssetEnvelope envelope;
        envelope.Asset.Kind                           = CC::ContentKind::Fracture;
        envelope.Asset.Guid                           = data.Guid;
        envelope.Asset.Subsystems                     = { { kFractureSubsystemTag, kFractureFormatVersion } };
        const std::span<const std::byte> payloadBytes = std::as_bytes( std::span( payload ) );
        envelope.Sections.push_back( { CC::EnvelopeSection::Payload, CC::EnvelopeCodec::Stored,
                                       std::vector<std::byte>( payloadBytes.begin(), payloadBytes.end() ) } );

        auto file = CC::WriteAssetEnvelope( envelope );
        if ( !file )
            return Common::MakeFormattedError<std::vector<unsigned char>>( "cannot wrap the fracture: {}",
                                                                           file.GetError() );
        const std::vector<std::byte>& wrapped = file.GetValue();
        std::vector<unsigned char>    out( wrapped.size() );
        std::memcpy( out.data(), wrapped.data(), wrapped.size() );
        return Common::MakeSuccess( std::move( out ) );
    }

    Common::ResultStr<FractureData> DecodeFracture( const std::vector<unsigned char>& file )
    {
        const CC::SubsystemVersion kKnown[] = { { kFractureSubsystemTag, kFractureFormatVersion } };
        auto                       envelope =
             CC::ReadAssetEnvelope( std::as_bytes( std::span( file ) ), CC::AssetHeaderReadContext{ kKnown } );
        if ( !envelope )
            return Common::MakeFormattedError<FractureData>( "not a fracture envelope: {}", envelope.GetError() );
        const CC::AssetEnvelope& e = envelope.GetValue();
        if ( e.Asset.Kind != CC::ContentKind::Fracture )
            return Common::MakeFormattedError<FractureData>( "the envelope's kind is {}, not Fracture",
                                                             CC::KindName( e.Asset.Kind ) );
        if ( e.Asset.Subsystems.size() != 1u || e.Asset.Subsystems[0].Tag != kFractureSubsystemTag )
            return Common::MakeFormattedError<FractureData>(
                 "the envelope states {} subsystem versions; a fracture states exactly one, under 'DFRC'",
                 e.Asset.Subsystems.size() );
        const auto section = std::find_if( e.Sections.begin(), e.Sections.end(),
                                           []( const auto& s ) { return s.Tag == CC::EnvelopeSection::Payload; } );
        if ( section == e.Sections.end() )
            return Common::MakeError<FractureData>( "the fracture envelope has no PAYL section" );

        std::vector<unsigned char> payload( section->Bytes.size() );
        std::memcpy( payload.data(), section->Bytes.data(), section->Bytes.size() );
        auto decoded = DecodeFracturePayload( payload );
        if ( !decoded )
            return decoded;
        FractureData data = decoded.ExtractValue();
        data.Guid         = e.Asset.Guid;
        return Common::MakeSuccess( std::move( data ) );
    }

    Common::ResultStr<CC::AssetGuid> ReadFractureGuid( std::istream& in )
    {
        const CC::SubsystemVersion kKnown[] = { { kFractureSubsystemTag, kFractureFormatVersion } };
        auto                       header   = CC::ReadEnvelopeHeader( in, CC::AssetHeaderReadContext{ kKnown } );
        if ( !header )
            return Common::MakeFormattedError<CC::AssetGuid>( "not a fracture envelope: {}", header.GetError() );
        if ( header.GetValue().Asset.Kind != CC::ContentKind::Fracture )
            return Common::MakeFormattedError<CC::AssetGuid>( "the envelope's kind is {}, not Fracture",
                                                              CC::KindName( header.GetValue().Asset.Kind ) );
        return Common::MakeSuccess( header.GetValue().Asset.Guid );
    }
} // namespace Desert::Destruction
