#include "AudioMix.hpp"

#include <Common/Content/AssetGuid.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <string>
#include <utility>

namespace Desert::Audio
{
    namespace
    {
        [[nodiscard]] bool Contains( std::span<const SoundClassDesc> classes, const SoundClassId& id )
        {
            return std::any_of( classes.begin(), classes.end(),
                                [&id]( const SoundClassDesc& desc ) { return desc.Id == id; } );
        }

        // Every class's own numbers and identity, before the set as a whole is looked at.
        [[nodiscard]] Common::BoolResultStr CheckEach( std::span<const SoundClassDesc> classes )
        {
            for ( std::size_t i = 0; i < classes.size(); ++i )
            {
                const SoundClassDesc& desc = classes[i];
                if ( desc.Id.IsNull() )
                    return Common::MakeFormattedError( "[Audio] sound class #{} of the set has a null GUID", i );
                const std::string guid = Common::Content::AssetGuidToText( desc.Id );
                if ( Contains( classes.first( i ), desc.Id ) )
                    return Common::MakeFormattedError( "[Audio] sound class {} is in the set twice", guid );
                if ( !( desc.Volume >= 0.0f ) )
                    return Common::MakeFormattedError( "[Audio] sound class {} has Volume {} (must be >= 0)", guid,
                                                       desc.Volume );
                if ( !( desc.Pitch > 0.0f ) )
                    return Common::MakeFormattedError( "[Audio] sound class {} has Pitch {} (must be > 0)", guid,
                                                       desc.Pitch );
                if ( !desc.Parent.IsNull() && !Contains( classes, desc.Parent ) )
                    return Common::MakeFormattedError(
                         "[Audio] sound class {} names parent {}, which is not in the set", guid,
                         Common::Content::AssetGuidToText( desc.Parent ) );
            }
            return Common::MakeSuccess( true );
        }

        // Parents before children (Kahn over a forest): each pass emits, in the order given, every class whose
        // parent is a root or already emitted. A pass that emits nothing while classes remain means the rest
        // are on a cycle (a class that is its own parent included).
        [[nodiscard]] Common::ResultStr<std::vector<SoundClassDesc>>
        SortParentsFirst( std::span<const SoundClassDesc> classes )
        {
            std::vector<SoundClassDesc> sorted;
            sorted.reserve( classes.size() );
            std::vector<bool> emitted( classes.size(), false );
            while ( sorted.size() < classes.size() )
            {
                const std::size_t before = sorted.size();
                for ( std::size_t i = 0; i < classes.size(); ++i )
                {
                    if ( emitted[i] )
                        continue;
                    const SoundClassDesc& desc = classes[i];
                    if ( desc.Parent.IsNull() || Contains( sorted, desc.Parent ) )
                    {
                        sorted.push_back( desc );
                        emitted[i] = true;
                    }
                }
                if ( sorted.size() == before )
                {
                    const auto onCycle = std::find( emitted.begin(), emitted.end(), false );
                    const auto index   = static_cast<std::size_t>( std::distance( emitted.begin(), onCycle ) );
                    return Common::MakeFormattedError<std::vector<SoundClassDesc>>(
                         "[Audio] sound class {} is on a parent cycle",
                         Common::Content::AssetGuidToText( classes[index].Id ) );
                }
            }
            return Common::MakeSuccess( std::move( sorted ) );
        }
    } // namespace

    AudioMix& AudioMix::Get()
    {
        static AudioMix s_Mix;
        return s_Mix;
    }

    Common::BoolResultStr AudioMix::SetClasses( std::span<const SoundClassDesc> classes )
    {
        auto checked = CheckEach( classes );
        if ( !checked.IsSuccess() )
            return checked;
        auto sorted = SortParentsFirst( classes );
        if ( !sorted.IsSuccess() )
            return Common::MakeError( sorted.GetError() );

        // The player's volumes are keyed by Id and live beside the set: nothing to carry over.
        m_Classes = sorted.ExtractValue();
        ++m_Version;
        Changed( MixChange::Classes );
        return Common::MakeSuccess( true );
    }

    std::span<const SoundClassDesc> AudioMix::Classes() const
    {
        return m_Classes;
    }

    bool AudioMix::Has( const SoundClassId& id ) const
    {
        return Contains( m_Classes, id );
    }

    uint64_t AudioMix::Version() const
    {
        return m_Version;
    }

    void AudioMix::SetMasterVolume( float volume )
    {
        m_Master = volume;
        Changed( MixChange::Volumes );
    }

    float AudioMix::MasterVolume() const
    {
        return m_Master;
    }

    Common::BoolResultStr AudioMix::SetUserVolume( const SoundClassId& id, float volume )
    {
        if ( id.IsNull() )
            return Common::MakeError( "[Audio] a user volume was set for a null sound class GUID" );
        const auto entry = std::find_if( m_UserVolumes.begin(), m_UserVolumes.end(),
                                         [&id]( const auto& userVolume ) { return userVolume.first == id; } );
        if ( entry != m_UserVolumes.end() )
            entry->second = volume;
        else
            m_UserVolumes.emplace_back( id, volume );
        Changed( MixChange::Volumes );
        return Common::MakeSuccess( true );
    }

    float AudioMix::UserVolume( const SoundClassId& id ) const
    {
        const auto entry = std::find_if( m_UserVolumes.begin(), m_UserVolumes.end(),
                                         [&id]( const auto& userVolume ) { return userVolume.first == id; } );
        return entry != m_UserVolumes.end() ? entry->second : 1.0f;
    }

    std::vector<SoundClassId> AudioMix::UnmatchedUserVolumes() const
    {
        std::vector<SoundClassId> unmatched;
        if ( m_Version == 0 )
            return unmatched; // no set yet: every volume is still waiting for its class, none is unmatched
        for ( const auto& userVolume : m_UserVolumes )
            if ( !Has( userVolume.first ) )
                unmatched.push_back( userVolume.first );
        return unmatched;
    }

    void AudioMix::SetListener( std::function<void( MixChange )> listener )
    {
        m_Listener = std::move( listener );
    }

    void AudioMix::Changed( MixChange change ) const
    {
        if ( m_Listener )
            m_Listener( change );
    }
} // namespace Desert::Audio
