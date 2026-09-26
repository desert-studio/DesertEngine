#include "SurfaceMaterialAsset.hpp"
#include <Engine/Assets/MaterialFormat.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Shader/ShaderAsset.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>
#include <Common/Content/CanonicalText.hpp>

#include <Engine/Assets/Mesh/PBRSurfaceParams.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>
#include <Engine/Assets/Serialization/Material.hpp>

#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <format>
#include <rflcpp/rfl/json.hpp>

namespace Desert::Assets
{

    SurfaceMaterialAsset::SurfaceMaterialAsset( AssetPriority priority, const Common::Filepath& filepath )
         : MaterialAsset( priority, filepath, AssetTypeID::Material )
    {
    }

    std::shared_ptr<SurfaceMaterialAsset>
    SurfaceMaterialAsset::CreateWorkingCopy( const SurfaceMaterialAsset& source )
    {
        auto copy =
             std::make_shared<SurfaceMaterialAsset>( source.m_Metadata.Priority, source.m_Metadata.Filepath );

        copy->m_Data       = source.m_Data;
        copy->m_ShaderName = source.m_ShaderName;
        // Carried over so a working copy of a material that is running on substituted defaults refuses to
        // save for the same reason its source does. Nothing saves the copy today, and this is what keeps
        // that true if something ever tries.
        copy->m_RunningOnSubstitutedDefaults = source.m_RunningOnSubstitutedDefaults;

        // Generated, and named as generated. A random id here is the one thing that keeps this copy out of
        // every map the subject is in — see the header for what a shared one would do to the mesh ->
        // material link. Both are written from ONE value because Load() maintains exactly that
        // equality (m_MaterialUUID = m_Metadata.Handle, adopted from the header GUID), and a copy that
        // broke it would resolve differently depending on which of the three a caller happened to ask.
        const Common::UUID identity = Common::UUID::Generate();
        copy->m_Metadata.Handle     = identity;
        copy->m_MaterialUUID        = identity;
        // Not the same asset either: a copy that kept the source's header GUID would state its identity.
        copy->m_Data.Header = std::nullopt;

        // Never Load()ed, so nothing else would set this — and an asset that is not ready for use is
        // skipped by everything that would draw it.
        copy->m_ReadyForUse = true;
        // And never READABLE from a file either: this is the flag eviction asks before it would release
        // the copy and let a reload steal the subject's identity. See Unload().
        copy->m_IsWorkingCopy = true;
        return copy;
    }

    void SurfaceMaterialAsset::AdoptStableHandle()
    {
        // Asset-database identity: the internal handle must be STABLE across editor runs so handle-based
        // references (scene GUIDs, canon textures, service maps) survive restarts. A GUID persisted INSIDE
        // the file is the better identity because it survives renames and moves as well, so it wins when it
        // is there. When it is not, the path-derived handle AssetBase already installed stands — which is
        // why there is no `else` here.
        //
        // THROUGH AdoptHandleFromFile so the handle->path inverse learns the adopted number too. The
        // path-derived one the constructor installed is already in the index; this one replaces it as the
        // material's identity, and it is the number every `Parent` GUID in shipped content folds to — so an index
        // that knew only the derived one would be empty for exactly the references that exist.
        if ( const auto guid = m_Data.Guid(); !guid.IsNull() )
            AdoptHandleFromFile( MaterialData::HandleOf( guid ),
                                 Common::AssetHandle::StableKeyForPath( m_Metadata.Filepath ) );
    }

    void SurfaceMaterialAsset::ResolveShader( const AssetManager* manager )
    {
        if ( !m_Data.Shader.has_value() )
        {
            m_ShaderName = std::string( kDefaultShaderName );
            return;
        }
        m_ShaderName.clear();
        if ( manager == nullptr )
            return;
        const std::string context = std::format( "material '{}'", m_Metadata.Filepath.generic_string() );
        const auto        name = FindShaderNameByRef( *manager, *m_Data.Shader, { "shader", "Shader", context } );
        if ( !name )
        {
            LOG_ERROR( "{}; the material draws nothing until it names one", name.GetError() );
            return;
        }
        m_ShaderName = name.GetValue();
    }

