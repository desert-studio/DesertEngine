#pragma once

#include <Engine/Media/MediaPlayer.hpp>

#include <memory>

namespace Desert::Media
{
    // The MediaPlayer's sound, into the engine's miniaudio mix (Audio::AudioEngine): a streaming data
    // source the device thread pulls from. PlayedFrames counts what the DEVICE has consumed — that count is
    // the player's master clock, so an underrun (silence) stalls the picture instead of letting it run away.
    class MediaAudioOutput final : public IMediaAudioSink
    {
    public:
        MediaAudioOutput();
        ~MediaAudioOutput() override;
        MediaAudioOutput( const MediaAudioOutput& )            = delete;
        MediaAudioOutput& operator=( const MediaAudioOutput& ) = delete;

        std::string            Start( uint32_t sampleRate, uint32_t channels ) override;
        void                   Push( const float* interleaved, uint64_t frames ) override;
        [[nodiscard]] uint64_t PlayedFrames() const override;
        [[nodiscard]] uint64_t QueuedFrames() const override;
        void                   SetPaused( bool paused ) override;
        void                   Flush() override;

        void SetVolume( float volume );

        struct Impl;

    private:
        std::unique_ptr<Impl> m_Impl;
    };
} // namespace Desert::Media
