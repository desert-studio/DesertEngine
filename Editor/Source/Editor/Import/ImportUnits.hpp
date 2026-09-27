#pragma once

#include <string_view>

namespace Desert::Editor::ImportUnits
{
    // WHAT UNIT AN IMPORTED FILE IS IN, AND WHETHER ANYONE ACTUALLY SAID SO.
    //
    // One world unit is one centimetre (Common::Units), so every importer owes the engine a single
    // number: how many centimetres one unit of the source file is worth. Three of the formats this
    // engine ships answer that question in three different ways, and the difference between them is
    // the whole reason this file exists rather than a constant at the call site.
    //
    // The defect it closes: assimp normalises everything to METRES — its FBX reader calls
    // SetFileScale( UnitScaleFactor * 0.01 ) (Editor/ThirdParty/assimp .../FBX/FBXImporter.cpp:180) —
    // so running aiProcess_GlobalScale with the default factor divided every centimetre-authored FBX
    // by 100. base.fbx, a 190 cm humanoid, imported 1.8983 units tall and nothing in the log said a
    // scale had been applied at all.

    // Where the number came from. The import log prints this, because a unit the file STATED and a
    // unit we ASSUMED must not read the same way afterwards.
    enum class Source
    {
        StatedByFile,       // the file carries its own unit (FBX GlobalSettings::UnitScaleFactor)
        FixedByFormat,      // the format defines one for every file (glTF 2.0 §3.5: metres)
        AssumedCentimetres, // neither the file nor the format says anything (OBJ) — assumed, and logged
        StatedButUnusable,  // the file stated a unit that cannot be a scale (zero, negative, not finite)
    };

    struct Scale
    {
        // Centimetres per file unit, which — because a world unit IS a centimetre — is the factor from
        // file space to world space. 1.0 for a centimetre file, 100.0 for a metre file, 2.54 for inches.
        float CentimetresPerUnit = 1.0f;

        Source From = Source::AssumedCentimetres;
    };

    // THE RULE, as a pure function of what the file said and what format it is. `extension` is the
    // source path's extension with its dot ( ".fbx" ), in any case. `fileStatedUnit` is whether the
    // file carried a unit at all, and `statedCentimetresPerUnit` is that unit when it did.
    //
    // A stated unit wins over the format's default: a format that fixes its unit (glTF) does not state
    // one per file, so the two never both apply, and if that ever changes the file is the better
    // authority. An unusable stated value does NOT silently become the default — it comes back as
    // StatedButUnusable so the caller can log the number it refused.
    Scale Resolve( std::string_view extension, bool fileStatedUnit, float statedCentimetresPerUnit );

    // THE FACTOR TO HAND assimp's aiProcess_GlobalScale so that the geometry lands in centimetres.
    //
    // assimp's ScaleProcess multiplies the scene by ( AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY *
    // AI_CONFIG_APP_SCALE_KEY ), and the second of those is the metres-per-file-unit assimp itself
    // worked out while reading (BaseImporter::UpdateImporterScale). We want that product to be our
    // centimetres-per-file-unit, so we supply the ratio. Letting assimp's own step do the work is
    // deliberate: it also scales bone offset matrices, node translations and animation position keys,
    // and a second implementation of that is a second thing to keep in agreement.
    //
    // `assimpMetresPerUnit` must be finite and greater than zero — assimp guarantees it (it refuses an
    // FBX whose UnitScaleFactor is zero, and the default is 1.0). `IsUsableScale` is how the caller
    // checks that before trusting this.
    float GlobalScaleFactor( float centimetresPerUnit, float assimpMetresPerUnit );

    // A scale has to be finite and strictly positive. A zero would collapse the mesh to a point and a
    // negative one would turn it inside out; both are worth a refusal with the number in it.
    bool IsUsableScale( float scale );

    // One short phrase for the import log, e.g. "stated by the file".
    const char* Describe( Source source );
} // namespace Desert::Editor::ImportUnits
