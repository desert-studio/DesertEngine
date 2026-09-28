// Ported in shape from UE 5.8 Engine/Source/Runtime/Foliage/Private/FoliageActor.cpp (FFoliageActor: one actor
// per instance transform, added / removed / moved with the FFoliageInfo), adapted: no UObject actors — the
// instances are prefab entities realized as the field's children, and the instance list is compared per frame
// instead of being driven by UE's per-instance callbacks, so an undo or a streamed-in cell needs no hook.
#include <Engine/World/Foliage/FoliagePrefabs.hpp>

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/Core.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace Desert::World::Foliage
{
    namespace
    {
        bool SameTransform( const glm::mat4& a, const glm::mat4& b )
        {
            for ( int c = 0; c < 4; ++c )
                for ( int r = 0; r < 4; ++r )
                    if ( std::abs( a[c][r] - b[c][r] ) > 1e-4f )
                        return false;
            return true;
        }
    } // namespace

    PrefabFoliagePlan PlanPrefabFoliage( std::span<const glm::mat4> wanted, std::span<const glm::mat4> realized )
    {
        PrefabFoliagePlan plan;
        const std::size_t kept = std::min( wanted.size(), realized.size() );
        plan.Create            = wanted.size() - kept;
        plan.Destroy           = realized.size() - kept;
        for ( std::size_t i = 0; i < kept; ++i )
            if ( !SameTransform( wanted[i], realized[i] ) )
                plan.Moved.push_back( i );
        return plan;
    }

    Common::BoolResultStr ApplyPrefabFoliage( std::span<const glm::mat4> wanted, const PrefabFoliageHost& host )
    {
        const std::vector<glm::mat4> realized = host.Realized();
        const PrefabFoliagePlan      plan     = PlanPrefabFoliage( wanted, realized );
        for ( std::size_t i = 0; i < plan.Destroy; ++i )
            host.DestroyLast();
        for ( const std::size_t index : plan.Moved )
            host.Place( index, wanted[index] );
        const std::size_t first = wanted.size() - plan.Create;
        for ( std::size_t index = first; index < wanted.size(); ++index )
        {
            if ( auto spawned = host.Spawn(); !spawned )
                return Common::MakeFormattedError<bool>( "instance {} of {} was not created: {}", index,
                                                         wanted.size(), spawned.GetError() );
            host.Place( index, wanted[index] );
        }
        return BOOLSUCCESS;
    }

    Common::ResultStr<std::string> PrefabFileGuid( const std::filesystem::path& file )
    {
        std::ifstream in( file, std::ios::binary );
        if ( !in )
            return Common::MakeFormattedError<std::string>( "prefab '{}' cannot be opened",
                                                            file.generic_string() );
        // The header object alone: the identity is in it, and the body can be any size.
        const auto object = Common::Content::ReadTextHeaderObject( in );
        if ( !object )
            return Common::MakeFormattedError<std::string>( "prefab '{}' has no header to be named by: {}",
                                                            file.generic_string(), object.GetError() );
        const auto header = Common::Content::ParseTextHeaderObject( object.GetValue() );
        if ( !header )
            return Common::MakeFormattedError<std::string>( "prefab '{}' header does not read: {}",
                                                            file.generic_string(), header.GetError() );
        if ( header.GetValue().Guid.empty() )
            return Common::MakeFormattedError<std::string>( "prefab '{}' states no GUID", file.generic_string() );
        return Common::MakeSuccess( header.GetValue().Guid );
    }
} // namespace Desert::World::Foliage
