#pragma once

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace Desert::Audio
{
    // A SOUND CLASS IS CONTENT (UE USoundClass): the mix knows a class only by the GUID of the asset that
    // states it. The module names no class; which classes exist is what the project's assets say.
    using SoundClassId = Common::Content::AssetGuid;

    // What one class asset states. Parent null = a root class (its group hangs off the engine's endpoint).
    struct SoundClassDesc
    {
        SoundClassId Id;
        SoundClassId Parent;
        float        Volume = 1.0f; // authored, 0..1+; multiplies down the hierarchy
        float        Pitch  = 1.0f; // authored, > 0; multiplies down the hierarchy

        bool operator==( const SoundClassDesc& ) const = default;
    };

    // What changed, so the one listener rebuilds groups only when the class set did.
    enum class MixChange : uint8_t
    {
        Volumes, // master or a user volume: group volumes are pushed, nothing is rebuilt
        Classes, // the class set / hierarchy / authored numbers: groups are rebuilt, every voice stopped
    };

    // THE MIX — the class hierarchy the project's assets state, plus the volumes the player asked for (master
    // and per class, keyed by the class asset). One owner of the numbers; the engine layer writes them
    // (SoundClassRegistry: SetClasses; GameUserSettings::Apply: the volumes), AudioEngine hears every change
    // through its one listener. Exists without an audio device.
    class AudioMix
    {
    public:
        static AudioMix& Get();

        // Replaces the whole class set. Refused, with nothing changed: a null or repeated Id, a Parent that is
        // not in the set, a cycle, a Volume < 0 or a Pitch <= 0 — each naming the class GUID. The player's
        // volumes are not touched (they are keyed by Id, not by the set). Version() moves.
        [[nodiscard]] Common::BoolResultStr SetClasses( std::span<const SoundClassDesc> classes );

        // Parents before children; the order groups are created in.
        [[nodiscard]] std::span<const SoundClassDesc> Classes() const;
        [[nodiscard]] bool                             Has( const SoundClassId& id ) const;

        // Moves on every accepted SetClasses; 0 = no class set was ever given.
        [[nodiscard]] uint64_t Version() const;

        // 0 = silent, 1 = as authored.
        void                SetMasterVolume( float volume );
        [[nodiscard]] float MasterVolume() const;

        // The player's volume of one class (multiplies the authored one), KEYED BY THE CLASS ASSET AND KEPT
        // WHETHER OR NOT THE SET HOLDS IT: the player's settings are applied before the host has indexed its
        // content (Runtime/Source/Main.cpp applies them between the layer's construction and its attach; the
        // class set arrives in the attach), and a hot reload replaces the set without losing them. A null Id
        // is refused.
        [[nodiscard]] Common::BoolResultStr SetUserVolume( const SoundClassId& id, float volume );
        // 1 for a class the player never set.
        [[nodiscard]] float UserVolume( const SoundClassId& id ) const;
        // The Ids the player set a volume for that the current set does not hold - what SoundClassRegistry
        // reports (one line each, with the GUID) after an accepted SetClasses. Empty before the first set.
        [[nodiscard]] std::vector<SoundClassId> UnmatchedUserVolumes() const;

        // The one listener: AudioEngine, once its device exists. Called after every accepted change.
        void SetListener( std::function<void( MixChange )> listener );

    private:
        void Changed( MixChange change ) const;

        float                             m_Master = 1.0f;
        std::vector<SoundClassDesc>       m_Classes;     // topologically sorted
        std::vector<std::pair<SoundClassId, float>> m_UserVolumes; // one entry per Id the player set
        uint64_t                          m_Version = 0;
        std::function<void( MixChange )>  m_Listener;
    };
} // namespace Desert::Audio