    Common::BoolResultStr SurfaceMaterialAsset::StateShaderByName( MaterialData& data, const AssetManager& manager,
                                                                   std::string_view name )
    {
        if ( name == kDefaultShaderName )
        {
            data.SetShader( {}, {} );
            return BOOLSUCCESS;
        }
        const auto ref = FindShaderRefByName( manager, name, { "shader", "Shader", "the edited material" } );
        if ( !ref )
            return Common::MakeError( ref.GetError() );
        const auto guid = Common::Content::AssetGuidFromText( ref.GetValue().Guid );
        if ( !guid )
            return Common::MakeError( guid.GetError() );
        data.SetShader( guid.GetValue(), ref.GetValue().Path );
        return BOOLSUCCESS;
    }

    void SurfaceMaterialAsset::ResolveDependencies( AssetManager& manager )
    {
        ResolveShader( &manager );
    }

    Common::BoolResultStr SurfaceMaterialAsset::LoadFromFile()
    {
        const auto raw = Common::Utils::FileSystem::ReadFileContent( m_Metadata.Filepath );

        const auto finalize = [this]()
        {
            AdoptStableHandle();
            ResolveShader( nullptr );

            // The EXTERNAL id is the handle, always. When the file carries a header GUID the two are the
            // same value by AdoptStableHandle; when it does not, they are the same path-derived value. The
            // external id used to be left unset in that second case, which under the old random default
            // meant MaterialService keyed such a material under a number that changed every launch — the
            // mesh->material link resolved through GetAssetHandleByExternal and missed after a restart.
            m_MaterialUUID = m_Metadata.Handle;

            m_ReadyForUse = true;
        };

        if ( !raw || raw.GetValue().empty() )
        {
            // New / empty material — canonical defaults; editable and re-savable. A MISSING file is
            // this branch by design too: the editor creates a material by naming a file that does
            // not exist yet (pinned by the AssetMissingFile suite).
            m_Data = MaterialData{};

            // BUT a file that IS there and could not be read is NOT a new material. Both used to land
            // here identically, with the reason ReadFileContent gave thrown away — so a .demat locked
            // by permissions, racing a delete or coming out of a truncated pak was presented to the
            // editor as a blank material somebody had just made, and the next save wrote that over it.
            // Exists() asks the VFS as well, so a packaged game answers this the same way.
            if ( !raw && Common::Utils::FileSystem::Exists( m_Metadata.Filepath ) )
            {
                LOG_ERROR( "[SurfaceMaterialAsset] '{}' EXISTS but could not be read ({}) — rendering "
                           "with DEFAULTS; the authored parameters are not applied and this material "
                           "will refuse to save over the file.",
                           m_Metadata.Filepath.string(), raw.GetError() );
                m_RunningOnSubstitutedDefaults = true;
            }

            finalize();
            return BOOLSUCCESS;
        }

        // The unified MaterialData protocol is the ONLY on-disk format (pre-protocol migration
        // readers were removed with the rest of the legacy paths).
        const auto parsed = ParseMaterialJson( m_Metadata.Filepath.generic_string(), raw.GetValue() );
        if ( parsed )
        {
            m_Data                         = parsed.GetValue();
            m_RunningOnSubstitutedDefaults = false; // a reload that parses clears a previous failure
            finalize();
            return BOOLSUCCESS;
        }

        // Nothing parsed — keep the editor usable with defaults.
        //
        // THIS BRANCH RETURNS SUCCESS DELIBERATELY, and the reason belongs here rather than in a
        // review comment: AssetManager::CreateAsset drops the asset entirely when Load answers an
        // error, so an unparseable .demat would vanish from the asset database, every mesh slot
        // pointing at it would resolve to nothing, and the user would be shown an empty material
        // picker instead of a material they can look at and fix. Refusing to load is a worse answer
        // than loading degraded — for the FILE, though, not for the DATA: the half that was actually
        // destructive is the re-save this message used to warn about while nothing could stop it, and
        // that is now refused by Save() below. Whether an unloadable asset should additionally mark
        // the whole SCENE as degraded is a larger change to the load path (audit Д31-8) and is not
        // decided here.
        LOG_ERROR( "[SurfaceMaterialAsset] {} — rendering with DEFAULTS; authored parameters are NOT "
                   "applied and this material will refuse to save over the file.",
                   parsed.GetError() );
        m_Data                         = MaterialData{};
        m_RunningOnSubstitutedDefaults = true;
        finalize();
        return BOOLSUCCESS;
    }

