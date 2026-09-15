#pragma once

#include <Engine/Graphic/Render2D/ClipRegion2D.hpp>
#include <Engine/Graphic/Render2D/Transform2D.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

// CPU-side retained draw list for the 2D batcher — the analogue of ImGui's ImDrawList, but engine-owned.
// The UI tree (Canvas / UILayout) records primitives here as plain geometry; the GPU backend (Render2D)
// then uploads Vertices/Indices once and issues one indexed draw per DrawCommand. Kept free of any GPU /
// Vulkan / ECS dependency so it is pure and unit-testable (like Desert::UI::UILayout).
namespace Desert::Graphic::Render2D
{
    struct Vertex2D
    {
        glm::vec2 Position; // pixel coordinates (top-left origin)
        glm::vec2 UV;
        glm::vec4 Color; // straight (non-premultiplied) RGBA, 0..1

        /// Same vertex? DEFAULTED, so the compiler compares the eight floats and not the 32 bytes. The one
        /// reader — UIIntrospection, finding the first vertex two draw lists disagree about — used memcmp,
        /// which answers a different question of any float: -0.0f and 0.0f are one position and two bit
        /// patterns, so a vertex mirrored to exactly zero read as "changed" and pointed the introspector at
        /// the wrong element.
        [[nodiscard]] bool operator==( const Vertex2D& ) const = default;
    };

    // A run of indices sharing the same GPU state (bound texture + clip rect). Consecutive primitives with
    // an identical state extend the current command instead of opening a new one, so flat-colour UI collapses
    // to a single draw. A null Texture means "solid" — the backend binds its 1x1 white texture.
    struct DrawCommand
    {
        const void* Texture     = nullptr;                    // opaque texture id (engine Image2D*), null => white
        glm::vec4   ClipRect    = { 0.0f, 0.0f, 0.0f, 0.0f }; // x,y,w,h px; W<=0 => unclipped
        uint32_t    IndexOffset = 0;                          // first index into GetIndices()
        uint32_t    IndexCount  = 0;                          // number of indices in this batch
        bool        Text        = false;                      // true => SDF glyph atlas (text pipeline), else UI2D

        // MATERIAL (Ю11): an opaque id for the UI-domain material this batch's fill is drawn with
        // (Graphic::UIMaterial*), null for every batch that is not one. The backend resolves it to a
        // pipeline of that material's own shader plus the material's row of `Materials[]`.
        //
        // IT IS A SECOND RESOURCE AND NOT A WIDENING OF `Texture`, deliberately. Slate puts its material
        // in the texture slot because an FSlateMaterialResource IS an FSlateShaderResource, and that is
        // the right call for Slate — but here `Texture` is read by UIIntrospection as "distinct
        // descriptor sets bound this frame" and by the UI Debugger as a texture column, and a material
        // arriving under that name would make both of them quietly wrong. Two pointers cost one extra
        // compare in the merge test below and keep every reader honest.
        //
        // The batch consequence is the same either way, and it is the good one: a material breaks a run
        // exactly like a different texture does, and an element with NO material adds nothing to the key
        // — measured on UI_ElementProbe, where 5 of 6 breaks are text/solid alternation and 0 are
        // resources.
        const void* Material = nullptr;

        // GLASS (backdrop blur): this batch samples the blurred scene snapshot instead of a texture, and
        // masks itself with a rounded rectangle. Every glass rect carries its own rect/radius/blur in push
        // constants, so glass commands are never merged with anything — one element, one draw.
        bool      Glass      = false;
        glm::vec4 GlassRect  = { 0.0f, 0.0f, 0.0f, 0.0f }; // min.xy, max.xy in the rect's OWN space, px
        float     GlassRound = 0.0f;                       // corner radius, px
        float     GlassLod   = 0.0f;                       // blur level in the backdrop pyramid

        // Screen px -> the rect's own space, so the mask above can be evaluated where the rect is
        // axis-aligned however the element is turned. Identity for an untransformed panel, and identity
        // maps gl_FragCoord onto itself EXACTLY, which is what keeps the untransformed picture unchanged.
        glm::mat3 GlassInverse = glm::mat3( 1.0f );
        // One screen pixel measured in that own space — the antialiasing feather, so a scaled-up panel
        // does not get a scaled-up soft edge. Exactly 1 when untransformed.
        float GlassFeather = 1.0f;
    };

    class DrawList2D
    {
    public:
        // Clear geometry for a new frame while keeping the allocated capacity (no per-frame reallocation).
        void Reset();

        // Filled axis-aligned rectangle. `min`/`max` are top-left / bottom-right pixel corners. `rounding` >0
        // rounds the corners (radius px, clamped to half the shorter side) via a triangle fan.
        void AddRectFilled( const glm::vec2& min, const glm::vec2& max, const glm::vec4& color,
                            float rounding = 0.0f );

