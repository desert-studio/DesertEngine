#include <Engine/Core/Serialize/EntityReferenceRemap.hpp>

#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <Common/Json/Json.hpp>

#include <charconv>
#include <cstdint>
#include <string>
#include <system_error>

namespace Desert::Core::Rules
{
    std::size_t RemapEntityReferences( Assets::EntityData& record, const EntityIdRemap& remap )
    {
        std::size_t changed = 0;
        for ( const EntityReferenceRow& row : kEntityReferences )
        {
            const std::string key( row.ComponentKey );
            const auto        payload = record.Components.get( key );
            if ( !payload )
                continue;
            auto object = payload.value().to_object();
            if ( !object )
                continue;
            Common::Json::Object block = object.value();
            const auto           field = block.get( std::string( row.Field ) );
            if ( !field )
                continue;
            const auto text = field.value().to_string();
            if ( !text )
                continue;

            const std::string& digits = text.value();
            std::uint64_t      id     = 0;
            const auto         idRead = std::from_chars( digits.data(), digits.data() + digits.size(), id );
            if ( idRead.ec != std::errc{} || idRead.ptr != digits.data() + digits.size() )
                continue;

            const std::optional<Common::UUID> target = remap( Common::UUID( id ) );
            if ( !target.has_value() || static_cast<std::uint64_t>( *target ) == id )
                continue;
            block[std::string( row.Field )] =
                 Common::Json::Value( std::to_string( static_cast<std::uint64_t>( *target ) ) );
            record.Components[key] = Common::Json::Value( block );
            ++changed;
        }
        return changed;
    }

    bool HasEntityReferenceBlock( const Assets::EntityData& record )
    {
        for ( const EntityReferenceRow& row : kEntityReferences )
        {
            const auto payload = record.Components.get( std::string( row.ComponentKey ) );
            if ( !payload )
                continue;
            return true;
        }
        return false;
    }
} // namespace Desert::Core::Rules