    Common::ResultStr<std::string> SurfaceMaterialAsset::Save() const
    {
        if ( m_RunningOnSubstitutedDefaults )
            return Common::MakeFormattedError<std::string>(
                 "'{}' is running on substituted defaults because its file could not be read or parsed; "
                 "writing them out would destroy the authored parameters permanently. Fix or delete the "
                 "file first.",
                 m_Metadata.Filepath.string() );

        // A NUMBER THAT IS NOT A NUMBER IS REFUSED HERE, BY NAME, AND THE ALTERNATIVE IS NOT A BAD FILE.
        //
        // `rfl::json::write` is `std::string( yyjson_mut_write( … ) )` and yyjson with no write flags
        // REFUSES a NaN or an infinity — it returns a null pointer, which that constructor then reads.
        // Measured on the vendored copy: `{"v":0.5}` writes, NaN and inf both answer NULL. So a material
        // holding one does not produce a broken `.demat`; it produces undefined behaviour inside a
        // third-party header while the editor is saving the artist's work.
        //
        // IT IS REACHABLE, and not through the file: yyjson refuses those tokens on the way IN too, so no
        // `.demat` on disk can carry one. The editor can. Every material row is an ImGui drag
        // (MaterialEditorPanel's DragScalarN / SliderScalarN), ImGui's Ctrl-click text entry parses with
        // `sscanf( buf, "%f", … )`, and `%f` accepts `nan`, `inf` and `1e40`. One typed word and Save is a
        // null dereference.
        //
        // ALL FOUR LANES, because all four are what gets written. The renderer's reader asks the same
        // question of the lanes IT will use (Graphic::MaterialValueIsFinite, shared so the two cannot
        // disagree about what a number is) and keeps the shader's default for the parameter; this one
        // cannot fall back to anything, because the caller asked for the bytes of THIS material.
        for ( const auto& param : m_Data.Params )
        {
            if ( Graphic::MaterialValueIsFinite( param.Value, Graphic::MaterialValueLanes::Four ) )
                continue;

            return Common::MakeFormattedError<std::string>(
                 "refusing to write '{}': parameter '{}' holds ({}, {}, {}, {}), which JSON has no "
                 "spelling for — the writer would dereference a null pointer rather than produce a file. "
                 "Reset that parameter or type a number into it.",
                 m_Metadata.Filepath.string(), param.Name, param.Value.x, param.Value.y, param.Value.z,
                 param.Value.w );
        }

        return WriteMaterialJson( m_Data );
    }

    Common::BoolResultStr SurfaceMaterialAsset::Unload()
    {
        // A WORKING COPY IS NOT EVICTABLE, AND THE REFUSAL IS THE POINT. `CreateWorkingCopy` builds an
        // asset that was never `Load()`ed, sets `m_ReadyForUse` by hand and mints a FRESH identity so it
        // stays out of every map the subject is in. Unloading one would flip that flag, and the next
        // `EnsureLoaded` would run `Load()` against the SOURCE's filepath — which calls
        // `AdoptStableHandle()` and would reassign the copy's handle from the file's header GUID, i.e. the
        // working copy would silently take over the subject's identity in MaterialService's maps. That is
        // precisely the defect the fresh id exists to prevent, arrived at from the other direction.
        if ( !IsReloadableFromFile() )
        {
            return Common::MakeFormattedError<bool>(
                 "'{}' is a Material Editor working copy: it was filled in memory and never read from a "
                 "file, so a reload would take its identity from the source material and hand every mesh "
                 "in the level to the editor's copy. The asset stays resident.",
                 m_Metadata.Filepath.string() );
        }

        // WAS A FLAG FLIP ONLY, while the class owns a whole `MaterialData` — the parameter list, the
        // name -> handle texture map and the shader name, all heap. Released now.
        //
        // `m_MaterialUUID` and `m_Metadata.Handle` deliberately survive: they are the id every mesh
        // submesh and every service map names this material by (AssetBase::Unload, rule 3).
        m_Data = MaterialData{};
        ResolveShader( nullptr );
        // Not cleared, and it must not be: it records that this session's values were SUBSTITUTED because
        // the file would not parse. Clearing it here would let a later Save() write the defaults over the
        // authored file — the exact loss Save()'s own refusal exists to prevent.
        m_ReadyForUse = false;
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
