#include "PackageCook.hpp"
#include "PackagedContentTrees.hpp"

#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Core/ShaderCompiler/ShaderCacheKey.hpp>
#include <Engine/Core/ShaderCompiler/ShaderCompiler.hpp>
#include <Engine/Core/ShaderCompiler/ShaderPreprocess/ShaderPreprocessor.hpp>
#include <Engine/Core/ShaderCompiler/ShaderSpirvCache.hpp>
#include <Engine/Graphic/Clouds/CloudMaterialValues.hpp>
#include <Engine/Graphic/GlobalIllumination/ScreenProbeGather.hpp>
#include <Engine/Graphic/GlobalIllumination/SurfaceCacheRenderer.hpp>
#include <Engine/Graphic/GlobalIllumination/WorldRadianceCache.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/DeferredLightingRenderer.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/SSRRenderer.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>
#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Core/Serialize/ExternalEntities.hpp>
#include <Engine/Core/Serialize/GenericBlock.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
#include <Engine/Graphic/View/TemporalAA.hpp>
#include <Engine/Graphic/View/TemporalSuperResolution.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>
#include <Engine/Runtime/Services/ServiceScanRoots.hpp>
#include <Engine/Text/FontCache.hpp>
#include <Engine/Vector/IconBake.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/DDCShipRegister.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Assets/MeshCardRepresentation.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/MeshDistanceField.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>
#include <Engine/Assets/Shader/ShaderAsset.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>

#include <Editor/Import/ImportManager.hpp>
#include <Editor/Import/MeshDeriver.hpp>
#include <Editor/Import/TextureImporter.hpp>

