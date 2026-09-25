#include <Common/Content/AssetRedirector.hpp>

#include <Common/Core/Core.hpp>

namespace Common::Content
{
    namespace
    {
        // Every rule a redirector obeys, checked on the way out AND on the way in, so a writer cannot
        // produce a file the reader refuses.
        BoolResultStr CheckRedirector( const AssetRedirector& redirector )
        {
            if ( redirector.Self.IsNull() )
                return MakeFormattedError( "redirector '{}': its own GUID is null", redirector.OldKey );
            if ( redirector.Target.IsNull() )
                return MakeFormattedError( "redirector '{}': its target GUID is null", redirector.OldKey );
            if ( redirector.Target == redirector.Self )
                return MakeFormattedError( "redirector '{}': it names itself as its target", redirector.OldKey );
            if ( redirector.OldKey.empty() )
                return MakeError( "redirector: the stable key it was written for is empty" );
            return BOOLSUCCESS;
        }

        AssetEnvelope EnvelopeOf( const AssetRedirector& redirector )
        {
            AssetEnvelope envelope;
            envelope.Asset.Kind         = ContentKind::Redirector;
            envelope.Asset.Guid         = redirector.Self;
            envelope.Asset.Dependencies = { redirector.Target };
            EnvelopeMeta meta;
            meta.Name = redirector.OldKey;
            envelope.Sections.push_back(
                 { EnvelopeSection::Meta, EnvelopeCodec::Stored, EncodeEnvelopeMeta( meta ) } );
            return envelope;
        }
    } // namespace

    ResultStr<std::vector<std::byte>> EncodeRedirector( const AssetRedirector& redirector )
    {
        if ( auto valid = CheckRedirector( redirector ); !valid )
            return MakeError<std::vector<std::byte>>( valid.GetError() );
        return WriteAssetEnvelope( EnvelopeOf( redirector ) );
    }

    BoolResultStr WriteRedirectorFile( const std::filesystem::path& file, const AssetRedirector& redirector )
    {
        if ( auto valid = CheckRedirector( redirector ); !valid )
            return valid;
        return WriteAssetEnvelopeFile( file, EnvelopeOf( redirector ) );
    }

    namespace
    {
        ResultStr<AssetRedirector> FromEnvelope( const AssetEnvelope& read )
        {
            if ( read.Asset.Kind != ContentKind::Redirector )
                return MakeFormattedError<AssetRedirector>( "redirector: the header states kind '{}'",
                                                            KindName( read.Asset.Kind ) );
            if ( read.Asset.Dependencies.size() != 1 )
                return MakeFormattedError<AssetRedirector>(
                     "redirector: it names {} targets, a redirector names one", read.Asset.Dependencies.size() );
            if ( read.Sections.size() != 1 || read.Sections.front().Tag != EnvelopeSection::Meta )
                return MakeFormattedError<AssetRedirector>(
                     "redirector: it carries {} section(s); a redirector carries its Meta and no body",
                     read.Sections.size() );
            auto meta = DecodeEnvelopeMeta( read.Sections.front().Bytes );
            if ( !meta )
                return MakeError<AssetRedirector>( meta.GetError() );

            AssetRedirector redirector{ read.Asset.Guid, read.Asset.Dependencies.front(), meta.GetValue().Name };
            if ( auto valid = CheckRedirector( redirector ); !valid )
                return MakeError<AssetRedirector>( valid.GetError() );
            return MakeSuccess( std::move( redirector ) );
        }

        // Record only: a redirector states no subsystem versions, so there is nothing for a build to judge.
        constexpr AssetHeaderReadContext kRecordOnly{ {}, true };
    } // namespace

    ResultStr<AssetRedirector> DecodeRedirector( std::span<const std::byte> file )
    {
        auto envelope = ReadAssetEnvelope( file, kRecordOnly );
        if ( !envelope )
            return MakeError<AssetRedirector>( envelope.GetError() );
        return FromEnvelope( envelope.GetValue() );
    }

    ResultStr<AssetRedirector> ReadRedirectorFile( const std::filesystem::path& file )
    {
        auto envelope = ReadAssetEnvelopeFile( file, kRecordOnly );
        if ( !envelope )
            return MakeError<AssetRedirector>( envelope.GetError() );
        auto redirector = FromEnvelope( envelope.GetValue() );
        if ( !redirector )
            return MakeFormattedError<AssetRedirector>( "{} ('{}')", redirector.GetError(), file.string() );
        return redirector;
    }
} // namespace Common::Content
