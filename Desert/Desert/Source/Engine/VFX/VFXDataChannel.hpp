#pragma once

// VFX-10. A SCENE'S VFX DATA CHANNELS (port of UE Niagara Data Channels, NiagaraDataChannel.h /
// NiagaraDataChannelAccessor.h): gameplay writes entries of a channel's payload (Position, Direction, Color,
// Float, Int fields, the `.dfxch` asset) during a frame; the VFXWorld's next tick hands them to every emitter
// whose EmitterUpdate group holds engine:SpawnFromChannel, then clears them. One UE "global island": a scene has
// one channel per asset, not one per spatial island.
//
// The spawn module (UE "Spawn From Data Channel" read in an emitter's spawn stage): each entry that passes the
// module's filters - within MaxDistance centimetres of the emitter, and a Float/Int field predicate - spawns
// ParticlesPerEntry particles with the payload bound to particle attributes (a Position field to the spawn
// position, a Direction field to the start velocity direction). At most MaxEntriesPerFrame entries spawn per tick;
// the rest are counted as Overflow in the instance's report, never dropped unseen.

#include <Engine/Assets/Serialization/VFXDataChannel.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
    class VFXDataChannelAsset;
} // namespace Desert::Assets

namespace Desert::VFX
{
    /// Where each field of a channel lives in an entry: Offsets[i] floats from the entry's start; Stride floats
    /// per entry (S::FloatCount of every field, in file order).
    struct VFXDataChannelLayout
    {
        std::vector<Assets::Serialization::VFXDataChannelField> Fields;
        std::vector<std::uint32_t>                              Offsets;
        std::uint32_t                                           Stride = 0;

        [[nodiscard]] static VFXDataChannelLayout From( const Assets::Serialization::VFXDataChannelData& data );
        /// The index of the field named @p name, or -1.
        [[nodiscard]] int Find( std::string_view name ) const;
    };

    /// One channel of a scene: its layout and the entries written since the last VFXWorld tick.
    class VFXDataChannel
    {
    public:
        explicit VFXDataChannel( VFXDataChannelLayout layout ) : m_Layout( std::move( layout ) )
        {
        }

        [[nodiscard]] const VFXDataChannelLayout& Layout() const
        {
            return m_Layout;
        }
        [[nodiscard]] std::size_t EntryCount() const
        {
            return m_Layout.Stride == 0 ? 0 : m_Entries.size() / m_Layout.Stride;
        }
        [[nodiscard]] std::span<const float> Entry( std::size_t index ) const
        {
            return { m_Entries.data() + index * m_Layout.Stride, m_Layout.Stride };
        }

        /// Appends @p count zeroed entries; returns the index of the first.
        std::size_t Append( std::size_t count );
        /// Writes field @p field of entry @p entry, refusing a field of another type or an entry out of range.
        Common::BoolResultStr Set( std::size_t entry, std::string_view field,
                                   Assets::Serialization::VFXDataChannelFieldType type, const glm::vec4& value );
        void                  ClearEntries()
        {
            m_Entries.clear();
        }

    private:
        VFXDataChannelLayout m_Layout;
        std::vector<float>   m_Entries;
    };

    /// UE UNiagaraDataChannelWriter: a window of @p count entries appended to one channel this frame, written
    /// field by field. Valid until the channel's entries are cleared (the next VFXWorld tick).
    class VFXDataChannelWriter
    {
    public:
        VFXDataChannelWriter() = default;
        VFXDataChannelWriter( VFXDataChannel& channel, std::size_t first, std::size_t count )
             : m_Channel( &channel ), m_First( first ), m_Count( count )
        {
        }

        [[nodiscard]] std::size_t Count() const
        {
            return m_Count;
        }

        Common::BoolResultStr WritePosition( std::size_t index, std::string_view field, const glm::vec3& cm );
        Common::BoolResultStr WriteDirection( std::size_t index, std::string_view field,
                                              const glm::vec3& direction );
        Common::BoolResultStr WriteColor( std::size_t index, std::string_view field, const glm::vec4& rgba );
        Common::BoolResultStr WriteFloat( std::size_t index, std::string_view field, float value );
        Common::BoolResultStr WriteInt( std::size_t index, std::string_view field, std::int32_t value );

    private:
        Common::BoolResultStr Write( std::size_t index, std::string_view field,
                                     Assets::Serialization::VFXDataChannelFieldType type, const glm::vec4& value );

        VFXDataChannel* m_Channel = nullptr;
        std::size_t     m_First   = 0;
        std::size_t     m_Count   = 0;
    };