        // Frosted-glass rectangle: fills with the BLURRED scene behind it, tinted by @p tint (its alpha is
        // how much of the tint covers the blur — 0 = pure blur, 1 = flat colour). @p blur01 picks how strong
        // the blur is (0..1, mapped to the backdrop pyramid's LODs by the backend). Rounded by @p rounding,
        // antialiased in the shader rather than tessellated. Falls back to a plain rounded rect when the
        // backend has no backdrop image (e.g. the very first frame).
        void AddGlassRect( const glm::vec2& min, const glm::vec2& max, const glm::vec4& tint,
                           float rounding = 0.0f, float blur01 = 1.0f );

        // Vertical two-colour gradient fill (top -> bottom). Solid batch (white texture).
        void AddRectFilledMultiColor( const glm::vec2& min, const glm::vec2& max, const glm::vec4& topColor,
                                      const glm::vec4& bottomColor );

        // Rectangle outline of the given pixel `thickness`, drawn as four filled bars (sharp corners).
        void AddRect( const glm::vec2& min, const glm::vec2& max, const glm::vec4& color, float thickness );

        // Filled triangle (e.g. a dropdown arrow). Solid batch (white texture).
        void AddTriangleFilled( const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& p2,
                                const glm::vec4& color );

        // Straight line segment of pixel `thickness`, drawn as a quad (butt caps). Solid batch. Used by the
        // built-in vector icon set (checks, chevrons, strokes).
        void AddLine( const glm::vec2& a, const glm::vec2& b, const glm::vec4& color, float thickness );

        // Annulus (ring) centred at `center`, from `innerRadius` to `outerRadius` (px), as a triangle strip.
        // The colour sweeps `colorA` -> `colorB` -> `colorA` around the ring (smooth, seamless), giving a
        // conic-style gradient border for circular avatars / status rings / progress rings. Solid batch.
        void AddRing( const glm::vec2& center, float outerRadius, float innerRadius, const glm::vec4& colorA,
                      const glm::vec4& colorB, int segments = 48 );

        // Clip subsequently-added primitives to `min`..`max` (px), intersected with the current clip (so
        // nested masks compose). Pair with PopClipRect.
        //
        // `min`/`max` are read in the CURRENT transform's space. The AXIS-ALIGNED part of the result is the
        // scissor the backend sets per batch; the OBLIQUE part — the four edges of a rotated clipper, which
        // a scissor cannot express — cuts the geometry here, before it is a vertex. So a rotated clipper
        // clips to the quadrilateral itself and not to the box around it, at zero extra draw calls: the
        // batch key still breaks only on texture, text mode and that scissor box.
        //
        // Ю8 stored only the box and recorded the looseness as a decision; this is that decision closed.
        // The walk's pointer clip goes through the same IntersectClipRegion, so the picture and the pointer
        // are cut by one object rather than by two that agree.
        void PushClipRect( const glm::vec2& min, const glm::vec2& max );
        void PopClipRect();

        // The accumulated clip, for the caller that must refuse a pointer exactly where the geometry was
        // refused. Read rather than rebuilt, for the same reason GetTransform() is.
        const ClipRegion2D& GetClipRegion() const
        {
            return m_Clip;
        }

        // --- Render transform ------------------------------------------------------------------------
        // Everything added between a Push and its Pop has its POSITIONS mapped through @p xform, composed
        // with whatever is already on the stack (so a child inherits its parent's). Nothing else changes:
        // UVs, colours and the batch state are untouched, which is precisely why a rotated element does
        // not open a draw call of its own.
        //
        // NOTHING IS PUSHED FOR A NEUTRAL TRANSFORM AND THAT IS THE INVARIANT: with an empty stack the
        // positions are stored verbatim, so a canvas that rotates nothing emits the same bytes it emitted
        // before this existed.
        void PushTransform( const glm::mat3& xform );
        void PopTransform();

        // The accumulated transform, for a caller that must undo it — the hit test inverts THIS matrix
        // rather than rebuilding its own, so the pointer lands where the geometry did.
        const glm::mat3& GetTransform() const
        {
            return m_Transform;
        }
        bool HasTransform() const
        {
            return m_HasTransform;
        }

        // Textured axis-aligned quad. `texture` is an opaque id (engine Image2D*) the backend binds; `uv0`/
        // `uv1` are the top-left / bottom-right texture coordinates (0..1), `tint` multiplies the sampled
        // texel (white = unchanged). Same-texture quads batch together; a new texture opens a new command.
        void AddImage( const void* texture, const glm::vec2& min, const glm::vec2& max, const glm::vec2& uv0,
                       const glm::vec2& uv1, const glm::vec4& tint );

