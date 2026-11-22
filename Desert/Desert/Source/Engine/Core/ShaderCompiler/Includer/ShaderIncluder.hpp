#pragma once

#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>

#include <Common/Core/Core.hpp> // Common::Filepath — this header leaned on the engine PCH for it,
                                // which broke every PCH-less consumer (the offline cook's tests)

#include <shaderc.hpp>

#include <cstddef>

namespace Desert::Core
{
    /**
     * @brief Resolves `#include` for shaderc, from disk in a dev build and from the mounted `.dpak` in a
     *        packaged game (the read goes through the VFS-aware FileSystem).
     *
     * ONE INSTANCE PER COMPILE, which is what makes the state below per-compile rather than global: the
     * compiler constructs an includer, hands it to shaderc, and drops it when the module is built.
     *
     * WHO OWNS THE STRINGS shaderc IS GIVEN, and why it is worth a paragraph. `shaderc_include_result`
     * carries RAW POINTERS plus lengths, and shaderc reads them long after GetInclude has returned — so
     * something has to keep the bytes alive until ReleaseInclude, and the only place to put it is the
     * result's own `user_data`. Until 2026-09-08 this file took `c_str()` from two heap strings and THEN
     * moved those strings into the pair it stored in `user_data`, which is correct only while the move
     * leaves the source's buffer where it was:
     *
     *   * a LONG string is heap-allocated, the move steals the pointer, and the captured `c_str()` happens
     *     to remain valid — every shipped shader path and every shader body is long, which is why nothing
     *     was ever seen;
     *   * a SHORT string lives inside the object (small-string optimisation), the move COPIES it and
     *     clears the source, and the captured pointer then reads an EMPTY buffer while `content_length`
     *     still says the original size. The compile succeeds with the include silently blank.
     *
     * Correctness rested on the LENGTH of the data rather than on ownership, and both heap strings leaked
     * on every include besides. Ownership is expressed by the type now: the bytes are placed in their
     * final home FIRST and the pointers are taken from there, and the one owner is released in
     * ReleaseInclude. Desert/Tests/Engine/ShaderIncluderOwnership drives an include whose content is a few
     * bytes — the case the old arrangement got wrong — and the live count below back to zero.
     */
    class ShaderIncluder final : public shaderc::CompileOptions::IncluderInterface
    {
    public:
        /**
         * @param basePath the file being compiled; the anchor for quoted includes.
         * @param variant  include targets whose bytes come from the CALLER rather than from disk — see
         *                 ShaderVariant.hpp. Asked before the file system and only for ANGLE includes,
         *                 whose path is written against the shader root and is therefore the same
         *                 string wherever it appears; a quoted include is relative to whoever wrote it
         *                 and names nothing a caller could address. A substituted name need not exist
         *                 on disk; when it does — the shipped default of Generated/CloudMedium.glslh
         *                 does — the substitution wins and the file is not read. Per-compile state,
         *                 because this object is per-compile.
         */
        explicit ShaderIncluder( const Common::Filepath& basePath, ShaderVariant variant = {} );
        ~ShaderIncluder() override;

        shaderc_include_result* GetInclude( const char* requested_source, shaderc_include_type type,
                                            const char* requesting_source, size_t include_depth ) override;

        void ReleaseInclude( shaderc_include_result* data ) override;

        /// HOW MANY RESULTS THIS INCLUDER HAS HANDED OUT AND NOT GOT BACK.
        ///
        /// It is the ownership claim made observable. Every result carries the only copy of its own bytes,
        /// so "released" and "freed" are the same event — and a count that does not return to zero is a
        /// leak of exactly that many include bodies. shaderc's contract is to release every result it is
        /// given, so the number is zero after a compile; the destructor says so out loud when it is not,
        /// because this object is the only one in a position to notice.
        [[nodiscard]] std::size_t LiveIncludeResults() const
        {
            return m_LiveResults;
        }

    private:
        shaderc_include_result* CreateErrorIncludeResult( const std::string& error );

        /// Allocates a result that owns @p name and @p content, taking the pointers AFTER the bytes are in
        /// their final home. The single place a `shaderc_include_result` is created, so the ownership rule
        /// has one statement rather than two that must agree.
        shaderc_include_result* MakeResult( std::string name, std::string content );

    private:
        Common::Filepath m_BasePath;
        ShaderVariant    m_Variant;
        std::size_t      m_LiveResults = 0;
    };

    /**
     * @brief WHERE A RESOURCE OF A PROGRAM IS DECLARED, for a refusal that knows only its name.
     *
     * SPIR-V carries no source line, so a reflection refusal ("set 0, binding 16 is claimed by two resources")
     * names resources, not lines — and the author has to find which of a dozen includes put the second one on
     * the slot. This walks the program text the way the compile did (comments skipped, `#include` followed
     * where it stands, with the includer's own resolution: the variant's substitution first, then a generated
     * protocol include, then the file) and answers the first line that DECLARES @p name: the identifier as a
     * whole word followed by `;`, `[`, `{` or the end of the line.
     *
     * @param source the program's DSL text (line numbers are the file's: the DSL translation is line-preserving).
     * @param path   the program's file; names it in the answer and anchors quoted includes.
     * @return "<file>:<line>", "<Include/Path.glslh>:<line> (generated ...)" for text that is not a file, or
     *         empty when no line declares the name (the caller says so instead of a location).
     */
    [[nodiscard]] std::string LocateShaderDeclaration( const std::string& source, const Common::Filepath& path,
                                                       std::string_view name, const ShaderVariant& variant = {} );
} // namespace Desert::Core
