#pragma once

#include <Engine/Assets/MaterialAsset.hpp>
#include <Engine/Assets/MaterialData.hpp>
#include <Engine/Assets/TextureAsset.hpp>

namespace Desert::Assets
{
    // The concrete material asset (.demat): a shader + generic parameter values (MaterialData —
    // THE single material protocol; the shader's schema defines what the params mean).
    // The template is named by the shader file's GUID and nothing else; there is no default template
    // (a material that names none is refused on load). The two engine PBR templates route to the batched
    // backend; any other Surface-domain DSL shader routes to the generic per-object path. Pre-protocol files
    // (typed PBR fields / the ancient cooker format) are migrated on Load and the file is upgraded on disk.
    class SurfaceMaterialAsset final : public MaterialAsset
    {
    public:
        SurfaceMaterialAsset( const Common::Filepath& filepath );

        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        // Serialize the canonical data to .demat JSON — or REFUSE, when this asset is running on
        // substituted defaults because its file could not be read or parsed (see Load). Writing that
        // out is what turns a recoverable corruption into a permanent loss of the authored parameters,
        // and it is the exact thing Load's own error message used to warn about while returning
        // success and leaving nothing able to stop it. A Result rather than a flag beside the getter
        // because a flag has to be REMEMBERED at three call sites and this cannot be forgotten.
        [[nodiscard]] Common::ResultStr<std::string> Save() const;

        bool IsReadyForUse() const override
        {
            return m_ReadyForUse;
        }

        // A working copy has no file behind it — see CreateWorkingCopy below and Unload's refusal.
        bool IsReloadableFromFile() const override
        {
            return !m_IsWorkingCopy;
        }

        // A DETACHED IN-MEMORY COPY of this material, for an editor that must hold an edit back from the
        // scene until somebody accepts it.
        //
        // WHY A SECOND ASSET AND NOT A SECOND MaterialData. A material reaches the screen as a runtime
        // Graphic::Material built from an asset and cached by MaterialService under that asset's HANDLE,
        // and every surface drawing it — a mesh in the level and the ball in the Material Editor alike —
        // resolves to that one object. "The preview shows the edit while the scene does not" is therefore
        // not expressible by keeping a spare copy of the values somewhere: it needs a second runtime
        // material for the second audience, and a second runtime material needs an asset of its own to be
        // built from. This is that asset.
        //
        // THE IDENTITY IS FRESH, and that is the whole risk this function carries. The material UUID is the
        // EXTERNAL id every mesh submesh names its material by (MaterialService's external -> internal
        // map), so a copy that kept the source's id would take that mapping over the moment it registered
        // and every mesh in the level would start resolving to the working copy — the defect this exists to
        // remove, inverted and worse. ParentMaterialId is copied unchanged for the opposite reason: it
        // names somebody else, and an instance's working copy has to resolve through the same parent chain
        // the subject does.
        //
        // The copy is NEVER WRITTEN. It carries the source's filepath so a log line about it names
        // something a person recognises; the document saves the subject, not this.
        [[nodiscard]] static std::shared_ptr<SurfaceMaterialAsset>
        CreateWorkingCopy( const SurfaceMaterialAsset& source );

        // Canonical data — single source of truth for the editor UI, serialization and the
        // runtime material build.
        MaterialData&       Data()       { return m_Data; }
        const MaterialData& Data() const { return m_Data; }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::Material;
        }

        // THE template this material draws with, and the one place that question is answered. IDENTITY IS
        // THE HANDLE: HandleForGuid of the GUID the file states, which is the handle the ShaderAsset adopted
        // from its own header. The name is display text only — the ShaderAsset's file stem — and no decision
        // is taken on it. Both are set by ResolveDependencies (Load alone leaves them empty: the lookup needs
        // the manager). NULL after a resolve means the file names no template, or a GUID no loaded shader
        // has; both are logged naming the material, and the material draws nothing. An INSTANCE states no
        // template: its template is the parent's, which callers resolve through the parent chain.
        [[nodiscard]] Common::AssetHandle GetShaderHandle() const override
        {
            return m_ShaderHandle;
        }

        virtual std::string GetShaderName() const override
        {
            return m_ShaderName;
        }

        // The template declaring `Role PBRSurface` takes the batched PBR backend; every other template is
        // drawn through the generic data-driven path. Asked of the RESOLVED template's manifest, never of a name.
        [[nodiscard]] bool UsesCustomShader() const
        {
            return !m_ShaderIsPBRSurface;
        }

        // Resolves Data().Shader's GUID to the ShaderAsset registered under HandleForGuid of it. The loads
        // run through the manager call it; so must a caller that changes Data().Shader in memory (the
        // Material Editor's shader picker, Apply and Discard) — without it GetShaderName would keep
        // answering the shader the data no longer names.
        void ResolveDependencies( AssetManager& manager ) override;

        // States in @p data the template an editor action CHOSE BY HANDLE (a picker row, the project's
        // default surface, a role lookup): by the shader's header GUID and stable path. Every template is
        // stated explicitly — there is no default said by absence. A handle no loaded shader has, or a
        // shader file with no header GUID, is refused and @p data is left untouched.
        static Common::BoolResultStr StateShader( MaterialData& data, const AssetManager& manager,
                                                  Common::AssetHandle shader );

        virtual Common::UUID GetMaterialUUID() const override
        {
            return m_MaterialUUID;
        }

    private:
        // Upgrades the path-derived handle AssetBase installed to the in-file MaterialId when the file has
        // one — asset-database identity that survives renames as well as restarts.
        void AdoptStableHandle();

        // Every load branch runs it with no manager (a stated shader stays unresolved until
        // ResolveDependencies), ResolveDependencies with one. Non-const: the template is a DEPENDENCY, and a
        // registered-but-unread shader shell is loaded here before its manifest (Role) is asked anything.
        void ResolveShader( AssetManager* manager );

        bool         m_ReadyForUse  = false;
        Common::UUID m_MaterialUUID = Common::UUID::Null();
        MaterialData m_Data;
        std::string         m_ShaderName;                 // display only
        Common::AssetHandle m_ShaderHandle;               // THE identity of the template; null = none resolved
        bool                m_ShaderIsPBRSurface = false; // the resolved template declares `Role PBRSurface`

        // TRUE when m_Data is NOT what the file says — the file exists but could not be read, or it
        // read and would not parse. The asset is deliberately still usable in that state (see Load),
        // so this is the only thing that distinguishes "a material with default values" from "a
        // material whose values were lost this session", and Save() is what asks.
        bool m_RunningOnSubstitutedDefaults = false;

        // TRUE only for the detached copy CreateWorkingCopy makes. It exists so eviction can ask rather
        // than guess: nothing else distinguishes a working copy from a loaded material, and the two must
        // never be treated alike (Unload says what happens if they are).
        bool m_IsWorkingCopy = false;
    };
} // namespace Desert::Assets
