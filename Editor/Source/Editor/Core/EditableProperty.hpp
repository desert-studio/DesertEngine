#pragma once

#include <array>
#include <optional>
#include <string>

namespace Desert::Editor
{
    /**
     * @brief ONE PROPERTY OF THE FOCUSED DOCUMENT, as a value. The vocabulary of the control channel's
     *        SECOND REQUEST CATEGORY.
     *
     * WHY THERE IS A SECOND CATEGORY AT ALL, and why it is a category rather than a second execution path.
     *
     * The channel executes the COMMAND PALETTE (Editor/Core/Control/ControlDispatch.hpp): a dictionary of
     * ACTIONS, each with a name a person could say out loud. Dragging a slider is not one of those. It has
     * no name, and inventing one would mean a palette entry per value — "Set Albedo to 0.3", a hundred and
     * seventy of them for one material — which is not a dictionary of actions, it is a serialised argument
     * list wearing one.
     *
     * So a property edit is a DIFFERENT KIND OF REQUEST, and the rule it must not break is the one about
     * EXECUTION: the value has to land in the same setter the widget calls. It does — MaterialEditorPanel::
     * WriteParam is the single write, reached by the slider and by `set` alike. One path, two ways to call
     * it, which is the point of the channel rather than an exception to it. What stays forbidden is a
     * second dispatch mechanism for palette COMMANDS; nothing here is one.
     *
     * THE CENSUS IS DERIVED, NEVER WRITTEN OUT. A document builds this list from the same declaration its
     * own editor draws from — for a material, the shader's `Properties` block, through
     * MaterialEdit::DescribeProperties. A hand-written list of names would be a THIRD list of parameter
     * names beside the schema and the widget table, and a third list is the defect shape this codebase
     * spends its days removing: two of them agree, the one nobody remembers falls behind, and what the
     * channel offers stops being what the window edits.
     *
     * NO ImGui AND NO glm HERE ON PURPOSE. This type crosses ISubjectDocument, the control channel's JSON
     * writer and a test suite that links only Common; a header that dragged a UI toolkit behind it could
     * not be asserted anywhere the editor is not built.
     */
    struct EditableProperty
    {
        /// What `set` addresses it by — the schema's own name, not the label. Two properties may display
        /// the same words; the name is what the material actually stores.
        std::string Name;

        /// What a person reads beside the control. Equal to Name when the schema declares no display name.
        std::string Label;

        /// WHICH GROUP OF THE WINDOW THIS ROW IS UNDER — the shader author's own `Category`, verbatim.
        /// EMPTY means the schema declares none for this param, which the window shows under a heading that
        /// says exactly that; empty is therefore a fact about the shader and not a value this census failed
        /// to fill in.
        ///
        /// Reported because the census is what a client checks the window against, and after the parameter
        /// table gained groups a row's position on screen is (group, then order within it). A census that
        /// named only the row would describe a flat list the window no longer draws — the client would count
        /// to the ninth entry, the person would count to the ninth row, and for any shader with groups those
        /// are different rows.
        std::string Group;

        /// The storage type, in the words the schema uses: "float", "float2", "float3", "float4", "int",
        /// "bool", "color", "texture", "textureCube", or the asset kind for a non-texture reference. Named
        /// on the wire so a client that sends three numbers to a float gets a refusal it can understand
        /// before it looks at the picture and concludes the editor is broken.
        std::string Type;

        /// How many of Value are meaningful. 1..4.
        int Components = 1;

        /// The clamp the widget applies, when the schema declares one. A `set` outside it is refused
        /// rather than clamped: a value silently moved is a value the caller will read back as its own.
        std::optional<float> Min;
        std::optional<float> Max;

        /// What the document is showing RIGHT NOW for this property — the same number the widget draws,
        /// seeded by the same rule (own override, else the parent's effective value, else the schema
        /// default). Components beyond @ref Components are zero. DOUBLE, as UE's FVector is: a world position
        /// (the selection's Location, the viewport camera) at 20 km is a double the census must not round;
        /// a float property widens into it exactly.
        std::array<double, 4> Value{};

        /// An INSTANCE row whose value comes from the parent rather than from this material's own
        /// overrides. Reported because "0.5" means something different depending on which of the two it
        /// is, and a client reading the census cannot otherwise tell an override from an inheritance.
        bool OverridesParent = false;

        /// FALSE is not "this row is broken" — several rows are legitimately unwritable through a channel
        /// that carries numbers (a texture slot takes an asset, an instance takes its textures whole from
        /// its parent). It is reported rather than omitted, because a property missing from the census
        /// reads as a property the shader does not declare, and those are two different problems.
        bool Settable = true;

        /// Why not. NEVER empty while Settable is false — a refusal with nothing said is the shape this
        /// whole channel is built to make impossible.
        std::string NotSettableReason;

        /// WHEN A WRITE TO THIS PROPERTY REACHES THE PICTURE: "Immediate", "Rebake", or empty when the
        /// schema makes no claim. Reported because the channel's whole contract is that a reply is released
        /// only after a frame that shows the command's effect — and for a `Rebake` property that frame is
        /// SECONDS away, because a CPU precomputation has to be re-run first (the cloud volume: 3.3 to 14.1
        /// seconds, task O8). A client that does not know which kind it just wrote reads the unchanged
        /// frame as a failed write, which is exactly the conclusion the owner reached by hand.
        std::string Timing;
    };
} // namespace Desert::Editor
