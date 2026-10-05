#include "AudioEngine.hpp"

#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <miniaudio/miniaudio.h>

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

        uint32_t                                            NextId = 1;
        std::unordered_map<uint32_t, std::unique_ptr<Sound>> Sounds;

        std::unique_ptr<Sound> LoadSound( const std::string& clipPath, bool loop, bool spatial,
                                          float volume )
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
            if ( ma_sound_init_from_data_source( &Engine, &sound->Decoder, flags, nullptr,
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
        m_Impl->Sounds.clear(); // sounds must die before the engine
        if ( m_Impl->Initialized )
            ma_engine_uninit( &m_Impl->Engine );
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
        m_Impl->Initialized = true;
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

    void AudioEngine::PlayOneShot( const std::string& clipPath, float volume )
    {
        if ( !EnsureInitialized() )
            return;
        auto sound = m_Impl->LoadSound( clipPath, /*loop=*/false, /*spatial=*/false, volume );
        if ( !sound )
            return;
        sound->OneShot = true;
        ma_sound_start( &sound->Handle );
        m_Impl->Sounds.emplace( m_Impl->NextId++, std::move( sound ) );
    }

    uint32_t AudioEngine::CreateSource( const std::string& clipPath, bool loop, bool spatial,
                                        float volume )
    {
        if ( !EnsureInitialized() )
            return 0;
        auto sound = m_Impl->LoadSound( clipPath, loop, spatial, volume );
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
