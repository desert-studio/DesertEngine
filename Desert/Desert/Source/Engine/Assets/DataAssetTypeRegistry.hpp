#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <vector>

namespace Desert::Reflection
{
    struct TypeInfo;
}

namespace Desert::Assets
{
    /**
     * @brief One payload type a `.dasset` may hold (COUPLING-AUDIT #6, ASSET-TYPES-OPEN).
     *
     * UE: FPrimaryAssetType names a UPrimaryDataAsset subclass, and any reflected class is an asset. Here the
     * payload is a REFLECTED STRUCT (DesertHeaderTool, Engine/Reflection) named by its reflected type name; the
     * file states that name, and the struct's fields are read and written by reflection - no hand-written
     * mirror, no AssetTypeID enumerator, no switch. A game module registers its WeaponDef / AbilityDef /
     * LootTable at init and every engine path (load, save, hot reload, the editor's property panel) serves it.
     */
    struct DataAssetType
    {
        std::string     Name; // the reflected type name: ReflectionRegistry::Find( Name ) is its TypeInfo
        std::size_t     Size = 0;
        std::type_index Type = std::type_index( typeid( void ) );
        // A default-constructed payload (its member initializers are the new asset's values).
        std::shared_ptr<void> ( *Make )() = nullptr;
        // dst = src, both of this type: an in-place reload keeps the object every reader holds.
        void ( *Assign )( void* dst, const void* src ) = nullptr;
    };

    class DataAssetTypeRegistry
    {
    public:
        static DataAssetTypeRegistry& Get();

        /// Registers T under its reflected name. Refused (as a value): a name the reflection registry does not
        /// know, a reflected size that is not sizeof(T) (the name names another struct), or the name already
        /// registered for another C++ type. Registering the same T twice is accepted (a module re-init).
        template <typename T>
        Common::BoolResultStr Register( std::string_view reflectedName )
        {
            static_assert( std::is_default_constructible_v<T> && std::is_copy_assignable_v<T>,
                           "a data asset payload is a default-constructible, copy-assignable reflected struct" );
            DataAssetType type;
            type.Name = std::string( reflectedName );
            type.Size = sizeof( T );
            type.Type = std::type_index( typeid( T ) );
            type.Make = +[]() -> std::shared_ptr<void> { return std::make_shared<T>(); };
            type.Assign =
                 +[]( void* dst, const void* src ) { *static_cast<T*>( dst ) = *static_cast<const T*>( src ); };
            return Add( std::move( type ) );
        }

        /// The registered type named @p name, or nullptr. The pointer is stable for the process.
        [[nodiscard]] const DataAssetType* Find( std::string_view name ) const;

        /// Every registered name, sorted - what the Content Browser's «New Data Asset» lists.
        [[nodiscard]] std::vector<std::string> Names() const;

        /// The reflected description of @p type, or an error naming it (a registration outlived its reflection).
        [[nodiscard]] static Common::ResultStr<const Reflection::TypeInfo*>
        ReflectedTypeOf( const DataAssetType& type );

    private:
        DataAssetTypeRegistry() = default;

        Common::BoolResultStr Add( DataAssetType type );

        mutable std::mutex                                                 m_Mutex;
        std::map<std::string, std::unique_ptr<DataAssetType>, std::less<>> m_Types;
    };
} // namespace Desert::Assets
