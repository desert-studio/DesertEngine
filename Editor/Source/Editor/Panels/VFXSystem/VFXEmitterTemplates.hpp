#pragma once

#include <Engine/Assets/Serialization/VFXSystem.hpp>

#include <Common/Core/ResultStr.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace Desert::Editor
{
    /**
     * @brief One EMITTER TEMPLATE the VFX System editor adds from (UE: a Niagara emitter asset flagged as a
     *        template, offered by "Add Emitter").
     *
     * Templates are DATA, not code: every `.dfx` in VFXEmitterTemplateDir() is an ordinary VFX system file whose
     * emitters are the templates (Fire, Smoke, Sparks, Magic, Explosion ship with the engine — the particle
     * presets of the removed particle editor, in centimetres). The directory carries its own VFXCategories.json,
     * so a template is read with the same ParseVFXSystem as any project system and needs no project category.
     */
    struct VFXEmitterTemplate
    {
        std::string                           Name; ///< the file stem, or "<stem> / <emitter>" when it has several
        Assets::Serialization::VFXEmitterData Emitter;
        Assets::Serialization::VFXBounds      Bounds; ///< the template system's bounds, centimetres
    };

    /// `<engine resources>/VFX/EmitterTemplates`.
    [[nodiscard]] std::filesystem::path VFXEmitterTemplateDir();

    /// Every template in @p dir, by name. A file that does not parse is an error naming it.
    Common::ResultStr<std::vector<VFXEmitterTemplate>> ReadVFXEmitterTemplates( const std::filesystem::path& dir );
} // namespace Desert::Editor
