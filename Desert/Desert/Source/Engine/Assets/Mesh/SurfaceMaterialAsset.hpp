#pragma once

#include <Engine/Assets/MaterialAsset.hpp>
#include <Engine/Assets/MaterialData.hpp>
#include <Engine/Assets/TextureAsset.hpp>

namespace Desert::Assets
{
    // The concrete material asset (.demat): a shader + generic parameter values (MaterialData —
    // THE single material protocol; the shader's schema defines what the params mean).
    // "StaticMeshPBR" (the default) routes to the optimized batched backend; any Surface-domain
    // DSL shader routes to the generic per-object path. Pre-protocol files (typed PBR fields /
    // the ancient cooker format) are migrated on Load and the file is upgraded on disk.
    class SurfaceMaterialAsset final : public MaterialAsset
    {
    public:
        SurfaceMaterialAsset( AssetPriority priority, const Common::Filepath& filepath );

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

        virtual std::string GetShaderName() const override
        {
            return m_Data.EffectiveShaderName();
        }

        virtual Common::UUID GetMaterialUUID() const override
        {
            return m_MaterialUUID;
        }

    private:
        // Upgrades the path-derived handle AssetBase installed to the in-file MaterialId when the file has
        // one — asset-database identity that survives renames as well as restarts.
        void AdoptStableHandle();

        bool         m_ReadyForUse  = false;
        Common::UUID m_MaterialUUID = Common::UUID::Null();
        MaterialData m_Data;

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
