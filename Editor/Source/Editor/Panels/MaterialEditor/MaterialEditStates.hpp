#pragma once

#include <Editor/Core/EditableProperty.hpp>
#include <Editor/Import/TextureSourceFormats.hpp>

#include <Engine/Assets/MaterialData.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Core/Formats/ShaderProgramMeta.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Editor::MaterialEdit
{
    // THE AUTHORED HALF OF A MATERIAL — the shader it draws with and the values it draws with. Everything
    // the Material Editor can change, and nothing else.
    //
    // The other half is IDENTITY: MaterialId, which is this asset's own name in the asset database, and
    // Parent, which is its place in the material-instance chain. Keeping the two halves apart is
    // load-bearing rather than tidy, because a document's WORKING COPY is a second material asset
    // registered beside the subject (see SurfaceMaterialAsset::CreateWorkingCopy for why it has to be a
    // second asset and not a spare struct). The copy therefore carries a MaterialId of its own, and a
    // whole-struct assignment in either direction would hand the subject's id to the copy on Discard, or
    // the copy's id to the subject on Apply. Both are the same failure: MaterialService keys the
    // mesh -> material link on exactly that id, so whichever of the two registered first would then answer
    // for both, and a mesh in the level would start drawing the preview's material.
    //
    // Parent is copied by NEITHER function, for the same reason and one more: it names somebody
    // else. An instance's working copy must resolve through the same parent chain the subject does, and
    // this window offers no way to re-parent a material, so a transfer of it could only ever be an
    // accident.

    // Do these two materials draw the same picture? Order-insensitive by name, because MaterialData stores
    // its parameters in a vector that grows in the order they were first written — two materials with the
    // same values authored in a different order are the same material, and comparing the vectors
    // positionally would report a document as permanently unapplied after a Discard.
    //
    // The shader is compared through SurfaceMaterialAsset::ShaderNameOf() rather than the optional, so an absent name and
    // an explicit "StaticMeshPBR" compare EQUAL. They are the same shader; a material that was saved
    // before the field existed must not read as differing from the one the editor just wrote.
    [[nodiscard]] inline bool AuthoredValuesEqual( const Assets::MaterialData& a, const Assets::MaterialData& b )
    {
        if ( Assets::SurfaceMaterialAsset::ShaderNameOf( a ) != Assets::SurfaceMaterialAsset::ShaderNameOf( b ) )
            return false;

        if ( a.Params.size() != b.Params.size() || a.Textures.size() != b.Textures.size() ||
             a.CloudAssets.size() != b.CloudAssets.size() || a.ShaderRefs.size() != b.ShaderRefs.size() )
            return false;

        // Sizes agree and MaterialData::SetParam/SetTexture never store a name twice, so "every entry of a
        // has an equal partner in b" is a full comparison rather than a one-way containment test.
        for ( const auto& param : a.Params )
        {
            const glm::vec4* other = b.FindParam( param.Name );
            if ( !other || *other != param.Value )
                return false;
        }

        // The getters answer 0 both for "not bound" and for "absent", which is right here: a slot explicitly
        // bound to nothing and a slot never written are the same material. Compared by the folded handle, so
        // a stale path locator beside the same GUID is not an authored difference.
        for ( const auto& texture : a.Textures )
            if ( b.GetTexture( texture.Name ) != a.GetTexture( texture.Name ) )
                return false;
        for ( const auto& cloud : a.CloudAssets )
            if ( b.GetCloudAsset( cloud.Name ) != a.GetCloudAsset( cloud.Name ) )
                return false;
        for ( const auto& shader : a.ShaderRefs )
            if ( b.GetShaderRef( shader.Name ) != a.GetShaderRef( shader.Name ) )
                return false;

        return true;
    }

    // Move the authored half of @p source into @p destination, leaving @p destination's identity alone.
    // The single transfer both Apply (working -> applied) and Discard (applied -> working) are written in,
    // so the two directions cannot drift into carrying different fields.
    inline void CopyAuthoredValues( Assets::MaterialData& destination, const Assets::MaterialData& source )
    {
        destination.ShaderName = source.ShaderName;
        destination.Params     = source.Params;
        destination.Textures    = source.Textures;
        destination.CloudAssets = source.CloudAssets;
        destination.ShaderRefs  = source.ShaderRefs;
    }

    // THE TWO "DIRTY"S, AND THEY ARE NOT ONE FLAG.
    //
    // `Unapplied` is about the SCENE: the artist has moved something that every mesh in every open scene is
    // still not showing. It is what decides whether Apply and Discard are offered.
    //
    // `Unsaved` is about the FILE: it is the document's dirty mark, the dot on the tab, the question on
    // close. It is measured against the WORKING state and not against the applied one, because an edit
    // that has not even been applied is still an edit the file does not have — a document that reported
    // itself clean between an edit and an Apply would lose that edit to a close with no question asked.
    //
    // Both are DERIVED from the three states rather than remembered as booleans set at the edit sites.
    // Flags of this kind are the shape of defect this engine keeps paying for: there is always one site
    // that forgets to set one (the Material Editor's own re-push flag was rewritten into a derived
    // identity for exactly this reason, see MaterialEditorPanel::PushedIdentity). Derived costs a walk of
    // a handful of named values once per frame and cannot be forgotten.
    struct DirtyState
    {
        bool Unapplied = false; // working != applied: the scene is not showing these edits
        bool Unsaved   = false; // working != on disk: the file does not have them
    };

    [[nodiscard]] inline DirtyState EvaluateDirty( const Assets::MaterialData& working,
                                                   const Assets::MaterialData& applied,
                                                   const Assets::MaterialData& onDisk )
    {
        DirtyState state;
        state.Unapplied = !AuthoredValuesEqual( working, applied );
        state.Unsaved   = !AuthoredValuesEqual( working, onDisk );
        return state;
    }

    /**
     * @brief DOES PUBLISHING THIS MATERIAL OWE THE SCENE ITS GLOBAL STAMP?
     *
     * MaterialService::BumpInvalidationVersion moves ONE counter that every mesh component in every open
     * scene compares itself against (Components.hpp SeenMaterialsVersion, MeshECSSystem.hpp:129). Each one
     * that differs throws away its cached RuntimeMaterialInstances and builds one MaterialInstance per slot
     * again on the next tick. That is the right price for an edit the scene must see, and the wrong price
     * for one it must not — and until this rule existed, EVERY FRAME of a drag paid it for values that by
     * construction stop at this window's preview. Measured on a 30-step drag: 30 stamps, now 0.
     *
     * IT BECAME A COST THE MOMENT STAGING LANDED, and not before. Until then a publish was only ever called
     * with the subject, so "the material was re-valued" and "the scene changed" were the same event. They
     * are two events now, and only one of them owes the stamp.
     *
     * WHO CACHES THE VALUES DECIDES IT, not which asset it is:
     *
     *   A BASE material's values live in the runtime Material the publish re-values in place. Anything
     *   rendering that asset directly — including the preview's own entity — reads through it and needs
     *   nothing. What caches is a CHILD INSTANCE, which bakes its overrides in at creation. A working copy
     *   is created with a freshly generated MaterialId that no file on disk names as a parent, so it HAS no
     *   children and the stamp would reach nobody who could care.
     *
     *   An INSTANCE has no runtime Material of its own; its overrides live in the cached MaterialInstance
     *   CreateRuntimeInstance built, and dropping that cache is the only lever there is. The preview's own
     *   entity holds one, so an instance's working copy still pays. Narrowing THAT needs a way to
     *   invalidate one scene's cached instances, which does not exist and which the Material Editor should
     *   not grow on its own — it is the renderer's vocabulary, not a document's.
     *
     * A RULE AND NOT AN `if` AT THE CALL SITE. MaterialEditorPanel.cpp is compiled by no suite at all
     * (scripts/CI/UnreachedSources.sh), so a decision written there is a decision nothing can show going
     * red — and this one is invisible when it is wrong: the pictures are identical either way, and only a
     * counter tells them apart.
     *
     * @param isInstance is the published material an instance of another?
     * @param isSubject  is it the document's SUBJECT — the asset the scenes render — rather than its
     *                   working copy?
     */
    [[nodiscard]] inline constexpr bool PublishOwesTheGlobalStamp( bool isInstance, bool isSubject ) noexcept
    {
        return isInstance || isSubject;
    }

    // ── THE PROPERTY CENSUS, DERIVED FROM THE SHADER'S SCHEMA ──────────────────────────────────────────
    //
    // WHAT THE CONTROL CHANNEL MAY WRITE IS NOT A LIST ANYBODY MAINTAINS. It is the shader's own
    // `Properties` block, read through ShaderProgramMeta — the SAME declaration MaterialEditorPanel::
    // DrawParameters walks to decide which rows exist and what widget each one gets.
    //
    // The alternative was a table of names in the channel's own code. It would have been a THIRD list of
    // parameter names beside the schema and the table, and this project closed two "a middle link drops a
    // property" defects on the day this was written. The third list does not exist here: there is one
    // walk of `schema.Params`, and everything below reads it.
    //
    // PURE, and that is load-bearing. MaterialEditorPanel.cpp cannot be reached by any test suite (it owns
    // a PreviewViewport, which owns a SceneRenderer, which needs a Vulkan device), so a census written
    // there would be a rule nothing could ever show going red. Here it is a function over two structs.

    /// How many components of a vec4 a schema type actually uses. ONE rule, asked by the census, by the
    /// refusal that counts a caller's numbers, and by the widget. A `float` given three numbers is a
    /// caller who meant a different parameter, and finding out from the picture is finding out too late.
    [[nodiscard]] inline int ComponentsOf( ::Desert::Core::Formats::ShaderValueType type )
    {
        using VT = ::Desert::Core::Formats::ShaderValueType;
        switch ( type )
        {
            case VT::Float2:
            case VT::Int2:
                return 2;
            case VT::Float3:
            case VT::Int3:
                return 3;
            case VT::Float4:
            case VT::Int4:
                return 4;
            default:
                // Float, Int, UInt, Bool and Unknown all live in x. Not a fallback standing in for a
                // missing case: a scalar IS one component, and Unknown is a schema the parser could not
                // type, which must not be guessed wider than the narrowest thing it could be.
                return 1;
        }
    }

    /// What a property is CALLED on the wire. The schema's type, refined by the widget where the widget
    /// changes what a caller must send: a Color is still four floats but a client that sees "color" knows
    /// they are not metres.
    [[nodiscard]] inline std::string TypeNameOf( const ::Desert::Core::Formats::ShaderParam& p )
    {
        using VT = ::Desert::Core::Formats::ShaderValueType;
        using W  = ::Desert::Core::Formats::ShaderParamWidget;

        if ( p.IsAssetRef() )
            return p.AssetKind;
        if ( p.IsTexture )
            return p.IsCubeTexture ? "textureCube" : "texture";
        if ( p.Widget == W::Color )
            return "color";

        switch ( p.Type )
        {
            case VT::Float:
                return "float";
            case VT::Float2:
                return "float2";
            case VT::Float3:
                return "float3";
            case VT::Float4:
                return "float4";
            case VT::Int:
                return "int";
            case VT::Int2:
                return "int2";
            case VT::Int3:
                return "int3";
            case VT::Int4:
                return "int4";
            case VT::UInt:
                return "uint";
            case VT::Bool:
                return "bool";
            default:
                return "unknown";
        }
    }

    /// WHAT A ROW FALLS BACK TO when this material says nothing about it — and the name of the thing this
    /// window calls "the default", which is NOT one thing.
    ///
    /// A BASE material inherits from its SHADER: `Properties … = 0.45` in the `.shader` file, which is a
    /// compile-time constant nobody can edit from this window. An INSTANCE inherits from its PARENT chain,
    /// which is another `.demat` somebody can open and change tomorrow. Details' reset (Д29) has only the
    /// first of those, because a component's default is `TypeInfo::GetDefaultInstance` and there is no
    /// second source — so the Details rule could not be copied here, it had to be generalised.
    ///
    /// One function, because EffectiveParamValue and the reset below must not each decide what "inherited"
    /// means: a reset that handed a row the schema default while the row was showing the parent's value
    /// would move the picture to a third number that nothing on screen ever displayed.
    [[nodiscard]] inline glm::vec4 InheritedParamValue( const Assets::MaterialData*                 parentData,
                                                        const ::Desert::Core::Formats::ShaderParam& p )
    {
        return parentData ? parentData->GetParam( p.Name, p.Default ) : p.Default;
    }

    /// The value a row SHOWS: this material's own override, else the parent's effective value (instance
    /// mode), else the schema default.
    ///
    /// ONE SEEDING RULE, called by the widget and by the census. Two copies of it would eventually differ
    /// by one fallback, and the symptom would be a client reading a number the window is not displaying —
    /// which is worse than no number, because a report quotes it beside a picture.
    [[nodiscard]] inline glm::vec4 EffectiveParamValue( const Assets::MaterialData&                 data,
                                                        const Assets::MaterialData*                 parentData,
                                                        const ::Desert::Core::Formats::ShaderParam& p )
    {
        return data.GetParam( p.Name, InheritedParamValue( parentData, p ) );
    }

    /// ── THE RESET, AND THE THREE THINGS IT IS NOT ONE OF ───────────────────────────────────────────────
    ///
    /// The Material Editor had no reset at all until M7, and the reason it could not simply borrow Details'
    /// (Д29) is that "default" names three different values here and only one of them is a type's
    /// default-constructed instance:
    ///
    ///   1. THE SHADER'S DEFAULT — `ShaderParam::Default`, parsed from `Properties … = 0.45`. What a BASE
    ///      material falls back to. Nothing in this window can change it; it changes when the `.shader`
    ///      does.
    ///   2. THE PARENT'S VALUE — what an INSTANCE falls back to, resolved through the chain by
    ///      MaterialService. A moving target: editing the parent moves every child that is not pinned.
    ///   3. "RESET OVERRIDES" — the toolbar button, instance-only, which is (2) applied to every row at
    ///      once. It existed before this one and is still the only way to clear the rows that carry a
    ///      value EQUAL to the parent's (see RowReset::None below for why those get no arrow).
    ///
    /// The row control is (1) or (2) depending on the document, never (3).
    enum class RowReset
    {
        None,            ///< the row is already showing what it inherits; there is nothing to hand back
        ToShaderDefault, ///< base material: back to the value the `.shader` declares
        ToParentValue,   ///< instance: drop this row's override, back to the parent chain
    };

    /// WHAT A RESET ON THIS ROW WOULD MEAN, and the ONE predicate the arrow is drawn from.
    ///
    /// THE RULE, in one sentence and readable in both directions: **a reset is offered exactly when the row
    /// is showing something other than what it inherits.** Details' Д29 rule ("the arrow appears only for a
    /// value different from the default") is the same sentence with the only fallback it has.
    ///
    /// WHY NOT "the material stores an entry for this row", which is the other obvious rule and the one the
    /// instance star already draws from. Because a `.demat` stores far more than it changes: measured on
    /// this repository's own `M_O4_Both_Clouds.demat`, 23 of its 28 stored parameters are byte-equal to the
    /// shader's default — they were written by the migration that moved the cloud look off the component,
    /// not by an artist. That rule would put 28 arrows on a material with 5 real deviations, and an arrow
    /// on every row is an arrow on none.
    ///
    /// WHAT THAT LEAVES UNCOVERED, said out loud rather than hidden: an INSTANCE may store an override that
    /// happens to equal its parent's value today. That is not a no-op — it is a PIN, and it will stop
    /// tracking the parent the moment somebody edits the parent. It gets the star (which asks "does this row
    /// track the parent?") and no arrow (which asks "is this row showing something else?"), because those
    /// are two different questions. "Reset Overrides" is what clears it, and its tooltip says so.
    ///
    /// TEXTURE AND ASSET-REFERENCE ROWS GET NO ARROW EITHER, for two different reasons, and neither is an
    /// oversight: a cloud type / layout slot already carries its own empty entry in its combo ("Default
    /// (cumulus congestus)", "None (procedural weather)"), so an arrow would be a second control for the
    /// one action; and a 2D texture slot cannot be UNBOUND at all today — Graphic::DataDrivenMaterial::
    /// SetTexture refuses a null image and MaterialFactory::ApplyShaderAsset skips handle 0, so erasing the
    /// entry would clear the file and leave the ball still sampling the old texture. A control that changes
    /// the document and not the picture is worse than no control (DC §1.3).
    [[nodiscard]] inline RowReset ResetOfferedFor( const Assets::MaterialData&                 data,
                                                   const Assets::MaterialData*                 parentData,
                                                   const ::Desert::Core::Formats::ShaderParam& p, bool isInstance )
    {
        if ( p.IsTexture || p.IsAssetRef() )
            return RowReset::None;

        if ( EffectiveParamValue( data, parentData, p ) == InheritedParamValue( parentData, p ) )
            return RowReset::None;

        return isInstance ? RowReset::ToParentValue : RowReset::ToShaderDefault;
    }

    /// What the arrow says when hovered. Two sentences and not one, because the artist has to know WHICH of
    /// the three the button is about before pressing it — the whole reason this enum exists.
    [[nodiscard]] inline const char* ResetTooltip( RowReset kind )
    {
        switch ( kind )
        {
            case RowReset::ToParentValue:
                return "Drop this override, back to the parent material's value.\nUse Reset Overrides in the "
                       "toolbar to drop them all.";
            case RowReset::ToShaderDefault:
                return "Reset to the value the shader declares for this parameter.\nThe material stops "
                       "carrying a value of its own here.";
            case RowReset::None:
            default:
                // Not reachable from a drawn arrow — the arrow is only drawn when a reset is offered. Named
                // rather than left to a fallthrough so that adding a kind fails to compile silently nowhere.
                return "";
        }
    }

    /// Why the channel cannot WRITE this property, or empty when it can.
    ///
    /// Asked by the census (to fill EditableProperty::NotSettableReason) and by `set` itself (to refuse),
    /// so the list a client reads and the answer it gets cannot disagree about one row. Splitting them was
    /// tried in every subsystem this project has and the two halves always drift.
    [[nodiscard]] inline std::string UnsettableReason( const ::Desert::Core::Formats::ShaderParam& p,
                                                       bool                                        isInstance )
    {
        // The window itself draws "from parent material" over both kinds of row in instance mode — the
        // reason is read off the same fact rather than invented here, so the two cannot disagree about
        // which rows an instance may write.
        const char* const inheritedByAnInstance =
             isInstance ? " An instance takes them whole from its parent in any case: per-instance "
                          "descriptors are a v2."
                        : "";

        if ( p.IsAssetRef() )
        {
            return "'" + p.Name + "' is a reference to a " + p.AssetKind +
                   ", not a value. It is bound by picking or dropping an asset, and an asset is not "
                   "something this request can carry." +
                   inheritedByAnInstance;
        }
        if ( p.IsTexture )
        {
            return "'" + p.Name +
                   "' is a texture slot. It is bound by dropping an asset on it, and an asset is not "
                   "something this request can carry." +
                   inheritedByAnInstance;
        }

        // Everything else is a uniform-buffer field, and an INSTANCE's value params are precisely what it
        // is allowed to override — so instance mode adds no refusal here.
        return {};
    }

    /// WHY A DROPPED FILE CANNOT GO IN A CLOUD TYPE (@p isType) OR CLOUD LAYOUT SLOT — one sentence,
    /// naming what arrived, what the slot takes, and, for the case this exists for, the clicks that DO get
    /// a picture into the sky.
    ///
    /// THE DROP USED TO DO NOTHING AND SAY NOTHING, which is what "I cannot add a texture to a cloud
    /// material" turned out to mean. FileExplorerPanel::EmitAssetDragSource types its payload by FileType
    /// and every image is FileType::Texture, so the browser emits TEXTURE_ASSET for a `.png` and falls back
    /// to AssetFile only for what it has no specific type for — which is exactly what `.dclayout` and
    /// `.decloudtype` are. An id that does not match fails SILENTLY in ImGui: nothing logs, nothing draws.
    /// CloudLayoutPanel's own image slots were fixed for this; the material's slots were not, which makes
    /// it the "one symptom fixed, its neighbour left standing" shape as well (DC §1.4).
    ///
    /// THE EXTENSIONS ARE PARAMETERS, not includes. This header is the pure half of the Material Editor —
    /// the half a suite can reach — and pulling `Engine/Assets/CloudLayout.hpp` in for two `const char*`
    /// would drag its tables along with it. The same trick RequestCloudDocumentOfType uses, for the same
    /// reason, and MaterialEditStates' suite pins that the panel passes the real constants.
    [[nodiscard]] inline std::string WhyThatCannotGoInThisSlot( const std::string& path, bool isType,
                                                                std::string_view typeExtension,
                                                                std::string_view layoutExtension )
    {
        const std::filesystem::path file      = path;
        const std::string           name      = file.filename().string();
        const std::string           extension = file.extension().string();

        const std::string wanted( isType ? typeExtension : layoutExtension );

        // A PICTURE IS THE CASE THIS FUNCTION EXISTS FOR, and the answer is a route rather than a "no".
        // Unreal's cloud material takes two LAYOUT TEXTURES and so does ours (O-4) — but a layout is not a
        // picture: it carries four species channels AND an add/remove mask, which one image cannot express,
        // which is exactly why CloudLayoutPanel imports the pattern and the mask as two separate pictures.
        // So the picture goes into a `.dclayout`, and the `.dclayout` comes here.
        //
        // "IS THIS AN IMAGE" IS ASKED OF THE ONE LIST, not of a sixth copy of it. The first version of this
        // function typed its own six extensions and TextureSourceFormatCensus failed the build for it by
        // name — which is the census doing exactly its job: two hand-written copies of this list had
        // already drifted once, and a JPEG outranked a TGA for a whole release because of it. Nothing here
        // CHOOSES between formats, so only the membership question is asked.
        {
            std::string lowered = extension;
            std::transform( lowered.begin(), lowered.end(), lowered.begin(),
                            []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );

            if ( TextureSourceFormatRank( lowered ) < kTextureSourceExtensionCount )
            {
                if ( isType )
                {
                    return "'" + name + "' is a picture, and this slot takes a cloud TYPE (" + wanted +
                           ") - the altitudes, silhouette and density of a kind of cloud, which no image "
                           "carries. Make one with Content Browser > right-click > New Cloud Asset > Cloud Type.";
                }
                return "'" + name + "' is a picture, and this slot takes a cloud LAYOUT (" + wanted +
                       "). A layout is not an image: it carries this layer's four species channels AND an "
                       "add/remove mask, which one picture cannot express. Make one with Content Browser > "
                       "right-click > New Cloud Asset > Cloud Layout, open it, and use 'Pattern image...' "
                       "to bring this picture in - then drop the .dclayout here.";
            }
        }

        // The near misses: a sibling cloud format arrives on the same generic payload, looks like it
        // belongs, and naming which of the two slots wants which is the whole of the answer.
        if ( extension == typeExtension || extension == layoutExtension )
        {
            return "'" + name + "' is a " + extension + " and this slot takes a " + wanted +
                   ( isType ? " (a kind of cloud)." : " (a painted map of the sky)." );
        }

        return "'" + name + "' is not something this slot can take. It takes a " + wanted + ".";
    }

    /// @p name in @p schema, or null with @p outRefusal saying why — never null and silent.
    ///
    /// A NAME THE SCHEMA DOES NOT DECLARE IS AN ERROR, not a value written and never read. Written, it
    /// would be serialised into the material, ignored by every shader, and the capture taken to prove the
    /// edit would render as "nothing moved" — indistinguishable from the feature being broken.
    [[nodiscard]] inline const ::Desert::Core::Formats::ShaderParam*
    FindSettableParam( const ::Desert::Core::Formats::ShaderProgramMeta& schema, const std::string& name,
                       bool isInstance, std::string& outRefusal )
    {
        const ::Desert::Core::Formats::ShaderParam* found = nullptr;
        for ( const auto& p : schema.Params )
        {
            if ( p.Name == name )
            {
                found = &p;
                break;
            }
        }

        if ( !found )
        {
            std::string known;
            for ( const auto& p : schema.Params )
            {
                if ( p.IsTexture || p.IsAssetRef() )
                    continue;
                if ( !known.empty() )
                    known += ", ";
                known += p.Name;
            }
            // The offer is built from the schema that was just walked, so a shader whose properties
            // changed cannot leave a stale suggestion behind.
            outRefusal = "'" + name + "' is not a property this document's shader declares. It offers: " +
                         ( known.empty() ? std::string( "nothing that carries a value." ) : known );
            return nullptr;
        }

        outRefusal = UnsettableReason( *found, isInstance );
        return outRefusal.empty() ? found : nullptr;
    }

    // ── THE PARAMETER TABLE'S GROUPS ───────────────────────────────────────────────────────────────────

    /// The name the hoisted group is drawn under. ONE definition, because the window prints it and the
    /// census reports it as `group`, and a second spelling would make a client's filter miss half the rows.
    inline constexpr const char* kInputsGroupName = "Inputs";

    /// WHAT KIND OF GROUP THIS IS, because three of them exist and only one belongs to the shader author.
    /// Told apart by a field rather than by inspecting the name, so a category somebody literally calls
    /// "Inputs" cannot be mistaken for the hoisted one.
    enum class ParameterGroupKind : uint8_t
    {
        /// The material's own ASSET REFERENCES, hoisted to the front. Not one of the author's stages.
        Inputs,

        /// A `Category` the shader author wrote. Numbered, in the order the file declares them.
        Authored,

        /// Params that declare no Category at all. Drawn last, under a heading that says so.
        Uncategorised,
    };

    /// WHEN A GROUP'S EDITS REACH THE PICTURE, as one value for the whole heading.
    ///
    /// DERIVED FROM THE MEMBERS AND NEVER FROM THE GROUP'S NAME. "Weather is a bake stage" is true of the
    /// cloud material and is a fact about its parameters, not about the word — a category called Weather in
    /// some other shader owes this window nothing. @ref Mixed is a real answer and not a failure: it means
    /// the heading cannot speak for its rows and each row has to, which is what the panel then draws.
    enum class GroupTiming : uint8_t
    {
        Unstated,  ///< no member of this group declares a timing — the window claims nothing
        Immediate, ///< every member that declares one says Immediate
        Rebake,    ///< every member that declares one says Rebake
        Mixed,     ///< members disagree; the heading defers to the rows
    };

    /// ONE GROUP of the parameter table: a `Category` the shader author wrote, and the params carrying it.
    struct ParameterGroup
    {
        /// Which of the three this is. Read this rather than testing @ref Category against a string.
        ParameterGroupKind Kind = ParameterGroupKind::Authored;

        /// The author's category text, verbatim; "Inputs" for the hoisted group. EMPTY means these params
        /// declare no Category at all — a group of its own rather than a silent default; see
        /// PlanParameterGroups.
        std::string Category;

        /// Indices into ShaderProgramMeta::Params, in declaration order within the group. INDICES AND NOT
        /// POINTERS: a plan that outlives the schema it describes is a defect a pointer hides and a
        /// subscript catches, and the plan is rebuilt every frame from a schema resolved seconds earlier.
        std::vector<std::size_t> Params;

        /// Where this group sits in the author's own work order, 0-based — the number the window prints
        /// ("00 · Cloud Types"). ABSENT for Inputs and for the uncategorised group, neither of which is a
        /// stage of anyone's order and neither of which may take a place in it.
        std::optional<std::size_t> Ordinal;

        /// What this group's edits cost to see — see GroupTiming. Folded from the members here rather than
        /// at the drawing site, so the window and any census of it read one answer.
        GroupTiming Timing = GroupTiming::Unstated;
    };

    /// The sentence the window puts beside a heading, and the channel puts on a row. ONE WORDING, because
    /// the panel and the property census both say it and two spellings of one guarantee is how a promise
    /// starts drifting from what the code does. Empty for Unstated: a window that says nothing is honest,
    /// and a window that says "immediate" about a parameter nobody classified is not.
    [[nodiscard]] constexpr const char* GroupTimingPhrase( GroupTiming timing ) noexcept
    {
        switch ( timing )
        {
            case GroupTiming::Unstated:
                return "";
            case GroupTiming::Immediate:
                return "on screen next frame";
            case GroupTiming::Rebake:
                return "rebuilds the volume - seconds";
            case GroupTiming::Mixed:
                return "mixed - see each row";
        }
        return "";
    }

    /// The same sentence for ONE parameter. Longer than the heading's, because a row's tooltip is where a
    /// person goes when the heading was not enough.
    [[nodiscard]] constexpr const char*
    ParamTimingPhrase( ::Desert::Core::Formats::ShaderParamTiming timing ) noexcept
    {
        switch ( timing )
        {
            case ::Desert::Core::Formats::ShaderParamTiming::Unspecified:
                return "";
            case ::Desert::Core::Formats::ShaderParamTiming::Immediate:
                return "This one is read while the frame is drawn: the picture moves with the slider.";
            case ::Desert::Core::Formats::ShaderParamTiming::Rebake:
                return "This one is an input to a precomputation on the CPU. Moving it starts that work "
                       "again and the sky keeps showing the PREVIOUS result until it lands - seconds, not "
                       "a frame. Nothing is broken while you wait.";
        }
        return "";
    }

    /// Fold the members' timings into the group's — the definition GroupTiming's own note gives.
    [[nodiscard]] inline GroupTiming FoldGroupTiming( const ::Desert::Core::Formats::ShaderProgramMeta& schema,
                                                      const ParameterGroup&                             group )
    {
        using PT = ::Desert::Core::Formats::ShaderParamTiming;

        GroupTiming folded = GroupTiming::Unstated;
        for ( const std::size_t index : group.Params )
        {
            const PT timing = schema.Params[index].Timing;
            if ( timing == PT::Unspecified )
                continue; // a param that makes no claim cannot make the group's claim false either

            const GroupTiming asGroup = timing == PT::Rebake ? GroupTiming::Rebake : GroupTiming::Immediate;
            if ( folded == GroupTiming::Unstated )
                folded = asGroup;
            else if ( folded != asGroup )
                return GroupTiming::Mixed;
        }
        return folded;
    }

    /// THE PARAMETER TABLE'S GROUPS, in the order the shader file declares them.
    ///
    /// `ShaderParam::Category` has been filled in by shader authors since the DSL gained the attribute and
    /// read by NOBODY. Fifty-two params across the shipped shaders carry one of nine values — Surface,
    /// Textures, Glass, Weather, Placement, Layout, Cloud Types, Detail, Lighting — and the Material Editor
    /// drew every one of them in a single flat table, so the cloud material's thirty-four arrived as one
    /// undivided list. Desert/Tests/Engine/CloudMaterialSchema has meanwhile been asserting that every cloud
    /// param carries a category "so it does not land in an unnamed group": an assertion about a value that
    /// had no consumer. This function is the consumer, and that assertion is now about something.
    ///
    /// THE ORDER IS THE FILE'S, AND IT IS DERIVED RATHER THAN LISTED. A group appears where its first member
    /// is declared. The alternative — a table of category names in a preferred order — would be a SECOND
    /// census beside the shader, and when it fell behind, a new category would silently sink to the bottom
    /// or vanish. Nothing here can fall behind, because there is no second list. The file order is already a
    /// work order: CloudRaymarch declares Cloud Types, Weather, Placement, Layout, Detail, Lighting, which
    /// is the order a sky is built in, and Epic number their own cloud material's groups the same way
    /// ("00 - Cloud Layout", "01 - Cloud Shape", "02 - Storm").
    ///
    /// PARAMS OF ONE CATEGORY NEED NOT BE CONTIGUOUS. They are in every shipped shader today and nothing
    /// keeps them that way. A plan built by watching for the category to CHANGE would open one group twice
    /// and number the second copy as though it were new — so this walks the list once and appends to
    /// whichever group already exists.
    ///
    /// AN UNCATEGORISED PARAM GETS A GROUP THAT SAYS SO. It is not folded into the previous named group and
    /// not left in an unnamed block that reads as a rendering accident. Six shipped shaders have such params
    /// (Terrain declares four: DetailTiling, u_GrassTex, u_RockTex, u_SnowTex; MatProbe, MatProbeUnlit,
    /// Skybox, TextSDF and Unlit one each), and the caller must tell "nothing here is categorised" — draw
    /// the flat table exactly as before — from "some of it is", where the leftovers get their own heading.
    /// Those are different facts about the shader and must not produce the same picture.
    [[nodiscard]] inline std::vector<ParameterGroup>
    PlanParameterGroups( const ::Desert::Core::Formats::ShaderProgramMeta& schema )
    {
        std::vector<ParameterGroup> groups;

        // THE MATERIAL'S OWN INPUTS, HOISTED TO THE FRONT — the other half of the shape the owner picked.
        //
        // An ASSET REFERENCE is not a value an artist dials; it is a dependency on another document, and
        // "which clouds is this sky made of" is the question this whole task exists to answer. Gathering
        // them at the top is what variant A drew and what the owner chose: the four cloud type slots read
        // as a roster there, instead of as four combo boxes among thirty scalars.
        //
        // MEMBERSHIP IS DERIVED, exactly as the order is: a param belongs here IF IT IS AN ASSET REFERENCE
        // (ShaderParam::IsAssetRef, which is the schema's own AssetKind field). A hand-written list of
        // parameter names would be the second census the rest of this file spends its comments refusing,
        // and a fifth cloud type slot would silently fail to appear in it.
        //
        // TEXTURES ARE NOT HOISTED, AND THAT IS THE LINE. A texture is bound to a slot of THIS material and
        // authored nowhere else — it is a value. An asset reference is a link to a document with a window of
        // its own. StaticMeshPBR's author already grouped its textures under "Textures"; hoisting them would
        // be this code re-deciding a grouping that was made correctly, which is the thing it must not do.
        for ( std::size_t index = 0; index < schema.Params.size(); ++index )
        {
            if ( !schema.Params[index].IsAssetRef() )
                continue;

            if ( groups.empty() )
            {
                ParameterGroup inputs;
                inputs.Kind     = ParameterGroupKind::Inputs;
                inputs.Category = kInputsGroupName;
                groups.push_back( std::move( inputs ) );
            }
            groups.front().Params.push_back( index );
        }

        for ( std::size_t index = 0; index < schema.Params.size(); ++index )
        {
            // Already hoisted. Drawing it in both places would be two controls over one value, which is the
            // duplicated-state defect this codebase removes rather than adds.
            if ( schema.Params[index].IsAssetRef() )
                continue;

            const std::string& category = schema.Params[index].Category;

            auto group =
                 std::find_if( groups.begin(), groups.end(), [&category]( const ParameterGroup& g )
                               { return g.Kind != ParameterGroupKind::Inputs && g.Category == category; } );
            if ( group == groups.end() )
            {
                ParameterGroup fresh;
                fresh.Kind = category.empty() ? ParameterGroupKind::Uncategorised : ParameterGroupKind::Authored;
                fresh.Category = category;
                groups.push_back( std::move( fresh ) );
                group = std::prev( groups.end() );
            }
            group->Params.push_back( index );
        }

        // A GROUP THE HOIST EMPTIED IS NOT DRAWN. CloudRaymarch's "Cloud Types" category is nothing but its
        // four type slots, so after the hoist it has no rows left. A heading with nothing under it reads as
        // a section that failed to load, not as one whose contents moved somewhere better.
        groups.erase( std::remove_if( groups.begin(), groups.end(),
                                      []( const ParameterGroup& g ) { return g.Params.empty(); } ),
                      groups.end() );

        // The uncategorised group goes LAST, wherever its first member happened to be declared. Two reasons,
        // and the second is the one that matters: it is not a stage of the author's order, so it must not
        // take a number in the middle of it; and moving it to the end means a shader that categorised
        // NOTHING produces exactly one group, which is what lets the window reproduce the old flat table
        // without a special case in the drawing code.
        const auto unnamed = std::find_if( groups.begin(), groups.end(), []( const ParameterGroup& g )
                                           { return g.Kind == ParameterGroupKind::Uncategorised; } );
        if ( unnamed != groups.end() && std::next( unnamed ) != groups.end() )
            std::rotate( unnamed, std::next( unnamed ), groups.end() );

        std::size_t ordinal = 0;
        for ( ParameterGroup& group : groups )
        {
            if ( group.Kind == ParameterGroupKind::Authored )
                group.Ordinal = ordinal++;
            group.Timing = FoldGroupTiming( schema, group );
        }

        return groups;
    }

    /// Does this plan divide the table at all? FALSE for a shader with no asset references that categorised
    /// nothing — one group, with no name — which the window draws as the single flat table it drew before
    /// groups existed. The distinction is the whole point: an undivided table is what "this shader has no
    /// categories" looks like, and a table under one heading called something would be inventing a fact.
    [[nodiscard]] inline bool HasNamedGroups( const std::vector<ParameterGroup>& groups )
    {
        return std::any_of( groups.begin(), groups.end(), []( const ParameterGroup& group )
                            { return group.Kind != ParameterGroupKind::Uncategorised; } );
    }

    /// EVERY property the document offers, in the order the WINDOW DRAWS THEM — which since the parameter
    /// table gained groups is the plan's order, not the schema's declaration order.
    ///
    /// THE TWO ORDERS HAVE TO BE ONE, and this is the line that makes them one. The census and the panel
    /// walking the same rows in the same sequence is the relation a client depends on when it reads
    /// `properties`, counts to the ninth row and then talks about "the ninth row" to a person looking at the
    /// window. Both now consume PlanParameterGroups, so there is one order and not two that agree by
    /// inspection — Desert/Tests/Editor/MaterialEditStates asserts the agreement rather than trusting it.
    /// (They differ only for a shader that interleaves categories; none does today, which is exactly the
    /// condition under which a relation quietly stops holding and nobody notices.)
    ///
    /// @p parentData is the parent material's data in instance mode, null for a base material.
    [[nodiscard]] inline std::vector<EditableProperty>
    DescribeProperties( const ::Desert::Core::Formats::ShaderProgramMeta& schema, const Assets::MaterialData& data,
                        const Assets::MaterialData* parentData, bool isInstance )
    {
        std::vector<EditableProperty> properties;
        properties.reserve( schema.Params.size() );

        const std::vector<ParameterGroup> groups = PlanParameterGroups( schema );

        for ( const ParameterGroup& group : groups )
        {
            for ( const std::size_t index : group.Params )
            {
                const auto& p = schema.Params[index];

                EditableProperty entry;
                entry.Group      = group.Category;
                entry.Name       = p.Name;
                entry.Label      = p.DisplayName.empty() ? p.Name : p.DisplayName;
                entry.Type       = TypeNameOf( p );
                entry.Components = ComponentsOf( p.Type );
                entry.Min        = p.Min;
                entry.Max        = p.Max;

                // A row with its own entry in the child IS an override — the same test the window's star
                // draws from, so the census and the panel mark the same rows.
                entry.OverridesParent = isInstance && !p.IsTexture && data.FindParam( p.Name ) != nullptr;

                entry.NotSettableReason = UnsettableReason( p, isInstance );
                entry.Settable          = entry.NotSettableReason.empty();

                // WHEN A WRITE HERE BECOMES VISIBLE. Taken from the schema's own attribute, so the channel
                // and the window answer the same question from the same place; empty when the shader makes
                // no claim, which is a fact about the shader rather than a gap here.
                if ( p.Timing != ::Desert::Core::Formats::ShaderParamTiming::Unspecified )
                    entry.Timing = ::Desert::Core::Formats::ShaderParamTimingName( p.Timing );

                if ( !p.IsTexture && !p.IsAssetRef() )
                {
                    const glm::vec4 value = EffectiveParamValue( data, parentData, p );
                    for ( int i = 0; i < entry.Components; ++i )
                        entry.Value[static_cast<std::size_t>( i )] = value[i];
                }
                else
                {
                    // A texture's value is an asset handle, and the array carries floats. Reported as zero
                    // components rather than as a float that happens to hold a 64-bit id badly: a number a
                    // client cannot use is worse than an absence it can see.
                    entry.Components = 0;
                }

                properties.push_back( std::move( entry ) );
            }
        }

        return properties;
    }
} // namespace Desert::Editor::MaterialEdit