        // Quad filled by a UI-DOMAIN MATERIAL (Ю11). `material` is an opaque id the backend resolves to
        // that material's pipeline and parameter row; `tint` travels as the vertex colour, so a material
        // that multiplies by `v_Color` honours the element's authored Color/Opacity for free and one that
        // ignores it is free to.
        //
        // UVs are 0..1 across the rect and there is NO `rounding` argument, which is a decision and not an
        // omission: a material owns its own shape. It receives that 0..1 UV, so a rounded corner is an SDF
        // it can evaluate — antialiased, and correct under rotation and non-uniform scale — where a
        // tessellated fan would hand it corner vertices whose UVs no longer describe the rect. UIGlass
        // already draws its rounded corners this way for the same reason.
        //
        // Same-material quads batch together; a different material opens a new command, and a material
        // quad never merges with a plain one.
        void AddMaterialRect( const void* material, const glm::vec2& min, const glm::vec2& max,
                              const glm::vec4& tint );

        // Textured quad sampled as an SDF glyph (the backend routes these to the text pipeline). Same args as
        // AddImage; `atlas` is the font's SDF atlas id, `uv0`/`uv1` the glyph's atlas sub-rect, `color` the
        // text colour. Text quads batch separately from image/solid quads even on the same texture.
        void AddText( const void* atlas, const glm::vec2& min, const glm::vec2& max, const glm::vec2& uv0,
                      const glm::vec2& uv1, const glm::vec4& color );

        const std::vector<Vertex2D>& GetVertices() const
        {
            return m_Vertices;
        }
        const std::vector<uint32_t>& GetIndices() const
        {
            return m_Indices;
        }
        const std::vector<DrawCommand>& GetCommands() const
        {
            return m_Commands;
        }

        bool Empty() const
        {
            return m_Indices.empty();
        }

    private:
        // The half of the clip the hardware can cut: the region's box, or a zero rect meaning "no scissor".
        glm::vec4 ScissorBox() const;

        // Returns a command matching the given state (texture + text mode + material), extending the last
        // one when possible or opening a new one anchored at the current end of the index buffer.
        DrawCommand& CurrentCommand( const void* texture, bool text, const void* material = nullptr );

        // Append one textured/tinted quad (the shared path behind AddRectFilled / AddImage / AddText /
        // AddMaterialRect).
        void AddQuad( const void* texture, const glm::vec2& min, const glm::vec2& max, const glm::vec2& uv0,
                      const glm::vec2& uv1, const glm::vec4& color, bool text, const void* material = nullptr );

        // --- The three shapes every primitive here is made of, and the only places geometry is appended ---
        // Each has an EXACT unclipped path — the vertices are stored as given and indexed exactly as they
        // were before an oblique clip existed, so a canvas with no rotated clipper emits the bytes it always
        // emitted — and a clipped path that hands each triangle to EmitClippedTriangle. Splitting them this
        // way is what lets the shared-vertex indexing survive: a clipped triangle owns its corners, an
        // unclipped fan does not.

        // A convex polygon (3 or 4 corners, in order): quads, lines, triangles.
        void EmitPoly( DrawCommand& cmd, const Vertex2D* corners, uint32_t count );
        // A centre plus a closed rim, the last rim vertex bridging back to the first: rounded rectangles.
        void EmitClosedFan( DrawCommand& cmd, const Vertex2D& centre, const Vertex2D* rim, uint32_t rimCount );
        // `pairCount` (outer, inner) vertex pairs, consecutive pairs bridged by two triangles: rings.
        void EmitStrip( DrawCommand& cmd, const Vertex2D* pairs, uint32_t pairCount );

        // Clip one triangle against the region's oblique half-planes and append what survives as its own
        // fan. Called only while at least one such plane is active.
        void EmitClippedTriangle( DrawCommand& cmd, const Vertex2D& a, const Vertex2D& b, const Vertex2D& c );

        bool ClippingOblique() const
        {
            return m_Clip.PlaneCount > 0;
        }

        // The one place a position becomes a vertex. With an empty transform stack it is the identity in
        // the strongest sense — the value is not touched at all.
        glm::vec2 Xf( const glm::vec2& p ) const
        {
            return m_HasTransform ? TransformPoint2D( m_Transform, p ) : p;
        }

        std::vector<Vertex2D>    m_Vertices;
        std::vector<uint32_t>    m_Indices;
        std::vector<DrawCommand> m_Commands;

        // Staging for the one primitive whose corner count is not a compile-time constant (AddRing). Kept
        // on the list so its capacity survives Reset() like the geometry buffers' does.
        std::vector<Vertex2D> m_Scratch;

        ClipRegion2D              m_Clip;
        std::vector<ClipRegion2D> m_ClipStack;

        glm::mat3 m_Transform    = glm::mat3( 1.0f );
        bool      m_HasTransform = false; // == !m_TransformStack.empty(), kept as a flag so
                                          // the per-vertex test is a bool and not a compare
        std::vector<glm::mat3> m_TransformStack;
    };
} // namespace Desert::Graphic::Render2D
