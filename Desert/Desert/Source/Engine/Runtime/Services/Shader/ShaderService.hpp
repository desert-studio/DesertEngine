#pragma once

#include <Engine/Assets/Shader/ShaderAsset.hpp>
#include <Engine/Graphic/Shader.hpp>

#include <unordered_set>

namespace Desert::Runtime
{
    class ShaderService
    {
    public:
        Common::BoolResultStr            Register( const std::shared_ptr<Assets::ShaderAsset>& shaderAsset );
        std::shared_ptr<Graphic::Shader> Get( const Assets::AssetHandle& handle ) const;
        std::shared_ptr<Graphic::Shader> GetByName( const std::string& name ) const;
        void                             Clear();

        /**
         * @brief The program @p name compiled under @p variant — a SECOND object for the same file,
         *        differing only in the bytes substituted for one of its includes.
         *
         * THE CALLER OWNS IT, and that is the design and not an accident. A material authoring a cloud
         * medium produces a new variant on every edit of its graph, and each variant owns VkShaderModules
         * and descriptor set layouts; a service-owned cache would grow by one program per edit for the
         * life of the session and never release one. So the service keeps only a WEAK reference: two
         * layers whose medium is the same text share one program, and the last holder dropping it is what
         * frees the modules. Hand it back by simply releasing the shared_ptr.
         *
         * The default variant is refused rather than served: that is what GetByName is for, and answering
         * it here would build a second, unregistered copy of a program that already exists.
         *
         * @return nullptr when the name is unknown, when @p variant is default, when the asset behind the
         *         name has expired, or when the substituted source DID NOT COMPILE — each logged with the
         *         name, never silently.
         *
         * The last of those was added by О1-G and is the one worth reading twice: a program with no
         * compiled stages used to be returned, on the stated ground that the caller would decide. Neither
         * caller did, and an uncompiled program reaches VulkanPipelineCompute::Invalidate, which indexes
         * element 0 of the stage list a failed compile leaves empty. An answer a caller cannot use is a
         * refusal; returning it as a success is the "empty successful answer" the contract forbids.
         */
        std::shared_ptr<Graphic::Shader> AcquireVariant( const std::string&            name,
                                                         const Graphic::ShaderVariant& variant );

        /**
         * @brief The authored body of a Volume `Medium { ... }` shader, by asset handle — the bytes a
         *        cloud material substitutes into the four programs that sample the cloud field.
         *
         * Read from the asset's CURRENT content on every call rather than cached, deliberately: hot
         * reload re-reads the asset in place, and a cached copy here would be the one thing between an
         * edited graph and a changed picture. It is a few kilobytes of text asked for once per frame at
         * most, against a shader compile if it has changed.
         *
         * @return empty when the handle names nothing or names a shader that is not a medium — logged
         *         once per handle, because "the material points at something that is not a medium" is an
         *         authoring mistake and must not read as "no medium was set". Not const: it latches
         *         that warning, and this is asked every frame.
         */
        std::string MediumSourceOf( const Assets::AssetHandle& handle );

        /**
         * @brief The medium's own Properties, in declaration order — the schema its authored values are
         *        resolved against (Engine/Graphic/Clouds/CloudMediumValues.hpp).
         *
         * IT IS SERVED FROM HERE AND NOT FROM A Shader, because a medium HAS no Shader: it declares no
         * stages, so Register deliberately builds no program object for it. The Properties block is
         * therefore the only part of that file anything can ask about, and asking would otherwise mean
         * re-parsing a `.shader` once per frame.
         *
         * ORDER IS LOAD-BEARING and is the file's own: index i of the numeric properties is field i of the
         * medium's std430 block, and the i-th Texture2D is Core::kCloudMediumTextureFirst + i. Sorting or
         * de-duplicating here would silently rebind every slot after the first change.
         *
         * @return nullptr when @p handle is not a registered medium — the same refusal MediumSourceOf
         *         makes, and distinguishable from a medium that exposes nothing, which answers an EMPTY
         *         vector. The caller needs both: one means "the shipped medium", the other means "this
         *         medium, which has no parameters".
         */
        const std::vector<Core::Formats::ShaderParam>* MediumSchemaOf( const Assets::AssetHandle& handle ) const;

        /// Re-read a medium's body after its file changed on disk. Called by the hot-reload poll, which
        /// is the only thing that knows a shader asset was re-read.
        /// @return true when @p content IS a medium, i.e. when the poll has nothing else to do for it.
        bool RefreshMediumSource( const Assets::AssetHandle& handle, const std::string& content );

        /// Recompile every LIVE variant built from @p handle. The hot-reload poll reloads the program it
        /// registered; without this the variants of that same file would keep the code they were built
        /// with, which reads as "editing the shader stopped working once I authored a medium".
        /// @return how many variants were reloaded.
        int ReloadVariantsOf( const Assets::AssetHandle& handle );

        // All registered shader program names (for the editor's material shader picker).
        //
        // A NAME IN HERE DOES NOT PROMISE THAT GetByName RESOLVES IT. A Volume medium owns its name —
        // it must, or a later shader could claim the same one and the two would fight over it — but it
        // has no Shader object at all, because it is a program fragment. The picker's own loop already
        // skips a name that does not resolve (a shader may also fail to compile), which is what makes
        // this safe; a new caller has to do the same.
        std::vector<std::string> GetAllNames() const;

    private:
        std::unordered_map<Assets::AssetHandle, std::shared_ptr<Graphic::Shader>> m_Shaders;
        std::unordered_map<std::string, Assets::AssetHandle>                      m_NameToHandleMap;
        // Named passes of multi-pass shaders, keyed "<Shader>/<Pass>". Kept separate from
        // m_Shaders because several programs share one asset handle.
        std::unordered_map<std::string, std::shared_ptr<Graphic::Shader>> m_PassShaders;

        // The assets the registered programs were built from, so a variant can be compiled from the same
        // source later. WEAK because the asset manager owns them; an expired one is a named refusal.
        std::unordered_map<Assets::AssetHandle, std::weak_ptr<Assets::ShaderAsset>> m_ShaderAssets;

        struct VariantEntry
        {
            Assets::AssetHandle            Handle;
            std::weak_ptr<Graphic::Shader> Program;
        };
        // Keyed "<name>#<16 hex digits of the variant hash>". Weak, for the reason in AcquireVariant.
        std::unordered_map<std::string, VariantEntry> m_Variants;

        // EVERY REGISTERED Volume MEDIUM, held as TEXT AND SCHEMA rather than as the asset it came from.
        // A medium's source is needed continuously — the cloud renderer asks for it every frame — and the
        // asset eviction sweep unloads a shader asset nothing holds strongly, which emptied it three
        // seconds into a scene the first time this was written the other way. A few kilobytes per medium
        // is the whole cost, and it makes the answer independent of when the sweep last ran.
        //
        // THE SCHEMA IS KEPT BESIDE THE BODY AND NOT DERIVED FROM IT ON DEMAND, for the same reason and one
        // more: the two come out of ONE parse of one file, so they cannot describe different revisions of
        // it. Re-parsing for the schema every frame would also be the third instance of the crash the body
        // is cached to avoid.
        struct MediumEntry
        {
            std::string                             Source;
            std::vector<Core::Formats::ShaderParam> Properties;
        };
        std::unordered_map<Assets::AssetHandle, MediumEntry> m_Media;

        /// Handles already reported as naming something that is not a medium, so the refusal is said once
        /// per handle rather than once per frame.
        std::unordered_set<uint64_t> m_WarnedNotAMedium;
    };
} // namespace Desert::Runtime