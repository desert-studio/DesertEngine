#include <Common/Core/AssetPathIndex.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Logger.hpp>

#include <mutex>
#include <unordered_map>

namespace Common::AssetPathIndex
{
    namespace
    {
        // A FUNCTION-LOCAL STATIC RATHER THAN A NAMESPACE-SCOPE ONE. `AssetHandle::FromCookedPath`
        // records into this table, and that function is reachable from other translation units'
        // static initialisers (an asset id computed to seed a table, which this repository does);
        // a namespace-scope object would then be read before its own constructor ran. The
        // function-local form is constructed on first use by definition, which is the only spelling
        // that has no order to get wrong.
        struct Table
        {
            std::mutex                                 Mutex;
            std::unordered_map<uint64_t, std::string>  KeyByHandle;
        };

        Table& Get()
        {
            static Table table;
            return table;
        }
    } // namespace

    BoolResultStr Record( uint64_t handle, std::string_view stableKey )
    {
        if ( handle == 0 )
            return MakeFormattedError<bool>( "a null handle names nothing, so it cannot be bound to '{}'",
                                             std::string( stableKey ) );
        if ( stableKey.empty() )
            return MakeFormattedError<bool>( "handle {} cannot be bound to an empty key", handle );

        Table& table = Get();

        std::lock_guard<std::mutex> lock( table.Mutex );

        const auto [it, inserted] = table.KeyByHandle.emplace( handle, std::string( stableKey ) );
        if ( inserted )
            return MakeSuccess( true );

        if ( it->second == stableKey )
            return MakeSuccess( true );

        // THE MESSAGE CARRIES BOTH KEYS AND THE NUMBER because a handle miss has no filename in it —
        // the same reason TextureAsset's mismatch message spells out every quantity it compares. A
        // reader who sees only "collision" has to find the two files by hashing the tree by hand.
        LOG_ERROR( "[AssetPathIndex] handle {0} is already bound to '{1}', so '{2}' was REFUSED and will "
                   "never be reachable by that number. Two files cannot share one identity: rename one of "
                   "them, then re-point every reference that names {0}.",
                   handle, it->second, std::string( stableKey ) );

        return MakeFormattedError<bool>( "handle {} is already bound to '{}'; '{}' was refused", handle,
                                         it->second, std::string( stableKey ) );
    }

    std::string KeyFor( uint64_t handle )
    {
        if ( handle == 0 )
            return {};

        Table& table = Get();

        std::lock_guard<std::mutex> lock( table.Mutex );

        const auto it = table.KeyByHandle.find( handle );
        return it == table.KeyByHandle.end() ? std::string() : it->second;
    }

    std::filesystem::path PathFor( uint64_t handle )
    {
        const std::string key = KeyFor( handle );
        if ( key.empty() )
            return {};

        return AssetHandle::PathForStableKey( key );
    }

    std::size_t Size()
    {
        Table& table = Get();

        std::lock_guard<std::mutex> lock( table.Mutex );

        return table.KeyByHandle.size();
    }

    void Clear()
    {
        Table& table = Get();

        std::lock_guard<std::mutex> lock( table.Mutex );

        table.KeyByHandle.clear();
    }
} // namespace Common::AssetPathIndex
