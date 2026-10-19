#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/VFX/VFXModuleCatalog.hpp>
#include <Engine/VFX/VFXModuleFormat.hpp>

#include <vector>

namespace Desert::Assets
{
    /**
     * @brief A VFX module on disk (`.dfxmod`) - UE's UNiagaraScript of Usage Module (VFX-MODULE-ASSET).
     *
     * ITS HANDLE IS HandleForGuid OF ITS HEADER GUID (the first-line comment header, as a `.shader`'s), adopted in
     * the constructor, so a stack row's {Guid, Path} survives a rename. The stack compiler does not go through
     * this object - it reads the file of the GUID's registry row (VFX::RegistryModuleResolver) - so the asset is
     * what the module document edits and what the registry and the handle census see.
     */
    class VFXModuleAsset final : public AssetBase
    {
    public:
        VFXModuleAsset( const Common::Filepath& filepath );

        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        [[nodiscard]] bool IsReadyForUse() const override
        {
            return m_Ready;
        }

        [[nodiscard]] const Common::Content::AssetGuid& Guid() const
        {
            return m_Guid;
        }

        [[nodiscard]] const VFX::VFXModuleDefinition& GetDefinition() const
        {
            return m_Definition;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::VFXModule;
        }

        /// The file's text, VFS-aware (a packaged game reads it from its pak): what LoadFromFile and the
        /// registry resolver parse.
        static Common::ResultStr<std::string> ReadText( const std::filesystem::path& file );

        /// Validates @p module (it must parse back) and writes its canonical text.
        static Common::BoolResultStr Save( const Common::Filepath&         filepath,
                                           const VFX::VFXModuleDefinition& module );

    private:
        Common::Content::AssetGuid m_Guid;
        VFX::VFXModuleDefinition   m_Definition;
        bool                       m_Ready = false;
    };

    /**
     * @brief The engine's answer to "which module is this GUID": the ContentRegistry row of the GUID (a `.dfxmod`
     * in the engine module library or in the project's Content, ScanRootsOf's two roots), its file read VFS-aware
     * and parsed. A GUID with no row, a row of another kind, or a file that does not parse is an error naming
     * the GUID and the file.
     */
    class RegistryVFXModuleResolver final : public VFX::VFXModuleResolver
    {
    public:
        [[nodiscard]] Common::ResultStr<VFX::VFXModuleDefinition>
        Resolve( const Common::Content::AssetGuid& guid ) const override;
    };

    /// THE MODULE CATALOG the VFX System editor offers (UE: the module picker): every VFXModule row of the
    /// ContentRegistry, engine library and project Content alike, resolved and sorted by category then name.
    /// A row that does not resolve is an error naming it — a picker that silently drops a module hides it.
    Common::ResultStr<std::vector<VFX::VFXModuleInfo>> ReadVFXModuleCatalog();
} // namespace Desert::Assets
