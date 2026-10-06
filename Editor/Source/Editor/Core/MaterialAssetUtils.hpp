#pragma once

// Demo/builder-side material authoring: meshes are coloured through REAL material assets in slots
// (UE-style), never through the per-entity MaterialComponent override channel — that channel is a
// runtime-only script seed, not an authoring surface.

#include <Engine/Assets/AssetManager.hpp>
#include <Common/Content/CanonicalText.hpp>
#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Engine/Assets/MaterialFormat.hpp>
#include <Engine/Assets/MaterialParamDiff.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/Shader/ShaderAsset.hpp>
#include <Engine/Project/ProjectContext.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/UUID.hpp>
#include <Common/Utilities/FileSystem.hpp>

// rfl serialization environment (same as SurfaceMaterialAsset.cpp) — needed to write a fresh
// material file with its stable GUID before the asset is created/registered.
#include <Common/Core/Serialization/GlmReflection.hpp>
#include <rflcpp/rfl/json.hpp>

#include <filesystem>
#include <format>
#include <initializer_list>
#include <string>
#include <system_error>
#include <utility>

namespace Desert::Editor::MaterialAssetUtils
{
    // States in @p data the template every NEW material is created with: the project's `.deproj`
    // DefaultSurfaceTemplate when it names one, else the one loaded shader declaring `Default Surface`
    // (Assets::FindDefaultSurfaceTemplate). A material is never written without a template.
    [[nodiscard]] inline Common::BoolResultStr StateDefaultSurface( Assets::MaterialData&       data,
                                                                    const Assets::AssetManager& manager )
    {
        const auto shader = Assets::FindDefaultSurfaceTemplate(
             manager, Project::ProjectContext::DefaultSurfaceTemplate(), Project::ProjectContext::FilePath() );
        if ( !shader )
            return Common::MakeError( shader.GetError() );
        return Assets::SurfaceMaterialAsset::StateShader( data, manager, shader.GetValue() );
    }

    // States in @p data the template @p templateGuid names (a shader header GUID, 32 hex digits); EMPTY names
    // the `Default Surface` template (StateDefaultSurface). Refused with the GUID when it is malformed or names
    // no loaded shader — never answered with the default, which would author the params on a template that
    // has no row for them.
    [[nodiscard]] inline Common::BoolResultStr
    StateTemplate( Assets::MaterialData& data, const Assets::AssetManager& manager, std::string_view templateGuid )
    {
        if ( templateGuid.empty() )
            return StateDefaultSurface( data, manager );
        const auto guid = Common::Content::AssetGuidFromText( templateGuid );
        if ( !guid )
            return Common::MakeError(
                 std::format( "template '{}' is not a shader GUID ({})", templateGuid, guid.GetError() ) );
        const Common::AssetHandle handle(
             static_cast<uint64_t>( Common::Content::HandleForGuid( guid.GetValue() ) ) );
        return Assets::SurfaceMaterialAsset::StateShader( data, manager, handle );
    }

    // WHICH OF THE TWO THINGS HAPPENED. The function below can either author a material or hand back
    // one somebody else authored, and those are different answers to the caller's question — which is
    // precisely what it used to be unable to say. `Reused` does NOT mean "wrong": a demo material the
    // user edited in the editor is supposed to survive a restart. It means "the values you passed were
    // not applied", and the caller, the log and the test suite are all entitled to know that.
    enum class MaterialAssetOrigin
    {
        Failed = 0,       // nothing usable came back; Handle is null
        Created,          // this call wrote the .demat from the requested params
        ReusedFromMemory, // an asset under this path was already loaded
        ReusedFromDisk,   // a .demat existed on disk and was loaded rather than written
    };

    // The answer, whole. Handle alone is what the old signature returned, and returning it alone is what
    // made "created what you asked for" and "found something else entirely" indistinguishable.
    struct MaterialAssetOutcome
    {
        Assets::AssetHandle Handle = Common::UUID::Null();
        MaterialAssetOrigin Origin = MaterialAssetOrigin::Failed;
        // Non-empty only when the material was reused AND disagrees with the request. Already reported
        // through LOG_WARN by the time the caller sees it — this is here so a caller can act, and so a
        // test can assert the reporting without scraping a log.
        std::vector<Assets::MaterialParamDivergence> Divergences;

        [[nodiscard]] bool IsValid() const
        {
            return Origin != MaterialAssetOrigin::Failed && !Handle.IsNull();
        }
        [[nodiscard]] bool Diverged() const
        {
            return !Divergences.empty();
        }
    };

