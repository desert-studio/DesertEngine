#include "AudioEngine.hpp"
#include "AudioMix.hpp"

#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <miniaudio/miniaudio.h>

#include <array>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace Desert::Audio
{
    namespace
    {
        // Resolve a clip path: as given first, then relative to the project's Assets root. Uses the
        // VFS-aware Exists so packaged games resolve into the mounted .dpak.
        std::filesystem::path ResolveClipPath( const std::string& clipPath )
        {
            if ( Common::Utils::FileSystem::Exists( clipPath ) )
                return clipPath;
            const auto assetsRelative = Common::Constants::Path::ASSETS_PATH / clipPath;
            if ( Common::Utils::FileSystem::Exists( assetsRelative ) )
                return assetsRelative;
            return {};
        }
    } // namespace

    // A playing sound: the (VFS-read) clip bytes must outlive the decoder, the decoder the sound.
    struct Sound
    {
        std::vector<uint8_t> Bytes;
        ma_decoder        Decoder{};
        ma_sound          Handle{};
        bool              DecoderReady = false;
        bool              SoundReady   = false;
        bool              OneShot      = false;

        ~Sound()
        {
            if ( SoundReady )
                ma_sound_uninit( &Handle );
            if ( DecoderReady )
                ma_decoder_uninit( &Decoder );
        }
    };

    struct AudioEngine::Impl
    {
        ma_engine Engine{};
        bool      Initialized = false;
        bool      InitFailed  = false;
        // One group per SoundClass, each sound is created INTO its class's group; AudioMix's class volume is
        // the group's volume and its master the engine's (PushMix).
        std::array<ma_sound_group, kSoundClassCount> Groups{};

        void PushMix()
        {
            const AudioMix& mix = AudioMix::Get();
            ma_engine_set_volume( &Engine, mix.MasterVolume() );
            for ( std::size_t i = 0; i < kSoundClassCount; ++i )
                ma_sound_group_set_volume( &Groups[i], mix.ClassVolume( static_cast<SoundClass>( i ) ) );
        }

        uint32_t                                            NextId = 1;
        std::unordered_map<uint32_t, std::unique_ptr<Sound>> Sounds;

        std::unique_ptr<Sound> LoadSound( const std::string& clipPath, SoundClass soundClass, bool loop,
                                          bool spatial, float volume )
        {
            const auto path = ResolveClipPath( clipPath );
            if ( path.empty() )
            {
                LOG_WARN( "[Audio] Clip not found: {}", clipPath );
                return nullptr;
            }

            auto sound = std::make_unique<Sound>();
            auto bytes = Common::Utils::FileSystem::ReadByteFileContent( path );
            if ( !bytes || bytes.GetValue().empty() )
                return nullptr;
            sound->Bytes = bytes.ExtractValue();

            if ( ma_decoder_init_memory( sound->Bytes.data(), sound->Bytes.size(), nullptr,
                                         &sound->Decoder ) != MA_SUCCESS )
            {
                LOG_WARN( "[Audio] Failed to decode clip: {}", clipPath );
                return nullptr;
            }
            sound->DecoderReady = true;

            const ma_uint32 flags = spatial ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION;
            if ( ma_sound_init_from_data_source( &Engine, &sound->Decoder, flags,
                                                 &Groups[static_cast<std::size_t>( soundClass )],
                                                 &sound->Handle ) != MA_SUCCESS )
            {
                LOG_WARN( "[Audio] Failed to create sound: {}", clipPath );
                return nullptr;
            }
            sound->SoundReady = true;

            ma_sound_set_looping( &sound->Handle, loop ? MA_TRUE : MA_FALSE );
            ma_sound_set_volume( &sound->Handle, volume );
            return sound;
        }
    };

    AudioEngine& AudioEngine::Get()
    {
        static AudioEngine s_Instance;
        return s_Instance;
    }

    AudioEngine::AudioEngine() : m_Impl( std::make_unique<Impl>() )
    {
    }

    AudioEngine::~AudioEngine()
    {
        m_Impl->Sounds.clear(); // sounds must die before their groups, the groups before the engine
        if ( m_Impl->Initialized )
        {
            AudioMix::Get().SetListener( {} );
            for ( ma_sound_group& group : m_Impl->Groups )
                ma_sound_group_uninit( &group );
            ma_engine_uninit( &m_Impl->Engine );
        }
    }

    ma_sound* AudioEngine::GetClassGroup( SoundClass soundClass )
    {
        return EnsureInitialized() ? &m_Impl->Groups[static_cast<std::size_t>( soundClass )] : nullptr;
    }

    ma_engine* AudioEngine::GetNativeEngine()
    {
        return EnsureInitialized() ? &m_Impl->Engine : nullptr;
    }

    bool AudioEngine::EnsureInitialized()
    {
        if ( m_Impl->Initialized )
            return true;
        if ( m_Impl->InitFailed )
            return false;

        if ( ma_engine_init( nullptr, &m_Impl->Engine ) != MA_SUCCESS )
        {
            // Headless machine / CI: log once and stay silent instead of failing the app.
            LOG_WARN( "[Audio] No audio device available — sound disabled." );
            m_Impl->InitFailed = true;
            return false;
        }
        LOG_INFO( "[Audio] miniaudio engine initialized ({} Hz)",
                  ma_engine_get_sample_rate( &m_Impl->Engine ) );
        for ( std::size_t i = 0; i < kSoundClassCount; ++i )
        {
            if ( ma_sound_group_init( &m_Impl->Engine, 0, nullptr, &m_Impl->Groups[i] ) != MA_SUCCESS )
            {
                LOG_ERROR( "[Audio] the sound group of class {} was not created — sound disabled", i );
                for ( std::size_t made = 0; made < i; ++made )
                    ma_sound_group_uninit( &m_Impl->Groups[made] );
                ma_engine_uninit( &m_Impl->Engine );
                m_Impl->InitFailed = true;
                return false;
            }
        }
        m_Impl->Initialized = true;
        // The mix the player set before there was a device is the one it starts with; later changes follow.
        m_Impl->PushMix();
        AudioMix::Get().SetListener( [impl = m_Impl.get()] { impl->PushMix(); } );
        return true;
    }

    void AudioEngine::SetListener( const glm::vec3& position, const glm::vec3& forward,
                                   const glm::vec3& up )
    {
        if ( !m_Impl->Initialized )
            return;
        ma_engine_listener_set_position( &m_Impl->Engine, 0, position.x, position.y, position.z );
        ma_engine_listener_set_direction( &m_Impl->Engine, 0, forward.x, forward.y, forward.z );
        ma_engine_listener_set_world_up( &m_Impl->Engine, 0, up.x, up.y, up.z );
    }

    void AudioEngine::PlayOneShot( const std::string& clipPath, SoundClass soundClass, float volume )
    {
        if ( !EnsureInitialized() )
            return;
        auto sound = m_Impl->LoadSound( clipPath, soundClass, /*loop=*/false, /*spatial=*/false, volume );
        if ( !sound )
            return;
        sound->OneShot = true;
        ma_sound_start( &sound->Handle );
        m_Impl->Sounds.emplace( m_Impl->NextId++, std::move( sound ) );
    }

    uint32_t AudioEngine::CreateSource( const std::string& clipPath, SoundClass soundClass, bool loop, bool spatial,
                                        float volume )
    {
        if ( !EnsureInitialized() )
            return 0;
        auto sound = m_Impl->LoadSound( clipPath, soundClass, loop, spatial, volume );
        if ( !sound )
            return 0;
        const uint32_t id = m_Impl->NextId++;
        m_Impl->Sounds.emplace( id, std::move( sound ) );
        return id;
    }

    void AudioEngine::DestroySource( uint32_t id )
    {
        m_Impl->Sounds.erase( id );
    }

    void AudioEngine::StartSource( uint32_t id )
    {
        if ( auto it = m_Impl->Sounds.find( id ); it != m_Impl->Sounds.end() )
            ma_sound_start( &it->second->Handle );
    }

    void AudioEngine::StopSource( uint32_t id )
    {
        if ( auto it = m_Impl->Sounds.find( id ); it != m_Impl->Sounds.end() )
            ma_sound_stop( &it->second->Handle );
    }

    bool AudioEngine::IsSourcePlaying( uint32_t id ) const
    {
        if ( auto it = m_Impl->Sounds.find( id ); it != m_Impl->Sounds.end() )
            return ma_sound_is_playing( &it->second->Handle ) == MA_TRUE;
        return false;
    }

    void AudioEngine::SetSourcePosition( uint32_t id, const glm::vec3& position )
    {
        if ( auto it = m_Impl->Sounds.find( id ); it != m_Impl->Sounds.end() )
            ma_sound_set_position( &it->second->Handle, position.x, position.y, position.z );
    }

    void AudioEngine::SetSourceVolume( uint32_t id, float volume )
    {
        if ( auto it = m_Impl->Sounds.find( id ); it != m_Impl->Sounds.end() )
            ma_sound_set_volume( &it->second->Handle, volume );
    }

    void AudioEngine::StopAll()
    {
        m_Impl->Sounds.clear();
    }

    void AudioEngine::Update()
    {
        // Reclaim finished one-shots (managed sources are owned by their system until destroyed).
        for ( auto it = m_Impl->Sounds.begin(); it != m_Impl->Sounds.end(); )
        {
            if ( it->second->OneShot && ma_sound_is_playing( &it->second->Handle ) == MA_FALSE )
                it = m_Impl->Sounds.erase( it );
            else
                ++it;
        }
    }
} // namespace Desert::Audio
