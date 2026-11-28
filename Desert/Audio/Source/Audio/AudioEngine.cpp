#include "AudioEngine.hpp"
#include "AudioMix.hpp"

#include <Common/Content/AssetGuid.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <miniaudio/miniaudio.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Desert::Audio
{
    namespace
    {
        // The file a clip reference names: a content key ("assets:Audio/Hit.wav") expanded under its root, or
        // an absolute path (what SoundAsset::Resolve hands over). A bare relative spelling is refused by name
        // inside PathForStableKey rather than tried against the working directory and then against the assets
        // root (PATH1: one reader, no guessed root). Exists is the VFS-aware one, so a packaged game resolves
        // into the mounted .dpak.
        std::filesystem::path ResolveClipPath( const std::string& clipPath )
        {
            const std::filesystem::path file = Common::AssetHandle::PathForStableKey( clipPath );
            if ( file.empty() || !Common::Utils::FileSystem::Exists( file ) )
                return {};
            return file;
        }

        // ---- THE CONTENT VFS: miniaudio's resource manager reads every clip through FileSystem ----------------
        // UE reads a wave's bulk data through the package file system (IFileHandle over the pak); ours is
        // FileSystem, which serves loose files and mounted .dpak entries alike. It reads a file whole, so an open
        // file is its bytes plus a cursor. The resource manager calls these on ITS job thread
        // (MA_SOUND_FLAG_ASYNC), which is the point: the read and the decode leave the game thread. Each open file
        // is its own object; nothing here is shared between two opens.
        struct ContentFile
        {
            std::vector<uint8_t> Bytes;
            std::size_t          Cursor = 0;
        };

        ContentFile* FileOf( ma_vfs_file file )
        {
            return static_cast<ContentFile*>( file );
        }

        // miniaudio hands the path over as the UTF-8 the engine gave it (VoiceFor below), on every platform.
        ma_result ContentOpen( ma_vfs* /*vfs*/, const char* filePath, ma_uint32 openMode, ma_vfs_file* file )
        {
            if ( filePath == nullptr || file == nullptr )
                return MA_INVALID_ARGS;
            *file = nullptr;
            if ( ( openMode & MA_OPEN_MODE_WRITE ) != 0 )
                return MA_ACCESS_DENIED; // content is read-only; the audio engine writes no file
            const std::string_view      utf8( filePath );
            const std::filesystem::path path( std::u8string( utf8.begin(), utf8.end() ) );
            auto                        bytes = Common::Utils::FileSystem::ReadByteFileContent( path );
            if ( !bytes )
                return MA_DOES_NOT_EXIST;
            auto opened   = std::make_unique<ContentFile>();
            opened->Bytes = bytes.ExtractValue();
            *file         = opened.release(); // owned by miniaudio until ContentClose
            return MA_SUCCESS;
        }

        ma_result ContentOpenWide( ma_vfs* /*vfs*/, const wchar_t* /*filePath*/, ma_uint32 /*openMode*/,
                                   ma_vfs_file* /*file*/ )
        {
            return MA_NOT_IMPLEMENTED; // the engine opens by UTF-8 only (ma_sound_init_from_file, not _w)
        }

        ma_result ContentClose( ma_vfs* /*vfs*/, ma_vfs_file file )
        {
            const std::unique_ptr<ContentFile> closed( FileOf( file ) );
            return MA_SUCCESS;
        }

        ma_result ContentRead( ma_vfs* /*vfs*/, ma_vfs_file file, void* destination, size_t sizeInBytes,
                               size_t* bytesRead )
        {
            ContentFile* const open  = FileOf( file );
            const std::size_t  left  = open->Bytes.size() - open->Cursor;
            const std::size_t  count = std::min( left, sizeInBytes );
            if ( count > 0 )
                std::memcpy( destination, open->Bytes.data() + open->Cursor, count );
            open->Cursor += count;
            if ( bytesRead != nullptr )
                *bytesRead = count;
            return ( count == 0 && sizeInBytes > 0 ) ? MA_AT_END : MA_SUCCESS;
        }

        ma_result ContentWrite( ma_vfs* /*vfs*/, ma_vfs_file /*file*/, const void* /*source*/,
                                size_t /*sizeInBytes*/, size_t* /*bytesWritten*/ )
        {
            return MA_ACCESS_DENIED;
        }

        ma_result ContentSeek( ma_vfs* /*vfs*/, ma_vfs_file file, ma_int64 offset, ma_seek_origin origin )
        {
            ContentFile* const open = FileOf( file );
            const auto         size = static_cast<ma_int64>( open->Bytes.size() );
            ma_int64           base = 0;
            if ( origin == ma_seek_origin_current )
                base = static_cast<ma_int64>( open->Cursor );
            else if ( origin == ma_seek_origin_end )
                base = size;
            const ma_int64 target = base + offset;
            if ( target < 0 || target > size )
                return MA_BAD_SEEK;
            open->Cursor = static_cast<std::size_t>( target );
            return MA_SUCCESS;
        }

        ma_result ContentTell( ma_vfs* /*vfs*/, ma_vfs_file file, ma_int64* cursor )
        {
            if ( cursor == nullptr )
                return MA_INVALID_ARGS;
            *cursor = static_cast<ma_int64>( FileOf( file )->Cursor );
            return MA_SUCCESS;
        }

        ma_result ContentInfo( ma_vfs* /*vfs*/, ma_vfs_file file, ma_file_info* info )
        {
            if ( info == nullptr )
                return MA_INVALID_ARGS;
            info->sizeInBytes = static_cast<ma_uint64>( FileOf( file )->Bytes.size() );
            return MA_SUCCESS;
        }

        // A custom miniaudio VFS is any object whose first member is the callback table.
        struct ContentVfs
        {
            ma_vfs_callbacks Callbacks{};
        };
    } // namespace

    // A playing sound: miniaudio's sound over a resource-manager data source (a shared decoded buffer or a
    // stream of its own). `Loading` is true until the job thread has opened and decoded the clip, so a
    // failure is reported once, by name (Update).
    struct Voice
    {
        ma_sound    Sound{};
        std::string Clip;
        bool        Ready   = false;
        bool        OneShot = false;
        bool        Loading = true;

        Voice()                          = default;
        Voice( const Voice& )            = delete;
        Voice& operator=( const Voice& ) = delete;
        ~Voice()
        {
            if ( Ready )
                ma_sound_uninit( &Sound );
        }
    };

    struct AudioEngine::Impl
    {
        ContentVfs Vfs;
        ma_engine  Engine{};
        bool       Initialized = false;
        bool       InitFailed  = false;
        // One group per class of AudioMix::Classes(), at the same index (parents first), each output into its
        // parent's group (a root into the endpoint); every sound is created INTO its class's group. A group's
        // volume is the class's authored volume times the player's, its pitch the authored one, and the node
        // graph multiplies both down the hierarchy; the master is the engine's (PushMix). Heap-held: a group is
        // a graph node other nodes point at, so it never moves while it lives. Empty when the last build failed.
        std::vector<std::unique_ptr<ma_sound_group>> Groups;

        // THE VOICE TABLE: slot index + generation (VoiceHandle). A released slot's generation moves on and
        // the slot is reused, so the table never grows past the most voices alive at once.
        struct Slot
        {
            uint32_t               Generation = 1;
            std::unique_ptr<Voice> Playing;
        };
        std::vector<Slot>     Slots;
        std::vector<uint32_t> FreeSlots;

        Impl()
        {
            Vfs.Callbacks.onOpen  = &ContentOpen;
            Vfs.Callbacks.onOpenW = &ContentOpenWide;
            Vfs.Callbacks.onClose = &ContentClose;
            Vfs.Callbacks.onRead  = &ContentRead;
            Vfs.Callbacks.onWrite = &ContentWrite;
            Vfs.Callbacks.onSeek  = &ContentSeek;
            Vfs.Callbacks.onTell  = &ContentTell;
            Vfs.Callbacks.onInfo  = &ContentInfo;
        }

        void PushMix()
        {
            const AudioMix& mix = AudioMix::Get();
            ma_engine_set_volume( &Engine, mix.MasterVolume() );
            const std::span<const SoundClassDesc> classes = mix.Classes();
            for ( std::size_t i = 0; i < Groups.size(); ++i )
            {
                ma_sound_group_set_volume( Groups[i].get(), classes[i].Volume * mix.UserVolume( classes[i].Id ) );
                ma_sound_group_set_pitch( Groups[i].get(), classes[i].Pitch );
            }
        }

        // Children before parents, so no group is torn down while a live one still outputs into it.
        void DestroyGroups()
        {
            for ( auto group = Groups.rbegin(); group != Groups.rend(); ++group )
                ma_sound_group_uninit( group->get() );
            Groups.clear();
        }

        // The groups of the current class set, parent first. A failure leaves no group and names the class:
        // every voice is then refused by VoiceFor rather than played outside its class.
        [[nodiscard]] bool BuildGroups()
        {
            const std::span<const SoundClassDesc> classes = AudioMix::Get().Classes();
            Groups.reserve( classes.size() );
            for ( const SoundClassDesc& desc : classes )
            {
                ma_sound_group* parent = nullptr;
                if ( !desc.Parent.IsNull() )
                    parent = GroupOf( desc.Parent ); // sorted parents first: always built already
                // A group is a mix bus, not an emitter: miniaudio spatializes a group by default (at the origin),
                // which a nested hierarchy would apply once per level. The voice alone is placed in the world.
                auto group = std::make_unique<ma_sound_group>();
                if ( ma_sound_group_init( &Engine, MA_SOUND_FLAG_NO_SPATIALIZATION, parent, group.get() ) !=
                     MA_SUCCESS )
                {
                    LOG_ERROR( "[Audio] the sound group of class {} was not created — no class plays",
                               Common::Content::AssetGuidToText( desc.Id ) );
                    DestroyGroups();
                    return false;
                }
                Groups.push_back( std::move( group ) );
            }
            LOG_INFO( "[Audio] sound class groups built: {} classes, {} B per group (sizeof ma_sound_group)",
                      Groups.size(), sizeof( ma_sound_group ) );
            PushMix();
            return true;
        }

        // The group of @p id: its index in AudioMix::Classes(). nullptr when the set does not hold it or the
        // last build failed.
        [[nodiscard]] ma_sound_group* GroupOf( const SoundClassId& id ) const
        {
            const std::span<const SoundClassDesc> classes = AudioMix::Get().Classes();
            for ( std::size_t i = 0; i < classes.size() && i < Groups.size(); ++i )
                if ( classes[i].Id == id )
                    return Groups[i].get();
            return nullptr;
        }

        // The one listener of AudioMix. A new class set replaces the graph: every voice plays into a group
        // that is about to die, so all are stopped first (AudioEngine::StopAll), then the groups are rebuilt.
        void OnMixChanged( MixChange change )
        {
            if ( change == MixChange::Volumes )
            {
                PushMix();
                return;
            }
            ReleaseAll();
            DestroyGroups();
            if ( !BuildGroups() )
                LOG_ERROR( "[Audio] the mix of version {} has no groups: every voice is refused until the next "
                           "class set",
                           AudioMix::Get().Version() );
        }

        [[nodiscard]] Voice* Find( VoiceHandle handle ) const
        {
            if ( !handle.IsValid() || handle.Index >= Slots.size() )
                return nullptr;
            const Slot& slot = Slots[handle.Index];
            if ( slot.Generation != handle.Generation )
                return nullptr;
            return slot.Playing.get();
        }

        VoiceHandle Insert( std::unique_ptr<Voice> voice )
        {
            uint32_t index = 0;
            if ( !FreeSlots.empty() )
            {
                index = FreeSlots.back();
                FreeSlots.pop_back();
            }
            else
            {
                index = static_cast<uint32_t>( Slots.size() );
                Slots.emplace_back();
            }
            Slots[index].Playing = std::move( voice );
            return VoiceHandle{ index, Slots[index].Generation };
        }

        void Release( uint32_t index )
        {
            Slot& slot = Slots[index];
            if ( !slot.Playing )
                return;
            slot.Playing.reset();
            // 0 is the invalid generation; a slot that wrapped starts over at 1.
            slot.Generation = slot.Generation == std::numeric_limits<uint32_t>::max() ? 1 : slot.Generation + 1;
            FreeSlots.push_back( index );
        }

        void ReleaseAll()
        {
            for ( uint32_t index = 0; index < Slots.size(); ++index )
                Release( index );
        }

        // A voice of @p source, loading on the resource manager's job thread. Resident: MA_SOUND_FLAG_DECODE —
        // the decoded buffer is keyed by the file and shared by every voice of it while any lives. Streamed:
        // MA_SOUND_FLAG_STREAM — paged decode, a stream per voice.
        std::unique_ptr<Voice> VoiceFor( const SoundSource& source, bool loop, bool spatial, float volume )
        {
            ma_sound_group* const group = GroupOf( source.Class );
            if ( group == nullptr )
            {
                LOG_WARN( "[Audio] {}: sound class {} is not in the mix — not played", source.Clip,
                          Common::Content::AssetGuidToText( source.Class ) );
                return nullptr;
            }
            const auto path = ResolveClipPath( source.Clip );
            if ( path.empty() )
            {
                LOG_WARN( "[Audio] Clip not found: {}", source.Clip );
                return nullptr;
            }

            ma_uint32 flags = MA_SOUND_FLAG_ASYNC;
            flags |= source.Loading == SoundLoading::Streamed ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
            if ( !spatial )
                flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;

            const std::u8string utf8Path = path.u8string();
            const std::string   utf8( utf8Path.begin(), utf8Path.end() );

            auto voice  = std::make_unique<Voice>();
            voice->Clip = source.Clip;
            if ( ma_sound_init_from_file( &Engine, utf8.c_str(), flags, group, nullptr, &voice->Sound ) !=
                 MA_SUCCESS )
            {
                LOG_WARN( "[Audio] Failed to create a voice of {} ({})", source.Clip,
                          SoundLoadingName( source.Loading ) );
                return nullptr;
            }
            voice->Ready = true;

            ma_sound_set_looping( &voice->Sound, loop ? MA_TRUE : MA_FALSE );
            ma_sound_set_volume( &voice->Sound, volume );
            return voice;
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
        m_Impl->ReleaseAll(); // voices must die before their groups, the groups before the engine
        if ( m_Impl->Initialized )
        {
            AudioMix::Get().SetListener( {} );
            m_Impl->DestroyGroups();
            ma_engine_uninit( &m_Impl->Engine );
        }
    }

    ma_sound* AudioEngine::GetClassGroup( const SoundClassId& soundClass )
    {
        if ( !EnsureInitialized() )
            return nullptr;
        ma_sound_group* const group = m_Impl->GroupOf( soundClass );
        if ( group == nullptr )
            LOG_WARN( "[Audio] a producer asked for sound class {}, which is not in the mix",
                      Common::Content::AssetGuidToText( soundClass ) );
        return group;
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

        // The engine owns its resource manager (decoded to f32 at the device rate, one job thread) and reads
        // through the content VFS. The default device is followed when the OS switches it (miniaudio's
        // automatic stream routing on CoreAudio and WASAPI, UE's AudioDeviceNotificationSubsystem).
        ma_engine_config config    = ma_engine_config_init();
        config.pResourceManagerVFS = &m_Impl->Vfs;
        if ( ma_engine_init( &config, &m_Impl->Engine ) != MA_SUCCESS )
        {
            // Headless machine / CI: log once and stay silent instead of failing the app.
            LOG_WARN( "[Audio] No audio device available — sound disabled." );
            m_Impl->InitFailed = true;
            return false;
        }
        LOG_INFO( "[Audio] miniaudio engine initialized ({} Hz)",
                  ma_engine_get_sample_rate( &m_Impl->Engine ) );
        // The class set and the mix the player set before there was a device are the ones it starts with; later
        // changes follow through the listener (a new set rebuilds, a volume is pushed).
        if ( !m_Impl->BuildGroups() )
        {
            ma_engine_uninit( &m_Impl->Engine );
            m_Impl->InitFailed = true;
            return false;
        }
        m_Impl->Initialized = true;
        AudioMix::Get().SetListener( [impl = m_Impl.get()]( MixChange change ) { impl->OnMixChanged( change ); } );
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

    void AudioEngine::PlayOneShot( const SoundSource& source, float volume )
    {
        if ( !EnsureInitialized() )
            return;
        auto voice = m_Impl->VoiceFor( source, /*loop=*/false, /*spatial=*/false, volume );
        if ( !voice )
            return;
        voice->OneShot = true;
        ma_sound_start( &voice->Sound );
        static_cast<void>( m_Impl->Insert( std::move( voice ) ) ); // reclaimed by Update when it ends
    }

    VoiceHandle AudioEngine::CreateVoice( const SoundSource& source, bool loop, bool spatial, float volume )
    {
        if ( !EnsureInitialized() )
            return {};
        auto voice = m_Impl->VoiceFor( source, loop, spatial, volume );
        if ( !voice )
            return {};
        return m_Impl->Insert( std::move( voice ) );
    }

    void AudioEngine::DestroyVoice( VoiceHandle voice )
    {
        if ( m_Impl->Find( voice ) != nullptr )
            m_Impl->Release( voice.Index );
    }

    void AudioEngine::StartVoice( VoiceHandle voice )
    {
        if ( Voice* playing = m_Impl->Find( voice ) )
            ma_sound_start( &playing->Sound );
    }

    void AudioEngine::StopVoice( VoiceHandle voice )
    {
        if ( Voice* playing = m_Impl->Find( voice ) )
            ma_sound_stop( &playing->Sound );
    }

    bool AudioEngine::IsVoicePlaying( VoiceHandle voice ) const
    {
        if ( const Voice* playing = m_Impl->Find( voice ) )
            return ma_sound_is_playing( &playing->Sound ) == MA_TRUE;
        return false;
    }

    void AudioEngine::SeekVoice( VoiceHandle voice, double seconds )
    {
        Voice* playing = m_Impl->Find( voice );
        if ( playing == nullptr )
            return;
        // The resource manager decodes every clip to the device rate (ma_engine_init: decodedSampleRate), so a
        // second is that many frames whether or not the clip has finished loading — a seek before it has is
        // kept by the data source and applied on its first read.
        const double frame =
             std::max( seconds, 0.0 ) * static_cast<double>( ma_engine_get_sample_rate( &m_Impl->Engine ) );
        ma_sound_seek_to_pcm_frame( &playing->Sound, static_cast<ma_uint64>( frame ) );
    }

    void AudioEngine::SetVoicePosition( VoiceHandle voice, const glm::vec3& position )
    {
        if ( Voice* playing = m_Impl->Find( voice ) )
            ma_sound_set_position( &playing->Sound, position.x, position.y, position.z );
    }

    void AudioEngine::SetVoiceVolume( VoiceHandle voice, float volume )
    {
        if ( Voice* playing = m_Impl->Find( voice ) )
            ma_sound_set_volume( &playing->Sound, volume );
    }

    void AudioEngine::StopAll()
    {
        m_Impl->ReleaseAll();
    }

    void AudioEngine::Update()
    {
        for ( uint32_t index = 0; index < m_Impl->Slots.size(); ++index )
        {
            Voice* const voice = m_Impl->Slots[index].Playing.get();
            if ( voice == nullptr )
                continue;
            // A clip that finished loading: a failure (unreadable file, an encoding no decoder knows) is
            // reported once, by name; a managed voice stays silent until its owner destroys it.
            bool failed = false;
            if ( voice->Loading )
            {
                const ma_result loaded = ma_resource_manager_data_source_result(
                     static_cast<ma_resource_manager_data_source*>( ma_sound_get_data_source( &voice->Sound ) ) );
                if ( loaded != MA_BUSY )
                {
                    voice->Loading = false;
                    failed         = loaded != MA_SUCCESS;
                    if ( failed )
                        LOG_ERROR( "[Audio] {} could not be loaded: {}", voice->Clip,
                                   ma_result_description( loaded ) );
                }
            }
            // Reclaim finished one-shots, and one-shots that will never sound (managed voices are owned by
            // their system until destroyed). A one-shot still loading is started, not finished.
            if ( voice->OneShot &&
                 ( failed || ( !voice->Loading && ma_sound_is_playing( &voice->Sound ) == MA_FALSE ) ) )
                m_Impl->Release( index );
        }
    }
} // namespace Desert::Audio
