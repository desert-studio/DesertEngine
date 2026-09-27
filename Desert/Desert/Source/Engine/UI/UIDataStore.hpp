#pragma once

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace Desert::UI
{
    // MVVM-lite: a flat key -> value store the UI binds to. Gameplay (C++ or Lua) writes "player.hp" and
    // every element bound to that key follows on the next frame — no element lookups, no per-widget glue,
    // and the writer never needs to know a UI exists.
    //
    // Deliberately global and flat: a UI binding is a name, and a scene has one screen. Values are copied
    // out on read, so a binding can never dangle.
    class UICollection;

    class UIDataStore
    {
    public:
        using Value = std::variant<double, bool, std::string, glm::vec3>;

        static UIDataStore& Get();

        // Copying copies the collections too (a record is itself a store, and Locals are copied with their
        // cell): declared here, defined where UICollection is complete.
        UIDataStore();
        UIDataStore( const UIDataStore& other );
        UIDataStore( UIDataStore&& other ) noexcept;
        UIDataStore& operator=( const UIDataStore& other );
        UIDataStore& operator=( UIDataStore&& other ) noexcept;
        ~UIDataStore();

        void Set( const std::string& key, Value value );
        void Erase( const std::string& key );
        void Clear(); // scene change / play-stop: bindings must not survive into another world

        bool Has( const std::string& key ) const;

        // Typed reads. Each converts when it sensibly can (a number reads as text, "1"/"true" reads as a
        // bool) and returns nothing when it cannot, so a mistyped binding shows the authored value rather
        // than a garbage one.
        std::optional<double>      Number( const std::string& key ) const;
        std::optional<bool>        Bool( const std::string& key ) const;
        std::optional<std::string> Text( const std::string& key ) const;
        std::optional<glm::vec3>   Color( const std::string& key ) const;

        const std::unordered_map<std::string, Value>& All() const
        {
            return m_Values;
        }

        // --- Collections (UIL1) ---------------------------------------------------------------------
        // A NAMED ORDERED LIST OF RECORDS, beside the flat values and not inside the variant: a Value is
        // copied out on every read, and a ten-thousand-row inventory copied out per frame is exactly what
        // a list bound to it must not cost. A collection is read in place, by index, and only the rows a
        // UIListView's window covers are ever asked for.
        //
        // Lives in the same store because it has the same lifetime question (Clear() on scene change) and
        // the same writer (gameplay via ui.list_*), and a second singleton would be a second place to
        // forget to clear.
        UICollection&       Collection( const std::string& key ); // created empty on first use
        const UICollection* FindCollection( const std::string& key ) const;
        void                EraseCollection( const std::string& key );

    private:
        std::unordered_map<std::string, Value> m_Values;
        // Held by pointer so a reference handed out by Collection() survives the map rehashing when a
        // second collection is created while the first is being filled.
        std::unordered_map<std::string, std::unique_ptr<UICollection>> m_Collections;
    };

    // An ordered list of records; a record is a UIDataStore of named fields, so a row's binding reads a
    // field through exactly the conversions (number -> text, "true" -> bool) a flat key already has.
    //
    // THREE COUNTERS, each answering a different consumer's question without walking the records:
    //   * Id       — stable per record across inserts and removes above it, the UE "item object"
    //                identity. Index is where a record IS; Id is WHICH record it is.
    //   * Revision — per record, bumped by every field write to it. A row whose (Id, Revision) is what it
    //                was last frame shows the same data.
    //   * Generation — per collection, bumped by every mutation of any kind, and stamped on the change log
    //                below, so a list can ask "what changed since I last looked" in O(changes).
    class UICollection
    {
    public:
        // One mutation, as a RANGE of indices [First, First + Count) — the shape UE's list views and every
        // retained list model notify in, and the one a list needs to keep its scroll anchored: an insert
        // above the window moves the window, one below it does not.
        struct Change
        {
            enum class Kind : std::uint8_t
            {
                Set,    // fields of existing records changed; indices did not move
                Insert, // Count records now occupy [First, First + Count); everything after moved down
                Remove, // Count records that were at [First, First + Count) are gone; everything after moved up
                Clear   // every record is gone
            };
            Kind          What       = Kind::Set;
            int           First      = 0;
            int           Count      = 0;
            std::uint64_t Generation = 0; // the collection's generation AFTER this change
        };

        // The change log is BOUNDED: a consumer that has not looked for longer than this many mutations is
        // told so (ChangesSince returns false) and treats the collection as replaced wholesale, which is
        // always a correct if coarser answer. Unbounded, a list nobody draws would grow it forever.
        static constexpr std::size_t kChangeLogDepth = 256;

        [[nodiscard]] int Size() const
        {
            return static_cast<int>( m_Records.size() );
        }
        [[nodiscard]] std::uint64_t Generation() const
        {
            return m_Generation;
        }
        // Which collection this is, process-wide: a list that saw one collection and now finds another
        // under the same key (erased and written again) must not read the new one's log as the old one's
        // continuation. A number rather than the address, because an address is reused.
        [[nodiscard]] std::uint64_t Serial() const
        {
            return m_Serial;
        }

        // Unchecked reads for a caller that already holds a valid index (the list's window is clamped to
        // Size()); the mutations below are the checked side because they take indices from scripts.
        [[nodiscard]] const UIDataStore& Record( int index ) const
        {
            return m_Records[static_cast<std::size_t>( index )].Fields;
        }
        [[nodiscard]] std::uint64_t RecordId( int index ) const
        {
            return m_Records[static_cast<std::size_t>( index )].Id;
        }
        [[nodiscard]] std::uint64_t RecordRevision( int index ) const
        {
            return m_Records[static_cast<std::size_t>( index )].Revision;
        }

        int                   Add( UIDataStore record );               // returns the new record's index
        Common::BoolResultStr Insert( int index, UIDataStore record ); // index in [0, Size()]
        Common::BoolResultStr Remove( int index );
        Common::BoolResultStr SetField( int index, const std::string& field, UIDataStore::Value value );
        void                  Clear();

        // Appends every change made after @p generation to @p out, oldest first. False when the log no
        // longer reaches back that far — the caller must then treat every index as changed.
        [[nodiscard]] bool ChangesSince( std::uint64_t generation, std::vector<Change>& out ) const;

    private:
        struct Entry
        {
            UIDataStore   Fields;
            std::uint64_t Id       = 0;
            std::uint64_t Revision = 0;
        };

        void Log( Change::Kind what, int first, int count );

        std::vector<Entry> m_Records;
        std::deque<Change> m_Log;
        std::uint64_t      m_Generation = 0;
        std::uint64_t      m_NextId     = 1;
        std::uint64_t      m_Revision   = 0; // one counter for every record, so a revision is never reused
        std::uint64_t      m_Serial     = NextSerial();

        static std::uint64_t NextSerial();
    };

    // The other half of the bridge: messages the canvas raised this frame (button actions, pointer
    // events, drops) queued for gameplay to consume. ScriptSystem drains it and calls OnUIMessage on
    // every loaded script, so a button reaches Lua without the UI knowing scripting exists.
    class UIMessageQueue
    {
    public:
        static UIMessageQueue& Get();

        void Push( std::string message );
        // Takes everything queued and clears it — a message is delivered exactly once.
        std::vector<std::string> Drain();
        void                     Clear();

    private:
        std::vector<std::string> m_Messages;
    };
} // namespace Desert::UI