    // Finds, or else authors, a material ASSET (.demat) on @p templateGuid's template (empty = the `Default
    // Surface` one, see StateTemplate) carrying the given schema params;
    // registers its shell with the MaterialService (the runtime material builds lazily on first Get, so
    // this is safe before shaders are preloaded) and returns the handle to drop into a mesh material
    // slot.
    //
    // IT IS NAMED FOR WHAT IT DOES. It was called `CreatePBRMaterialAsset` and it creates nothing at all
    // when a file already exists — an EXISTING file is never rewritten, on purpose, so that a user's
    // edits to a demo material survive a restart. That intent is kept. What is gone is the silence
    // around it: a reused material is now COMPARED against the request, and every parameter that
    // disagrees is named with both values (contract §1.4 — never substitute quietly; log the reason and
    // the actual numbers).
    //
    // Why this mattered enough to change a signature: `CB_Red.demat` shipped carrying MetallicFactor 1.0
    // and RoughnessFactor 0.0 against a call site asking for roughness 0.9 and no metalness. The Cornell
    // box's left wall rendered as a black chrome mirror for as long as the file existed, and the builder
    // could not have fixed it on any launch because it never looked. The values at the call site had
    // quietly become unreachable code.
    [[nodiscard]] inline MaterialAssetOutcome
    FindOrCreatePBRMaterialAsset( const Assets::AssetManager* am, const std::string& name,
                                  const std::vector<Assets::MaterialParamRequest>& params,
                                  std::string_view                                 templateGuid = {} )
    {
        MaterialAssetOutcome outcome;
        if ( !am )
        {
            LOG_ERROR( "[Material] '{}' was not resolved: no AssetManager.", name );
            return outcome;
        }

        const std::string           ext( Common::Constants::Extensions::MATERIAL_EXTENSION );
        const std::filesystem::path dir = Common::Constants::Path::MATERIAL_PATH;
        std::error_code             ec;
        std::filesystem::create_directories( dir, ec );
        const std::filesystem::path path = dir / ( name + ext );

        // Reported at the one place both reuse paths pass through, so neither can grow a silent branch.
        const auto reportReuse = [&]( const Assets::MaterialData& found )
        {
            outcome.Divergences = Assets::DiffRequestedParams( params, found );
            if ( outcome.Divergences.empty() )
                return;
            LOG_WARN( "[Material] '{}' already exists and was reused UNCHANGED, but it disagrees with "
                      "what the caller asked for: {}. The requested values were NOT applied — edit the "
                      "file, or delete it to have it re-authored.",
                      path.generic_string(), Assets::DescribeDivergences( outcome.Divergences ) );
        };

        if ( auto existing = am->FindByPath<Assets::SurfaceMaterialAsset>( path.generic_string() ) )
        {
            Runtime::ResourceRegistry::GetMaterialService()->RegisterAsset( existing );
            reportReuse( existing->Data() );
            outcome.Handle = existing->GetMetadata().Handle;
            outcome.Origin = MaterialAssetOrigin::ReusedFromMemory;
            return outcome;
        }

        const bool onDisk = std::filesystem::exists( path, ec );
        if ( !onDisk )
        {
            Assets::MaterialData data;
            if ( const auto stated = StateTemplate( data, *am, templateGuid ); !stated )
            {
                LOG_ERROR( "[Material] '{}' was not created: {}", path.generic_string(), stated.GetError() );
                return outcome;
            }
            for ( const auto& param : params )
                data.SetParam( param.Name, param.Value );
            // REFUSED rather than carried on: the CreateAsset below would load the file that was not
            // written, get canonical defaults, and hand back a handle for a material that has none of
            // the authored parameters and no file behind it. The caller (a startup scene builder) would
            // then put that handle into a mesh slot and save it into a .desce.
            if ( const auto written = Assets::WriteMaterialFile( path, data ); !written )
            {
                LOG_ERROR( "[Material] '{}' was not created: {}", path.generic_string(), written.GetError() );
                return outcome;
            }
        }

        auto asset = const_cast<Assets::AssetManager&>( *am ).CreateAsset<Assets::SurfaceMaterialAsset>(
             path.generic_string() );
        if ( !asset )
        {
            LOG_ERROR( "[Material] '{}' could not be created as an asset.", path.generic_string() );
            return outcome;
        }

        Runtime::ResourceRegistry::GetMaterialService()->RegisterAsset( asset );
        // A file that was already on disk is somebody else's statement of what this material is, exactly
        // as a loaded one is — so it gets the same comparison. This is the branch CB_Red came back
        // through on every cold start, and the branch that said nothing.
        if ( onDisk )
            reportReuse( asset->Data() );
        outcome.Handle = asset->GetMetadata().Handle;
        outcome.Origin = onDisk ? MaterialAssetOrigin::ReusedFromDisk : MaterialAssetOrigin::Created;
        return outcome;
    }

    // The brace-list spelling the demo builders read best. Same function.
    [[nodiscard]] inline MaterialAssetOutcome
    FindOrCreatePBRMaterialAsset( const Assets::AssetManager* am, const std::string& name,
                                  std::initializer_list<std::pair<const char*, glm::vec4>> params,
                                  std::string_view                                         templateGuid = {} )
    {
        std::vector<Assets::MaterialParamRequest> requested;
        requested.reserve( params.size() );
        for ( const auto& [pname, value] : params )
            requested.push_back( { std::string( pname ), value } );
        return FindOrCreatePBRMaterialAsset( am, name, requested, templateGuid );
    }

    [[nodiscard]] inline MaterialAssetOutcome FindOrCreatePBRMaterialAsset( const Assets::AssetManager* am,
                                                                            const std::string&          name,
                                                                            const glm::vec4&            albedo,
                                                                            float                       roughness )
    {
        return FindOrCreatePBRMaterialAsset(
             am, name,
             { { "AlbedoColor", albedo }, { "RoughnessFactor", glm::vec4( roughness, 0.0f, 0.0f, 0.0f ) } } );
    }
} // namespace Desert::Editor::MaterialAssetUtils