#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Content/ImportRecord.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Settings/Scalability.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace Desert::Editor
{
    namespace fs = std::filesystem;

    namespace
    {
        void CookShaders( bool spirvDebugInfo, bool developerInstruments, CookStats& stats )
        {
            namespace Preprocess = ::Desert::Core::Preprocess;

            for ( const fs::path& file : PackagedShaderPrograms( developerInstruments ) )
            {
                // Ф3 made the primitive return a ResultStr, so a miss is a named refusal instead of an
                // empty string indistinguishable from an empty file. Policy here is unchanged - count it
                // and move on - but the reason now reaches the log.
                const auto contentRead = Common::Utils::FileSystem::ReadFileContent( file );
                if ( !contentRead || contentRead.GetValue().empty() )
                {
                    ++stats.Failures; // the primitive already logged a miss; an empty file is silent
                    continue;
                }
                const std::string& content = contentRead.GetValue();

                // The default program plus every named pass — the same set ShaderService::Register
                // turns into programs at startup. A broken .shader fails THIS shader's cook (the
                // preprocessor's refusal names the file and the reason), not the whole packaging run.
                std::vector<std::string> passes = { "" };
                const auto meta = Preprocess::ShaderPreprocess::ParseProgramMetaForPass( content, file, "" );
                if ( !meta.IsSuccess() )
                {
                    LOG_ERROR( "[PackageCook] {} was not cooked: {}", file.string(), meta.GetError() );
                    ++stats.Failures;
                    continue;
                }
                // The pass the default program IS (IsDefaultProgramPass): cooked once, as "".
                for ( const std::string& pass : meta.GetValue().PassNames )
                    if ( !Preprocess::IsDefaultProgramPass( meta.GetValue(), pass ) )
                        passes.push_back( pass );

                for ( const std::string& passName : passes )
                {
                    const auto stages =
                         Preprocess::ShaderPreprocess::PreProcessProgramPass( content, file, passName );
                    if ( !stages.IsSuccess() )
                    {
                        LOG_ERROR( "[PackageCook] {} pass '{}' was not cooked: {}", file.string(), passName,
                                   stages.GetError() );
                        ++stats.Failures;
                        continue;
                    }
                    for ( const auto& [stage, source] : stages.GetValue() )
                    {
                        const uint64_t key = ::Desert::Core::ComputeShaderCacheKeyForProfile( stage, source, file,
                                                                                              spirvDebugInfo );
                        if ( ::Desert::Core::TryLoadCachedSpirv( key ) )
                        {
                            ++stats.ShadersCached;
                            continue;
                        }
                        // Compiles under the SAME key (same inputs, same profile) and stores it.
                        if ( !::Desert::Core::ShaderCompiler::CompileGLSLToSPIRVForProfile(
                                   stage, source, file.string(), spirvDebugInfo )
                                   .IsSuccess() )
                        {
                            ++stats.Failures; // the compiler logged file/stage/diagnostic
                            continue;
                        }
                        // A compile whose artifact did not reach the disk is not a cooked artifact:
                        // the pak would ship nothing under this key and the player would pay the
                        // compile. The store is best-effort for the runtime and mandatory here.
                        if ( !::Desert::Core::TryLoadCachedSpirv( key ) )
                        {
                            LOG_ERROR( "[PackageCook] {} [{}] compiled but its artifact did not reach {} — "
                                       "the package would ship a shader the runtime must recompile",
                                       file.string(), static_cast<int>( stage ),
                                       ::Desert::Core::SpirvCachePathForKey( key ).string() );
                            ++stats.StoreFailures;
                            continue;
                        }
                        ++stats.ShadersCompiled;
                    }
                }
            }
        }

        // ONE COOKED PERMUTATION: @p cooked.Program ("<Shader>" or "<Shader>/<Pass>", ShaderService's names) under
        // its variant, every stage, into the SPIR-V store under the key the runtime asks for.
        void CookVariant( const ::Desert::Core::CookedProgramVariant& cooked, const std::vector<fs::path>& programs,
                          bool spirvDebugInfo, CookStats& stats )
        {
            namespace Preprocess = ::Desert::Core::Preprocess;

            const std::size_t slash  = cooked.Program.find( '/' );
            const std::string shader = cooked.Program.substr( 0, slash );
            const std::string pass   = slash == std::string::npos ? std::string() : cooked.Program.substr( slash + 1 );
            const auto        file =
                 std::ranges::find_if( programs, [&]( const fs::path& p ) { return p.stem().string() == shader; } );
            if ( file == programs.end() )
            {
                LOG_ERROR( "[PackageCook] the shipped program {} is not among the packaged shaders", cooked.Program );
                ++stats.Failures;
                return;
            }
            const auto content = Common::Utils::FileSystem::ReadFileContent( *file );
            if ( !content || content.GetValue().empty() )
            {
                ++stats.Failures; // the primitive logged a miss; an empty file is CookShaders' failure too
                return;
            }
            const auto stages = Preprocess::ShaderPreprocess::PreProcessProgramPass( content.GetValue(), *file, pass );
            if ( !stages.IsSuccess() )
            {
                LOG_ERROR( "[PackageCook] {} [{:016x}] was not cooked: {}", cooked.Program, cooked.Variant.Hash(),
                           stages.GetError() );
                ++stats.Failures;
                return;
            }
            for ( const auto& [stage, source] : stages.GetValue() )
            {
                const uint64_t key = ::Desert::Core::ComputeShaderCacheKeyForProfile( stage, source, *file,
                                                                                      spirvDebugInfo, cooked.Variant );
                if ( ::Desert::Core::TryLoadCachedSpirv( key ) )
                {
                    ++stats.ShadersCached;
                    continue;
                }
                if ( !::Desert::Core::ShaderCompiler::CompileGLSLToSPIRVForProfile( stage, source, file->string(),
                                                                                   spirvDebugInfo, cooked.Variant )
                           .IsSuccess() )
                {
                    ++stats.Failures;
                    continue;
                }
                if ( !::Desert::Core::TryLoadCachedSpirv( key ) )
                {
                    LOG_ERROR( "[PackageCook] {} [{:016x}] stage {} compiled but its artifact did not reach {}",
                               cooked.Program, cooked.Variant.Hash(), static_cast<int>( stage ),
                               ::Desert::Core::SpirvCachePathForKey( key ).string() );
                    ++stats.StoreFailures;
                    continue;
                }
                ++stats.ShadersCompiled;
            }
        }

        // THE PERMUTATIONS THE PACKAGE CAN SELECT (UE cooks a global shader's permutations), each system's one
        // list - the list its runtime acquires from - so a package never compiles one (ShaderVariant::
        // ShippedByCook refuses the miss by name):
        //   the tracing consumers   under every tracer permutation (GlobalIllumination::kTracerPermutations):
        //                           WorldRadianceCache, SurfaceCacheRenderer, SSRRenderer ProgramVariants;
        //   the untraced systems    DeferredLightingRenderer, TemporalAA, TemporalSuperResolution ProgramVariants
        //                           (and SSR's untraced front layer in SSRRenderer's);
        //   the mesh materials      every registered `.demat` (ContentRegistry) under its forward cell's
        //                           permutations (MeshRenderer::MaterialProgramVariants);
        //   the shader overrides    every registered scene's Material (Shader Override) block, its shader by
        //                           GUID, under MeshRenderer::OverrideProgramVariants;
        //   the cloud media         every registered Volume medium into each of its programs
        //                           (CloudMediumProgramVariants);
        //   the screen probes       every layout a Scalability.json level names - {ScreenProbeDownsample,
        //                           ScreenProbeTracedTexels} per level - under ScreenProbeGather::ProgramVariants.
        void CookShaderVariants( bool spirvDebugInfo, bool developerInstruments, CookStats& stats )
        {
            namespace Scalability = ::Common::Scalability;
            namespace GI          = Graphic::GlobalIllumination;

            std::vector<::Desert::Core::CookedProgramVariant> cooked;
            const auto append = [&cooked]( std::vector<::Desert::Core::CookedProgramVariant> list )
            {
                for ( auto& entry : list )
                    cooked.push_back( std::move( entry ) );
            };
            append( GI::WorldRadianceCache::ProgramVariants() );
            append( Graphic::SurfaceCache::SurfaceCacheRenderer::ProgramVariants() );
            append( Graphic::System::SSRRenderer::ProgramVariants() );
            append( Graphic::System::DeferredLightingRenderer::ProgramVariants() );
            append( Graphic::TemporalAA::ProgramVariants() );
            append( Graphic::TemporalSuperResolution::ProgramVariants() );

            if ( const auto shaders = Runtime::ResourceRegistry::GetShaderService() )
            {
                for ( const std::string& medium : shaders->MediumSources() )
                    append( Graphic::CloudMediumProgramVariants( medium ) );

                if ( const auto* materials = Runtime::ResourceRegistry::GetMaterialService() )
                {
                    std::vector<::Common::AssetHandle> handles;
                    for ( const auto& row : Assets::ContentRegistry::Rows( Common::Content::ContentKind::Material ) )
                        handles.push_back( row.Handle );
                    append( Graphic::System::MeshRenderer::MaterialProgramVariants( *materials, *shaders, handles ) );
                }
                else
                {
                    LOG_ERROR( "[PackageCook] no material service: the mesh materials' permutations were not cooked" );
                    ++stats.Failures;
                }

                // The Shader Override entities of the scenes the package carries: the shader by the GUID the
                // block states, its compile key the shader file's stem (Assets::FindShaderNameByRef's rule).
                const auto shaderRows = Assets::ContentRegistry::Rows( Common::Content::ContentKind::Shader );
                std::vector<std::string> overrides;
                for ( const auto& row : Assets::ContentRegistry::Rows( Common::Content::ContentKind::Scene ) )
                {
                    auto text  = ::Desert::Core::ExternalEntities::ReadSceneFileText( row.Path );
                    auto scene = text ? ::Desert::Core::ParseLoadableScene( row.Path.string(), text.GetValue() )
                                      : Common::MakeError<::Desert::Core::LoadableScene>( text.GetError() );
                    if ( !scene )
                    {
                        LOG_ERROR( "[PackageCook] {}: its shader overrides' permutations were not cooked: {}",
                                   row.Key, scene.GetError() );
                        ++stats.Failures;
                        continue;
                    }
                    for ( const auto& entity : scene.GetValue().Scene.Entities )
                    {
                        const auto found = entity.Components.get( "Material" );
                        if ( !found.has_value() )
                            continue;
                        ::Common::Json::Issues issues;
                        const auto block = ::Desert::Core::Serialize::ReadBlock<Assets::MaterialComponentSer>(
                             ::Common::Json::Root( found.value(), ::Common::Json::Path().Key( "Material" ) ),
                             issues );
                        if ( !block.has_value() || !block->Shader.has_value() )
                            continue; // no shader: nothing drawn forward (the scene load names a bad block)
                        const auto shader =
                             std::find_if( shaderRows.begin(), shaderRows.end(),
                                           [&]( const auto& s )
                                           {
                                               return s.Guid.has_value() &&
                                                      Common::Content::AssetGuidToText( *s.Guid ) ==
                                                           block->Shader->Guid;
                                           } );
                        if ( shader == shaderRows.end() )
                        {
                            LOG_ERROR( "[PackageCook] {}: a Shader Override names shader {} ({}), which no "
                                       "registered shader carries - its permutations were not cooked",
                                       row.Key, block->Shader->Guid, block->Shader->Path );
                            ++stats.Failures;
                            continue;
                        }
                        overrides.push_back( shader->Path.stem().string() );
                    }
                }
                append( Graphic::System::MeshRenderer::OverrideProgramVariants( *shaders, overrides ) );
            }
            else
            {
                LOG_ERROR( "[PackageCook] no shader service: the cloud media's, materials' and shader overrides' "
                           "permutations were not cooked" );
                ++stats.Failures;
            }

            const fs::path tableFile = ::Common::Constants::Path::CONFIG_PATH / "Scalability.json";
            const auto     text      = Common::Utils::FileSystem::ReadFileContent( tableFile );
            const auto     table     = text ? Scalability::ScalabilityTable::Parse( text.GetValue() )
                                            : Common::MakeError<Scalability::ScalabilityTable>( text.GetError() );
            if ( !table )
            {
                LOG_ERROR( "[PackageCook] {}: the screen-probe permutations its levels select were not cooked: {}",
                           tableFile.string(), table.GetError() );
                ++stats.Failures;
            }
            else
            {
                std::set<std::pair<uint32_t, uint32_t>> layouts;
                for ( std::size_t level = 0; level < Scalability::kLevelCount; ++level )
                {
                    const auto at = static_cast<Scalability::Level>( level );
                    layouts.emplace(
                         static_cast<uint32_t>( table.GetValue().ValueAt( Scalability::Parameter::ScreenProbeDownsample, at ) ),
                         static_cast<uint32_t>( table.GetValue().ValueAt( Scalability::Parameter::ScreenProbeTracedTexels, at ) ) );
                }
                for ( const auto& [downsample, tracedTexels] : layouts )
                    append( GI::ScreenProbeGather::ProgramVariants( downsample, tracedTexels ) );
            }

            // THE CENSUS: every ShippedByCook permutation this session ACQUIRED (ShaderService::ShippedByCookRequests)
            // must be in the cooked set, or the package refuses it by name at runtime. Named and failed here instead.
            if ( const auto shaders = Runtime::ResourceRegistry::GetShaderService() )
            {
                std::set<std::pair<std::string, uint64_t>> cookedKeys;
                for ( const auto& entry : cooked )
                    cookedKeys.emplace( entry.Program, entry.Variant.Hash() );
                for ( const auto& request : shaders->ShippedByCookRequests() )
                {
                    if ( cookedKeys.contains( { request.Program, request.Variant.Hash() } ) )
                        continue;
                    std::string defines;
                    for ( const std::string& define : request.Variant.Defines )
                        defines += ( defines.empty() ? "" : " " ) + define;
                    LOG_ERROR( "[PackageCook] {} [{:016x}] ({}) was acquired as ShippedByCook but no cook list names "
                               "it - the package would refuse it; add it to the acquiring system's ProgramVariants",
                               request.Program, request.Variant.Hash(), defines );
                    ++stats.Failures;
                }
            }

            const std::vector<fs::path> programs = PackagedShaderPrograms( developerInstruments );
            for ( const ::Desert::Core::CookedProgramVariant& entry : cooked )
                CookVariant( entry, programs, spirvDebugInfo, stats );
        }

        void CookFonts( CookStats& stats )
        {
            for ( const fs::path* root : Runtime::FontScanRoots() )
            {
                for ( const auto& p : Common::Utils::FileSystem::ListFilesRecursive( *root ) )
                {
                    if ( p.extension() != ".ttf" ) // FontService::EnsurePreloaded's own filter
                        continue;

                    // Ф3 made the read primitives return a ResultStr, so a miss is a NAMED refusal rather
                    // than an empty vector indistinguishable from an empty file. This site wants the
                    // same policy it always had - count the failure and move on to the next font - but
                    // it now says WHY in the log instead of swallowing the reason.
                    const auto ttfRead = Common::Utils::FileSystem::ReadByteFileContent( p );
                    if ( !ttfRead )
                    {
                        LOG_ERROR( "[PackageCook] '{}' could not be read, not baking it: {}", p.generic_string(),
                                   ttfRead.GetError() );
                        ++stats.Failures;
                        continue;
                    }
                    const auto& ttf = ttfRead.GetValue();
                    if ( ttf.empty() )
                    {
                        LOG_ERROR( "[PackageCook] '{}' is an empty file, not baking it.", p.generic_string() );
                        ++stats.Failures;
                        continue;
                    }

                    // The bake the runtime asks for on a text-bearing first frame: the default size,
                    // ASCII only. A scene using glyphs beyond ASCII re-bakes once at runtime (a
                    // different glyph set is a different key by design); the shipped default covers
                    // the common case and the default font.
                    const uint64_t  key = Text::FontCacheKey( ttf, Text::kDefaultBakePixelHeight, {} );
                    Text::BakedFont existing;
                    if ( Text::TryLoadBakedFont( Text::FontCachePath( key ), existing ) )
                    {
                        ++stats.FontsCached;
                        continue;
                    }

                    const Text::BakedFont baked = Text::BakeFontForCache( ttf, Text::kDefaultBakePixelHeight, {} );
                    if ( !baked.Valid() )
                    {
                        LOG_ERROR( "[PackageCook] {} could not be baked into an SDF atlas", p.string() );
                        ++stats.Failures;
                        continue;
                    }
                    if ( !Text::StoreBakedFont( Text::FontCachePath( key ), baked ).IsSuccess() )
                    {
                        LOG_ERROR( "[PackageCook] {} baked but its atlas did not reach {} — the package "
                                   "would ship a font the runtime must rebake",
                                   p.string(), Text::FontCachePath( key ).string() );
                        ++stats.StoreFailures;
                        continue;
                    }
                    ++stats.FontsBaked;
                }
            }
        }

        void CookIcons( CookStats& stats )
        {
            for ( const fs::path* root : Runtime::IconScanRoots() )
            {
                for ( const auto& p : Common::Utils::FileSystem::ListFilesRecursive( *root ) )
                {
                    if ( p.extension() != ".svg" ) // IconService::EnsurePreloaded's own filter
                        continue;
                    if ( IsEditorOnlyResource( p ) ) // not in the package, so not baked for it
                        continue;

                    const auto svgRead = Common::Utils::FileSystem::ReadByteFileContent( p );
                    if ( !svgRead )
                    {
                        LOG_ERROR( "[PackageCook] '{}' could not be read, not baking it: {}", p.generic_string(),
                                   svgRead.GetError() );
                        ++stats.Failures;
                        continue;
                    }
                    const auto& svg = svgRead.GetValue();
                    if ( svg.empty() )
                    {
                        LOG_ERROR( "[PackageCook] '{}' is an empty file, not baking it.", p.generic_string() );
                        ++stats.Failures;
                        continue;
                    }

                    const uint64_t    key = Vector::IconCacheKey( svg );
                    Vector::BakedIcon existing;
                    if ( Vector::TryLoadBakedIcon( Vector::IconCachePath( key ), existing ) )
                    {
                        ++stats.IconsCached;
                        continue;
                    }

                    const Vector::BakedIcon baked = Vector::BakeIconSdf( svg.data(), svg.size() );
                    if ( !baked.Valid() )
                    {
                        LOG_ERROR( "[PackageCook] {} has no filled shapes the icon importer understands",
                                   p.string() );
                        ++stats.Failures;
                        continue;
                    }
                    if ( !Vector::StoreBakedIcon( Vector::IconCachePath( key ), baked ).IsSuccess() )
                    {
                        LOG_ERROR( "[PackageCook] {} baked but its SDF did not reach {} — the package "
                                   "would ship an icon the runtime must rebake",
                                   p.string(), Vector::IconCachePath( key ).string() );
                        ++stats.StoreFailures;
                        continue;
                    }
                    ++stats.IconsBaked;
                }
            }
        }
        void CookTextures( CookStats& stats )
        {
            // THE EDITOR'S IMPORTER, NOT A SECOND COOK. Decode, mip chain, the BC7 gates, the intent
            // file, the freshness key and the path formula all live in TextureImporter; a packager copy
            // of any of them would be a second opinion on what a shipped texture is. Each failure was
            // logged where it happened, with the file's name.
            // Every texture asset the package carries gets its platform data into the DDC here; the
            // Texture bucket is then staged like every other (StageCookedEntries), and the game reads it
            // there -- it has no builder, so an entry missing now is a load error in the game.
            // THE SET IS EVERY TEXTURE ASSET UNDER THE ASSETS ROOT, plus the loose images. The loose roots
            // (Textures/, Meshes/) are where IMAGES get imported; a texture asset written by a tool rather than
            // imported from an image — Build HLODs' baked `<world>_HLOD/T_<tile>_BaseColor.detex` beside its
            // world — lives wherever its owner does, and the package carries it all the same. Listing only the
            // loose roots shipped those with no platform data unless an editor session had happened to draw
            // them ("has no platform data under DDC key ..., the package was cooked without it"). Assets only,
            // never images outside the loose roots: importing an image creates its asset on disk, which a
            // package cook must not do.
            std::vector<fs::path> sources = LooseTextureSources();
            for ( const auto& p :
                  Common::Utils::FileSystem::ListFilesRecursive( Common::Constants::Path::ASSETS_PATH ) )
            {
                if ( p.extension() == Assets::kTextureAssetExtension && Assets::IsTextureSourceAssetFile( p ) )
                    sources.push_back( p );
            }
            std::sort( sources.begin(), sources.end() );
            sources.erase( std::unique( sources.begin(), sources.end() ), sources.end() );

            TextureImporter importer;
            for ( const fs::path& source : sources )
            {
                switch ( importer.Cook( source ).Outcome )
                {
                    case TextureCookOutcome::Cooked:
                        ++stats.TexturesCooked;
                        break;
                    case TextureCookOutcome::Fresh:
                        ++stats.TexturesCached;
                        break;
                    case TextureCookOutcome::Failed:
                        ++stats.Failures;
                        break;
                    case TextureCookOutcome::Unwritten:
                        ++stats.StoreFailures;
                        break;
                }
            }

            // THE ROWS GO WHERE THE RUNTIME READS THEM. The texture cook entered every `.tex` into the
            // content registry as it went (WriteCookedBytes on a write, NoteFile on a fresh one), and the
            // player's preload asks the registry for textures rather than listing a directory — so a row
            // that stays in this process's memory is a texture the package carries and the game never
            // loads. A registry that could not be written is counted as an unwritten artifact for the
            // same reason a SPIR-V blob is.
            if ( Assets::ContentRegistry::Dirty() )
            {
                if ( const auto saved = Assets::ContentRegistry::Save(); !saved )
                {
                    LOG_ERROR( "[PackageCook] the cooked textures' registry rows were not written: {} — the "
                               "package would carry textures its runtime cannot find",
                               saved.GetError() );
                    ++stats.StoreFailures;
                }
            }
        }

        // THE COOK WRITES THE MESH'S DERIVED DATA (UE: the cooker builds every static mesh's render data, and the
        // game never sees its source). A packaged game registers no mesh builder, so every static mesh it can
        // load must already have its imported envelope (kMeshSourceDeriver) and its render data (kMeshDeriver)
        // in the DDC buckets StageCookedEntries ships - before this, only the meshes an editor session had
        // happened to draw on the packaging machine had them. The editor's own builders are installed here
        // (the same two EditorStartup installs), so the packager tool cooks through the importers and not a
        // copy of them. THE SET is every static mesh asset under the assets root: each `.stmesh` file
        // (hand-authored, edited import, a split import's node mesh) and the asset each whole-file StaticMesh
        // import record stands for (IsWholeFileMeshImport: since AF4h it has no file of its own; StageTree stages
        // its cooked envelope beside the record).
        void CookMeshes( CookStats& stats )
        {
            Assets::SetMeshPlatformDataBuilder( &Editor::BuildMeshPlatformData );
            Assets::SetMeshSourceBuilder( &Editor::BuildMeshSourceOnMiss );

            std::vector<fs::path> assets;
            for ( const auto& p :
                  Common::Utils::FileSystem::ListFilesRecursive( Common::Constants::Path::ASSETS_PATH ) )
            {
                if ( p.extension() == ".stmesh" )
                {
                    assets.push_back( p );
                    continue;
                }
                if ( !Common::Content::IsImportRecord( p ) )
                    continue;
                const auto wholeFile = IsWholeFileMeshImport( p );
                if ( !wholeFile.IsSuccess() )
                {
                    LOG_ERROR( "[PackageCook] import record '{}' cannot be read for the mesh cook: {}", p.string(),
                               wholeFile.GetError() );
                    ++stats.Failures;
                    continue;
                }
                if ( wholeFile.GetValue() ) // a split import's node meshes are the `.stmesh` files listed above
                    assets.push_back( Common::Content::MeshAssetOfImportRecord( p ) );
            }
            std::sort( assets.begin(), assets.end() );
            assets.erase( std::unique( assets.begin(), assets.end() ), assets.end() );

            for ( const fs::path& asset : assets )
            {
                const auto platformData = Assets::LoadMeshPlatformDataKeyed( asset );
                if ( !platformData.IsSuccess() )
                {
                    LOG_ERROR( "[PackageCook] static mesh '{}' was not cooked: {}", asset.string(),
                               platformData.GetError() );
                    ++stats.Failures;
                    continue;
                }
                ++stats.MeshesCooked;
            }

            // EVERY COOKED MESH'S DISTANCE FIELD AND CARDS (UE: the cooker builds them with the mesh): a packaged
            // game registers no builder, so a field or cards no editor session happened to derive was a miss
            // there and the mesh left the distance-field scene / the surface cache. Derived through
            // StaticMeshAsset::DeriveDistanceFieldAndCardsNow - the very derivation the editor's
            // ResolveDependencies queues, so the keys are the ones the runtime asks for - with the editor's two
            // builders (EditorStartup). Its material sidedness input reads the materials and their templates, so
            // this cook's own asset manager holds every registry shader parsed (CPU only, no device).
            Assets::SetMeshDistanceFieldBuilder( &Assets::BuildMeshDistanceFieldBytes );
            Assets::SetMeshCardsBuilder( &Assets::BuildMeshCardsBytes );
            const auto manager = std::make_shared<Assets::AssetManager>();
            for ( const auto& shader :
                  Assets::ContentRegistry::FilesOfKind( Common::Content::ContentKind::Shader ) )
                (void)manager->CreateAsset<Assets::ShaderAsset>( shader );
            for ( const fs::path& asset : assets )
            {
                const auto mesh = std::make_shared<Assets::StaticMeshAsset>( asset );
                if ( const auto loaded = mesh->Load(); !loaded )
                {
                    LOG_ERROR(
                         "[PackageCook] static mesh '{}' could not be read for its distance field and cards: {}",
                         asset.string(), loaded.GetError() );
                    ++stats.Failures;
                    continue;
                }
                if ( const auto derived = mesh->DeriveDistanceFieldAndCardsNow( *manager ); !derived )
                {
                    LOG_ERROR( "[PackageCook] static mesh '{}' has no cooked distance field / cards: {}",
                               asset.string(), derived.GetError() );
                    ++stats.Failures;
                    continue;
                }
                ++stats.MeshFieldsCooked;
            }
        }

        // Copies the DDC buckets a game reads into the cooked tree under the key the runtime asks for:
        // Saved/Cooked/<Platform>/Cooked/Buckets/... is packed as "Cooked/Buckets/...", which is
        // DDC::PackagedPath of the loose entry — the one lookup a packaged DDC reader falls back to.
        // WHOLE BUCKETS, not a list of what this pass touched: a fixture or an earlier cook's entry under a
        // key the runtime will ask for is exactly as valid (the key is its inputs). The driver pipeline blob
        // is per user and per device and lives in the player's own directory (PipelineCacheFile.hpp), so no
        // cook ships one.
        //
        // The set of buckets is Assets::ShippedDDCBuckets() (DDCShipRegister.hpp), not a literal array here:
        // a hand-written list is exactly what let kMeshDeriver's mesh-render-data bucket go unshipped (PK3) -
        // nothing forced this list to grow when a deriver did. Editor-only buckets (Thumbnails - no
        // Common::DDC::Deriver at all, read only by the asset browser) are excluded by the register, by name,
        // with a reason; see DDCShipRegister.hpp.
        void StageCookedEntries( CookStats& stats )
        {
            const std::vector<std::string_view> shippedBuckets = Assets::ShippedDDCBuckets();
            const fs::path  cooked  = Common::DDC::PlatformCookedDir() / Common::Constants::Path::COOKED_DIR_NAME;
            const fs::path  ddcRoot = Common::DDC::Root();
            std::error_code ec;

            for ( const std::string_view bucket : shippedBuckets )
            {
                const fs::path dir = Common::DDC::BucketDir( bucket );
                if ( !fs::is_directory( dir, ec ) )
                    continue;
                for ( const auto& file : fs::recursive_directory_iterator( dir, ec ) )
                {
                    if ( !file.is_regular_file( ec ) )
                        continue;
                    const fs::path target = cooked / file.path().lexically_normal().lexically_relative( ddcRoot );
                    fs::create_directories( target.parent_path(), ec );
                    fs::copy_file( file.path(), target, fs::copy_options::overwrite_existing, ec );
                    if ( ec )
                    {
                        LOG_ERROR( "[PackageCook] could not stage {} into {}: {}", file.path().string(),
                                   target.string(), ec.message() );
                        ++stats.StoreFailures;
                    }
                }
            }
        }
    } // namespace

    // The same enumeration + extension filter the engine shader boot used (lowercased
    // extension over ListFilesRecursive of the live SHADERDIR_PATH): the cook must see exactly
    // the set of shaders the runtime will register, or a shader the runtime compiles at startup
    // is one the cook silently skipped.
    std::vector<fs::path> PackagedShaderPrograms( bool developerInstruments )
    {
        std::vector<fs::path> out;
        for ( const auto& candidate :
              Common::Utils::FileSystem::ListFilesRecursive( Common::Constants::Path::SHADERDIR_PATH ) )
        {
            std::string ext = candidate.extension().string();
            std::transform( ext.begin(), ext.end(), ext.begin(), ::tolower );
            // A program the package leaves out (PackagedContentTrees.hpp: editor-only, or developer-only
            // under a Shipping runtime) is not in the package, so it is not cooked for it.
            if ( ext == ".shader" && !IsLeftOutOfPackage( candidate, developerInstruments ) )
                out.push_back( candidate );
        }
        return out;
    }

    CookStats CookContentCaches( bool spirvDebugInfo, bool developerInstruments )
    {
        CookStats stats;
        // THE ONE COOKED TREE starts empty: a package never carries a previous cook's leftovers, and
        // everything the packager packs is written into it by this cook and by StageShippedContent.
        std::error_code wipe;
        fs::remove_all( Common::DDC::PlatformCookedDir(), wipe );
        CookShaders( spirvDebugInfo, developerInstruments, stats );
        CookShaderVariants( spirvDebugInfo, developerInstruments, stats );
        CookFonts( stats );
        CookIcons( stats );
        CookTextures( stats );
        CookMeshes( stats );
        StageCookedEntries( stats );

        LOG_INFO( "[PackageCook] shaders {} compiled / {} cached, fonts {} baked / {} cached, icons {} "
                  "baked / {} cached, textures {} cooked / {} cached, static meshes {} cooked ({} with distance "
                  "field + "
                  "cards), {} failure(s), {} "
                  "unwritten",
                  stats.ShadersCompiled, stats.ShadersCached, stats.FontsBaked, stats.FontsCached,
                  stats.IconsBaked, stats.IconsCached, stats.TexturesCooked, stats.TexturesCached,
                  stats.MeshesCooked, stats.MeshFieldsCooked, stats.Failures, stats.StoreFailures );
        if ( stats.StoreFailures > 0 )
        {
            // Loud on its own line: this one is never normal, and a package built over it ships a
            // cache with holes in it that only a player's slow startup would ever reveal.
            LOG_ERROR( "[PackageCook] {} cooked artifact(s) could not be written — the package will ship "
                       "an incomplete cache and the game will rebuild them at every start",
                       stats.StoreFailures );
        }
        return stats;
    }

    Common::BoolResultStr StageCookedTextureAsset( const fs::path& editorAsset, const fs::path& target )
    {
        const auto raw = Common::Utils::FileSystem::ReadFileContent( editorAsset );
        if ( !raw.IsSuccess() )
            return Common::MakeError<bool>( raw.GetError() );
        const auto cooked = Assets::CookTextureAssetForRuntime( std::span<const std::byte>(
             reinterpret_cast<const std::byte*>( raw.GetValue().data() ), raw.GetValue().size() ) );
        if ( !cooked.IsSuccess() )
            return Common::MakeFormattedError<bool>( "texture asset '{}' cannot be cooked for the package: {}",
                                                     editorAsset.string(), cooked.GetError() );
        std::error_code ec;
        fs::create_directories( target.parent_path(), ec );
        const auto written = Common::Utils::FileSystem::WriteBytesToFileAtomic(
             target, std::span<const std::byte>( reinterpret_cast<const std::byte*>( cooked.GetValue().data() ),
                                                 cooked.GetValue().size() ) );
        if ( !written.IsSuccess() )
            return Common::MakeFormattedError<bool>( "the cooked texture asset '{}' was not written: {}",
                                                     target.string(), written.GetError() );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr StageCookedImportedMesh( const fs::path& importRecord, const fs::path& target )
    {
        const fs::path asset      = Common::Content::MeshAssetOfImportRecord( importRecord );
        const fs::path source     = importRecord.parent_path() / importRecord.stem();
        const auto     recordGuid = Assets::Serialization::ReadImportRecordGuid( source );
        if ( !recordGuid.IsSuccess() )
            return Common::MakeFormattedError<bool>( "imported mesh '{}' cannot be staged: {}", asset.string(),
                                                     recordGuid.GetError() );
        const auto envelope = Assets::LoadMeshSourceAsset( asset );
        if ( !envelope.IsSuccess() )
            return Common::MakeFormattedError<bool>( "imported mesh '{}' cannot be staged: {}", asset.string(),
                                                     envelope.GetError() );
        if ( envelope.GetValue().Guid != recordGuid.GetValue() )
            return Common::MakeFormattedError<bool>(
                 "imported mesh '{}' cannot be staged: its envelope states GUID {} but its import record '{}' "
                 "states {}",
                 asset.string(), Common::Content::AssetGuidToText( envelope.GetValue().Guid ),
                 importRecord.string(), Common::Content::AssetGuidToText( recordGuid.GetValue() ) );
        std::error_code ec;
        fs::create_directories( target.parent_path(), ec );
        if ( auto written = Assets::WriteMeshSourceAssetFile( target, envelope.GetValue() ); !written.IsSuccess() )
            return Common::MakeFormattedError<bool>( "the cooked mesh asset '{}' was not written: {}",
                                                     target.string(), written.GetError() );
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<bool> IsWholeFileMeshImport( const fs::path& importRecord )
    {
        const fs::path source = importRecord.parent_path() / importRecord.stem(); // base.fbx.deimport -> base.fbx
        const auto     kind   = Assets::Serialization::ReadImportRecordKind( source );
        if ( !kind.IsSuccess() )
            return Common::MakeFormattedError<bool>( "{}", kind.GetError() );
        if ( kind.GetValue() != Common::Content::ContentKind::StaticMesh )
            return Common::MakeSuccess( false );
        const auto record = Assets::Serialization::ReadImportRecord( source );
        if ( !record.IsSuccess() )
            return Common::MakeFormattedError<bool>( "{}", record.GetError() );
        if ( !record.GetValue().has_value() )
            return Common::MakeFormattedError<bool>( "'{}' has no import record", source.generic_string() );
        return Common::MakeSuccess( !record.GetValue()->Nodes.has_value() );
    }
} // namespace Desert::Editor
