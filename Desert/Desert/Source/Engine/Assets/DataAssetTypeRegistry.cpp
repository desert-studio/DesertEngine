#include <Engine/Assets/DataAssetTypeRegistry.hpp>

#include <Engine/Reflection/ReflectionRegistry.hpp>

namespace Desert::Assets
{
    DataAssetTypeRegistry& DataAssetTypeRegistry::Get()
    {
        static DataAssetTypeRegistry registry;
        return registry;
    }

    Common::BoolResultStr DataAssetTypeRegistry::Add( DataAssetType type )
    {
        if ( type.Name.empty() )
            return Common::MakeFormattedError<bool>( "a data asset type needs its reflected name" );

        const Reflection::TypeInfo* reflected = Reflection::ReflectionRegistry::Get().Find( type.Name );
        if ( reflected == nullptr )
            return Common::MakeFormattedError<bool>(
                 "data asset type '{}' is not a reflected struct (no REFLECT in a header DesertHeaderTool reads)",
                 type.Name );
        if ( reflected->Size != type.Size )
            return Common::MakeFormattedError<bool>(
                 "data asset type '{}': the reflected struct of that name is {} bytes, the registered C++ type {}",
                 type.Name, reflected->Size, type.Size );

        const std::lock_guard<std::mutex> lock( m_Mutex );
        if ( const auto it = m_Types.find( type.Name ); it != m_Types.end() )
        {
            if ( it->second->Type != type.Type )
                return Common::MakeFormattedError<bool>(
                     "data asset type '{}' is already registered for another C++ type", type.Name );
            return BOOLSUCCESS;
        }
        std::string name = type.Name;
        m_Types.emplace( std::move( name ), std::make_unique<DataAssetType>( std::move( type ) ) );
        return BOOLSUCCESS;
    }

    const DataAssetType* DataAssetTypeRegistry::Find( std::string_view name ) const
    {
        const std::lock_guard<std::mutex> lock( m_Mutex );
        const auto                        it = m_Types.find( name );
        return it == m_Types.end() ? nullptr : it->second.get();
    }

    std::vector<std::string> DataAssetTypeRegistry::Names() const
    {
        const std::lock_guard<std::mutex> lock( m_Mutex );
        std::vector<std::string>          names;
        names.reserve( m_Types.size() );
        for ( const auto& [name, type] : m_Types )
            names.push_back( name );
        return names; // std::map: already sorted
    }

    Common::ResultStr<const Reflection::TypeInfo*>
    DataAssetTypeRegistry::ReflectedTypeOf( const DataAssetType& type )
    {
        const Reflection::TypeInfo* reflected = Reflection::ReflectionRegistry::Get().Find( type.Name );
        if ( reflected == nullptr )
            return Common::MakeFormattedError<const Reflection::TypeInfo*>(
                 "data asset type '{}' has no reflected description", type.Name );
        return Common::MakeSuccess( reflected );
    }
} // namespace Desert::Assets
