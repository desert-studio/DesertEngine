#include <Engine/VFX/VFXModuleCatalog.hpp>

#include <Engine/VFX/VFXEmitterSpawn.hpp>

#include <algorithm>
#include <format>

namespace Desert::VFX
{
    namespace S = Assets::Serialization;

    namespace
    {
        using T = VFXValueType;

        VFXModuleDecl Decl( const char* name, const VFXValueType type )
        {
            VFXModuleDecl decl;
            decl.Name = name;
            decl.Type = type;
            return decl;
        }

        bool SameGuid( const Assets::AssetGuidRef& ref, const Common::Content::AssetGuid& guid )
        {
            const auto parsed = Common::Content::AssetGuidFromText( ref.Guid );
            return parsed && parsed.GetValue() == guid;
        }
    } // namespace

    VFXModuleInfo MakeModuleInfo( const VFXModuleDefinition& module, std::string path )
    {
        VFXModuleInfo info;
        info.Module      = { Common::Content::AssetGuidToText( module.Guid ), std::move( path ) };
        info.DisplayName = module.DisplayName;
        info.Category    = module.Category;
        info.Stages      = module.Stages;
        info.Inputs      = module.Block.Inputs;
        info.Addable =
             module.Guid != kVFXSpawnFromChannelModule.Guid && module.Guid != kVFXSpawnFromEventModule.Guid;
        return info;
    }

    std::span<const VFXModuleDecl> CpuModuleContract( const Common::Content::AssetGuid& guid )
    {
        // VFXEmitterSpawn.cpp reads these by index (SpawnRate [0]; SpawnBurst [0] count, [1] time) and by name
        // (SpawnFromChannel): the order is the C++'s, the `.dfxmod` states the same order.
        static const std::vector<VFXModuleDecl> rate    = { Decl( "SpawnRate", T::Float ) };
        static const std::vector<VFXModuleDecl> burst   = { Decl( "SpawnCount", T::Int ),
                                                            Decl( "SpawnTime", T::Float ) };
        static const std::vector<VFXModuleDecl> channel = { Decl( "Channel", T::Int ),
                                                            Decl( "ParticlesPerEntry", T::Int ),
                                                            Decl( "MaxEntriesPerFrame", T::Int ),
                                                            Decl( "MaxDistance", T::Float ),
                                                            Decl( "Position", T::Vec3 ),
                                                            Decl( "Direction", T::Vec3 ),
                                                            Decl( "Color", T::Vec4 ),
                                                            Decl( "Lifetime", T::Float ),
                                                            Decl( "Size", T::Float ),
                                                            Decl( "Filter", T::Float ),
                                                            Decl( "FilterOp", T::Int ),
                                                            Decl( "FilterValue", T::Float ) };
        if ( guid == kVFXSpawnRateModule.Guid )
            return rate;
        if ( guid == kVFXSpawnBurstModule.Guid )
            return burst;
        if ( guid == kVFXSpawnFromChannelModule.Guid )
            return channel;
        return {};
    }

    Common::BoolResultStr CheckCpuModuleContract( const VFXModuleDefinition& module, const std::string_view where )
    {
        if ( !HasCpuBehaviour( module.Guid ) )
            return BOOLSUCCESS;
        const auto     contract = CpuModuleContract( module.Guid );
        const auto&    declared = module.Block.Inputs;
        const size_t   count    = std::max( contract.size(), declared.size() );
        constexpr auto kNone    = std::string_view( "nothing" );
        for ( size_t k = 0; k < count; ++k )
        {
            const VFXModuleDecl* want = k < contract.size() ? &contract[k] : nullptr;
            const VFXModuleDecl* have = k < declared.size() ? &declared[k] : nullptr;
            if ( want != nullptr && have != nullptr && want->Name == have->Name && want->Type == have->Type )
                continue;
            return Common::MakeFormattedError<bool>(
                 "{}: module '{}' input {} is {} {} in its file but the engine C++ reads {} {} there", where,
                 module.DisplayName, k, have ? std::string_view( have->Name ) : kNone,
                 have ? GlslTypeName( have->Type ) : std::string_view(),
                 want ? std::string_view( want->Name ) : kNone,
                 want ? GlslTypeName( want->Type ) : std::string_view() );
        }
        return BOOLSUCCESS;
    }

    S::VFXModuleUse DefaultModuleUse( const VFXModuleInfo& module )
    {
        S::VFXModuleUse use;
        use.Module = module.Module;
        for ( const VFXModuleDecl& decl : module.Inputs )
        {
            S::VFXModuleInput in;
            in.Name   = decl.Name;
            in.Type   = decl.Type;
            in.Source = S::VFXInputSource::Value;
            in.Value  = decl.Default.value_or( glm::vec4( 0.0f ) );
            use.Inputs.push_back( std::move( in ) );
        }
        return use;
    }

    const VFXModuleInfo* FindModule( std::span<const VFXModuleInfo> catalog, const Assets::AssetGuidRef& ref )
    {
        const auto guid = Common::Content::AssetGuidFromText( ref.Guid );
        if ( !guid )
            return nullptr;
        for ( const VFXModuleInfo& info : catalog )
            if ( SameGuid( info.Module, guid.GetValue() ) )
                return &info;
        return nullptr;
    }

    const VFXModuleDecl* FindModuleInput( std::span<const VFXModuleInfo> catalog, const Assets::AssetGuidRef& ref,
                                          const std::string_view input )
    {
        const VFXModuleInfo* info = FindModule( catalog, ref );
        if ( info == nullptr )
            return nullptr;
        for ( const VFXModuleDecl& decl : info->Inputs )
            if ( decl.Name == input )
                return &decl;
        return nullptr;
    }
} // namespace Desert::VFX
