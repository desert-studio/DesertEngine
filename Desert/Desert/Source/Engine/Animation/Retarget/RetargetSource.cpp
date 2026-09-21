#include <Engine/Animation/Retarget/RetargetSource.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Animation::Retarget
{
    Common::ResultStr<std::unique_ptr<RetargetSource>> RetargetSource::Create( Skeleton        source,
                                                                               const Skeleton& target,
                                                                               RetargetSetup   setup,
                                                                               uint64_t        assetHandle,
                                                                               uint32_t        assetRevision )
    {
        // The signatures are read BEFORE the move, from the two rigs actually being used, rather than
        // taken as parameters. A stamp that could disagree with the thing it stamps is the defect the
        // stamp exists to prevent.
        const uint64_t sourceSignature = source.GetSignature();
        const uint64_t targetSignature = target.GetSignature();

        auto built = std::unique_ptr<RetargetSource>( new RetargetSource( std::move( source ) ) );

        if ( auto ok = built->m_Retargeter.Initialize( built->m_Source, target, std::move( setup ) ); !ok )
        {
            return Common::MakeFormattedError<std::unique_ptr<RetargetSource>>( "{}", ok.GetError() );
        }

        built->m_AssetHandle     = assetHandle;
        built->m_AssetRevision   = assetRevision;
        built->m_SourceSignature = sourceSignature;
        built->m_TargetSignature = targetSignature;

        // SIZED HERE AND NOT ON THE FIRST FRAME. `Retargeter::Retarget` refuses a source pose that is not
        // the source rig's size, so an unsized scratch would make the first frame after every rebuild a
        // refusal — once per rebuild, which is exactly often enough to look like a flicker and rare enough
        // never to be caught.
        built->m_SourceScratch = built->m_Retargeter.GetSourceInitialPose();
        built->m_LayerScratch.Resize( target.GetBones().size() );

        return Common::MakeSuccess( std::move( built ) );
    }

    bool RetargetSource::Run( const Skeleton& target, const LocalPose& sourceLocal, LocalPose& out )
    {
        auto result = m_Retargeter.Retarget( m_Source, target, sourceLocal, out );
        if ( result )
        {
            m_LastError.clear();
            m_LastLoggedError.clear();
            return true;
        }

        m_LastError = result.GetError();
        if ( m_LastError != m_LastLoggedError )
        {
            m_LastLoggedError = m_LastError;
            LOG_ERROR( "[Animation] retarget from rig (signature {}) onto rig (signature {}) refused: {}",
                       m_SourceSignature, m_TargetSignature, m_LastError );
        }
        return false;
    }
} // namespace Desert::Animation::Retarget
