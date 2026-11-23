// The GNS side of the mesh CA: GNS's own certificate format (CMsgSteamDatagramCertificateSigned, Ed25519), signed
// and trusted through its certificate store - the same objects steamnetworkingsockets_certtool writes, without
// the tool (its JSON dependency is not vendored). Compiled with GNS's private include directories and defines
// (Desert/Net/CMakeLists.txt); nothing of GNS's internals leaves this file.
#include "Net/MeshCertificates.hpp"

#include "steamnetworkingsockets/steamnetworkingsockets_internal.h"
#include "steamnetworkingsockets/steamnetworkingsockets_certstore.h"
#include "crypto.h"
#include "crypto_25519.h"
#include "steamnetworkingsockets_messages_certs.pb.h"

#include <steam/steamnetworkingsockets.h>

#include <ctime>
#include <format>
#include <fstream>
#include <sstream>

namespace Desert::Net
{
    namespace
    {
        using namespace SteamNetworkingSocketsLib;

        std::string Base64( const std::string& bytes )
        {
            uint32      size = CCrypto::Base64EncodeMaxOutput( static_cast<uint32>( bytes.size() ), "" ) + 1;
            std::string text( size, '\0' );
            if ( !CCrypto::Base64Encode( reinterpret_cast<const uint8*>( bytes.data() ),
                                         static_cast<uint32>( bytes.size() ), text.data(), &size, "" ) )
                return {};
            text.resize( size );
            while ( !text.empty() && ( text.back() == '\0' || text.back() == '\n' || text.back() == '\r' ) )
                text.pop_back();
            return text;
        }

        std::string Unbase64( const std::string& text )
        {
            uint32      size = CCrypto::Base64DecodeMaxOutput( static_cast<uint32>( text.size() ) );
            std::string bytes( size, '\0' );
            if ( !CCrypto::Base64Decode( text.data(), text.size(), reinterpret_cast<uint8*>( bytes.data() ), &size ) )
                return {};
            bytes.resize( size );
            return bytes;
        }

        bool WriteText( const std::filesystem::path& path, const std::string& text, bool secret, std::string& error )
        {
            std::ofstream out( path, std::ios::binary | std::ios::trunc );
            out << text << "\n";
            if ( !out )
            {
                error = std::format( "cannot write '{}'", path.string() );
                return false;
            }
            out.close();
            if ( secret )
            {
                std::error_code ec;
                std::filesystem::permissions( path,
                                              std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                              std::filesystem::perm_options::replace, ec );
            }
            return true;
        }

        std::string ReadText( const std::filesystem::path& path )
        {
            std::ifstream     in( path, std::ios::binary );
            std::stringstream text;
            text << in.rdbuf();
            std::string s = text.str();
            while ( !s.empty() && ( s.back() == '\n' || s.back() == '\r' || s.back() == ' ' ) )
                s.pop_back();
            return s;
        }

        // A certificate for @p key, signed by @p ca; @p identity empty for the root.
        CMsgSteamDatagramCertificateSigned Sign( const CECSigningPublicKey& key, const std::string& identity,
                                                 const CECSigningPrivateKey& ca, int days )
        {
            CMsgSteamDatagramCertificate cert;
            cert.set_key_type( CMsgSteamDatagramCertificate_EKeyType_ED25519 );
            key.GetRawDataAsStdString( cert.mutable_key_data() );
            if ( !identity.empty() )
            {
                cert.set_identity_string( "str:" + identity );
                // GNS accepts a peer certificate only when it is bound to the process's AppID; our servers run
                // outside Steam, where that AppID is 0 (the root stays unrestricted).
                cert.add_app_ids( 0 );
            }
            cert.set_time_created( static_cast<uint32>( std::time( nullptr ) ) );
            cert.set_time_expiry( cert.time_created() + static_cast<uint32>( days ) * 24u * 3600u );

            CECSigningPublicKey caPublic;
            ca.GetPublicKey( &caPublic );
            CMsgSteamDatagramCertificateSigned signedCert;
            signedCert.set_cert( cert.SerializeAsString() );
            CryptoSignature_t signature;
            ca.GenerateSignature( signedCert.cert().data(), signedCert.cert().size(), &signature );
            signedCert.set_ca_key_id( CalculatePublicKeyID( caPublic ) );
            signedCert.set_ca_signature( &signature, sizeof( signature ) );
            return signedCert;
        }
    } // namespace

    bool CreateMeshDevCa( const std::filesystem::path& directory, const std::vector<std::string>& identities,
                          int days, std::string& error )
    {
        CCrypto::Init(); // no transport exists in this mode: GameNetworkingSockets_Init would have done it
        std::error_code ec;
        std::filesystem::create_directories( directory, ec );
        if ( ec )
        {
            error = std::format( "cannot create '{}': {}", directory.string(), ec.message() );
            return false;
        }
        CECSigningPublicKey  caPublic;
        CECSigningPrivateKey caPrivate;
        CCrypto::GenerateSigningKeyPair( &caPublic, &caPrivate );
        char   pem[4096];
        uint32 pemSize = 0;
        if ( !caPrivate.GetAsPEM( pem, sizeof( pem ), &pemSize ) )
        {
            error = "the CA private key does not encode as PEM";
            return false;
        }
        if ( !WriteText( directory / "ca.key", std::string( pem, pemSize ), true, error ) ||
             !WriteText( directory / "ca.cert", Base64( Sign( caPublic, {}, caPrivate, days ).SerializeAsString() ),
                         false, error ) )
            return false;
        for ( const std::string& identity : identities )
        {
            CECSigningPublicKey  keyPublic;
            CECSigningPrivateKey keyPrivate;
            CCrypto::GenerateSigningKeyPair( &keyPublic, &keyPrivate );
            CMsgSteamDatagramCertificateSigned cert = Sign( keyPublic, identity, caPrivate, days );
            keyPrivate.GetRawDataAsStdString( cert.mutable_private_key_data() );
            if ( !WriteText( directory / ( identity + ".cert" ), Base64( cert.SerializeAsString() ), true, error ) )
                return false;
        }
        return true;
    }

    bool InstallMeshCertificate( const std::filesystem::path& directory, std::string_view identity,
                                 std::string& error )
    {
        const std::string root = ReadText( directory / "ca.cert" );
        const std::string own  = Unbase64( ReadText( directory / ( std::string( identity ) + ".cert" ) ) );
        if ( root.empty() || own.empty() )
        {
            error = std::format( "no mesh CA at '{}' (ca.cert and {}.cert; make one: DesertServer --mesh-dev-ca "
                                 "<dir>)",
                                 directory.string(), identity );
            return false;
        }
        SteamNetworkingErrMsg gnsError;
        if ( !CertStore_AddCertFromBase64( root.c_str(), gnsError ) )
        {
            error = std::format( "'{}' is not a usable root: {}", ( directory / "ca.cert" ).string(), gnsError );
            return false;
        }
        if ( !SteamNetworkingSockets()->SetCertificate( own.data(), static_cast<int>( own.size() ), gnsError ) )
        {
            error = std::format( "'{}.cert' refused by GNS: {}", identity, gnsError );
            return false;
        }
        return true;
    }
} // namespace Desert::Net
