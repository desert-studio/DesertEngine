#include <PhysicsCore/PhysicalMaterial.hpp>

#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <utility>

namespace Desert::Physics
{
    float CombineMaterialValues( float a, MaterialCombine modeA, float b, MaterialCombine modeB )
    {
        switch ( std::max( modeA, modeB ) )
        {
            case MaterialCombine::Average:
                return 0.5f * ( a + b );
            case MaterialCombine::Min:
                return std::min( a, b );
            case MaterialCombine::Multiply:
                return a * b;
            case MaterialCombine::Max:
                return std::max( a, b );
        }
        // Reached only by a value no enumerator names; every named mode is handled above.
        return 0.5f * ( a + b );
    }

    Common::BoolResultStr ValidatePhysicalMaterial( const PhysicalMaterialDesc& desc )
    {
        if ( !std::isfinite( desc.Friction ) || desc.Friction < 0.0f )
            return Common::MakeFormattedError<bool>( "Friction {} is not a finite value >= 0", desc.Friction );
        if ( !std::isfinite( desc.Restitution ) || desc.Restitution < 0.0f || desc.Restitution > 1.0f )
            return Common::MakeFormattedError<bool>( "Restitution {} is outside [0, 1]", desc.Restitution );
        if ( !std::isfinite( desc.DensityGPerCm3 ) || !( desc.DensityGPerCm3 > 0.0f ) )
            return Common::MakeFormattedError<bool>( "Density {} g/cm^3 is not a finite value > 0",
                                                     desc.DensityGPerCm3 );
        return BOOLSUCCESS;
    }

    Common::ResultStr<PhysicalSurfaces> PhysicalSurfaces::Build( const std::vector<std::string>& names )
    {
        if ( names.size() > kMaxSurfaceTypes )
            return Common::MakeFormattedError<PhysicalSurfaces>(
                 "{} surface types are named; a project has at most {} after '{}' (UE SurfaceType1..SurfaceType62)",
                 names.size(), kMaxSurfaceTypes, kDefaultSurfaceName );
        PhysicalSurfaces surfaces;
        surfaces.m_Names.reserve( names.size() + 1u );
        surfaces.m_Names.emplace_back( kDefaultSurfaceName );
        for ( std::size_t i = 0; i < names.size(); ++i )
        {
            const std::string& name = names[i];
            if ( name.empty() )
                return Common::MakeFormattedError<PhysicalSurfaces>( "surface type {} has no name", i + 1u );
            if ( std::find( surfaces.m_Names.begin(), surfaces.m_Names.end(), name ) != surfaces.m_Names.end() )
                return Common::MakeFormattedError<PhysicalSurfaces>(
                     "surface type '{}' is named twice ('{}' is the register's own first type)", name,
                     kDefaultSurfaceName );
            surfaces.m_Names.push_back( name );
        }
        return Common::MakeSuccess( std::move( surfaces ) );
    }

    Common::ResultStr<SurfaceTypeId> PhysicalSurfaces::Resolve( std::string_view name ) const
    {
        for ( std::size_t i = 0; i < m_Names.size(); ++i )
            if ( m_Names[i] == name )
                return Common::MakeSuccess( static_cast<SurfaceTypeId>( i ) );
        std::string known;
        for ( const std::string& type : m_Names )
            known += known.empty() ? type : std::format( ", {}", type );
        return Common::MakeFormattedError<SurfaceTypeId>( "no surface type '{}' (types: {})", name, known );
    }

    const std::string* PhysicalSurfaces::NameOf( SurfaceTypeId id ) const
    {
        return id < m_Names.size() ? &m_Names[id] : nullptr;
    }

    Common::ResultStr<PhysicalMaterialsSettings> ReadPhysicalMaterialsSettings( const std::filesystem::path& path )
    {
        // Through the VFS as well as the disk: a packaged game reads the register out of its archive.
        if ( !Common::Utils::FileSystem::Exists( path ) )
            return Common::MakeFormattedError<PhysicalMaterialsSettings>(
                 "physical materials register '{}' does not exist", path.string() );
        auto read = Common::Json::ReadFile<PhysicalMaterialsConfig>( path );
        if ( !read )
            return Common::MakeFormattedError<PhysicalMaterialsSettings>( "{}", read.GetError() );
        const PhysicalMaterialsConfig& config = read.GetValue();
        if ( config.DefaultMaterialGuid.empty() )
            return Common::MakeFormattedError<PhysicalMaterialsSettings>(
                 "'{}' names no DefaultMaterialGuid: a body without a material of its own has nothing to slide on",
                 path.string() );
        auto surfaces = PhysicalSurfaces::Build( config.SurfaceTypes );
        if ( !surfaces )
            return Common::MakeFormattedError<PhysicalMaterialsSettings>( "'{}': {}", path.string(),
                                                                          surfaces.GetError() );
        PhysicalMaterialsSettings settings;
        settings.DefaultMaterialGuid = config.DefaultMaterialGuid;
        settings.DefaultMaterialPath = config.DefaultMaterialPath;
        settings.Surfaces            = surfaces.ExtractValue();
        return Common::MakeSuccess( std::move( settings ) );
    }
} // namespace Desert::Physics