    /// The channels of one scene (owned by its VFXWorld), keyed by channel name (the asset's file stem).
    class VFXDataChannels
    {
    public:
        /// Makes channel @p name of @p data's layout. Registering a name again with the same layout keeps its
        /// entries; with another layout it is refused while entries are pending, else replaced.
        Common::BoolResultStr Register( const std::string&                               name,
                                        const Assets::Serialization::VFXDataChannelData& data );
        /// The channel of a loaded `.dfxch` asset, named by its file stem (the asset's name).
        Common::BoolResultStr Register( const Assets::VFXDataChannelAsset& asset );
        /// What C++ gameplay calls (UE: a UNiagaraDataChannelAsset reference): the channel asset @p handle names
        /// in @p assets; refused when no ready VFXDataChannelAsset has that handle.
        Common::BoolResultStr Register( const Common::AssetHandle& handle, const Assets::AssetManager& assets );

        [[nodiscard]] const VFXDataChannel* Find( std::string_view name ) const;

        /// Appends @p count entries to channel @p name for this frame (refused for an unregistered name).
        Common::ResultStr<VFXDataChannelWriter> Write( std::string_view name, std::size_t count );

        /// The end of a VFXWorld tick: every channel's entries are consumed.
        void ClearEntries();
        /// The scene is emptied: every channel is forgotten.
        void Clear()
        {
            m_Channels.clear();
        }

    private:
        std::map<std::string, VFXDataChannel, std::less<>> m_Channels;
    };

    /// The field predicate of a Spawn from Channel module: `field <Op> Value`.
    enum class VFXChannelFilterOp : std::uint32_t
    {
        Less,
        LessEqual,
        Greater,
        GreaterEqual,
        Equal,
        NotEqual,
    };
    inline constexpr std::uint32_t kVFXChannelFilterOpCount = 6;

    /// engine:SpawnFromChannel as the CPU reads it (VFXEmitterSpawn CompileSpawnPlan).
    struct VFXChannelSpawnModule
    {
        std::string        Channel;
        std::uint32_t      ParticlesPerEntry  = 1;
        std::uint32_t      MaxEntriesPerFrame = 1;
        float              MaxDistance        = 0.0f; ///< centimetres from the emitter; 0 = no distance filter
        std::string        PositionField;  ///< a Position field -> spawn position; empty = the emitter's
        std::string        DirectionField; ///< a Direction field -> start velocity direction; empty = cone
        std::string        FilterField;    ///< a Float or Int field; empty = no predicate
        std::string        ColorField;     ///< a Color field -> the particle's Tint (multiplies colour over life)
        std::string        LifetimeField;  ///< a Float or Int field -> the particle's lifetime in seconds
        VFXChannelFilterOp FilterOp    = VFXChannelFilterOp::Greater;
        float              FilterValue = 0.0f;

        [[nodiscard]] bool operator==( const VFXChannelSpawnModule& ) const = default;
    };

    /// One entry that spawns: Count particles with its bound payload.
    struct VFXChannelSpawnRequest
    {
        std::uint32_t Entry        = 0;
        std::uint32_t Count        = 0;
        bool          HasPosition  = false;
        glm::vec3     Position     = glm::vec3( 0.0f ); ///< world centimetres
        bool          HasDirection = false;
        glm::vec3     Direction    = glm::vec3( 0.0f );
        bool          HasColor     = false;
        glm::vec4     Color        = glm::vec4( 1.0f ); ///< linear rgba
        bool          HasLifetime  = false;
        float         Lifetime     = 0.0f; ///< seconds
    };

    /// What one tick did with a channel's entries for one emitter. Read = Spawned + Filtered + Overflow.
    struct VFXChannelSpawnReport
    {
        std::uint32_t Read     = 0;
        std::uint32_t Spawned  = 0; ///< entries that spawn
        std::uint32_t Filtered = 0; ///< refused by the distance filter or the predicate
        std::uint32_t Overflow = 0; ///< passed the filters beyond MaxEntriesPerFrame (or no step ran this tick)
        bool          ChannelMissing = false;
    };

    struct VFXChannelSpawnBatch
    {
        std::vector<VFXChannelSpawnRequest> Requests;
        VFXChannelSpawnReport               Report;

        [[nodiscard]] std::uint32_t ParticleCount() const;
    };

    /// Checks @p module's fields against @p layout: each named field exists and has the type its role needs
    /// (Position / Direction / Color / Float or Int), and a distance filter has a Position field to measure.
    Common::BoolResultStr BindChannelSpawn( const VFXChannelSpawnModule& module,
                                            const VFXDataChannelLayout&  layout );

    /// The spawns @p module makes from this frame's entries of @p channel for an emitter at @p emitterCm. A
    /// missing channel or one the module does not bind spawns nothing and says so in the report.
    [[nodiscard]] VFXChannelSpawnBatch GatherChannelSpawns( const VFXChannelSpawnModule& module,
                                                            const VFXDataChannel*        channel,
                                                            const glm::vec3&             emitterCm );
} // namespace Desert::VFX
