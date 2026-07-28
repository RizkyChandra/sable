// The on-canvas text tool (#20): SDL3 text input, an inline IME preedit, and a
// caret that follows the view.
//
// D-002 cost #2 said this day would come: "Weak IME and text input... plan to
// lean on SDL3's text-input APIs there rather than ImGui's default field
// behaviour." So nothing here is an ImGui::InputText. Characters arrive as
// SDL_EVENT_TEXT_INPUT and half-finished ones as SDL_EVENT_TEXT_EDITING, which
// is the only route an input method can reach us by — and the audience that
// route exists for is everyone typing Chinese, Japanese or Korean.
//
// The composed text is drawn INTO the layer, not over it: the artist sees the
// real glyphs, at the real size, composited by the one compositor that also
// does the export (#1). Only the caret and the preedit underline are overlays,
// for the same reason the selection outline is one — they are not part of the
// picture.
#pragma once

#include <SDL3/SDL.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sbl/canvas.hpp"
#include "sbl/text.hpp"
#include "view_transform.hpp"

class TextTool {
public:
    [[nodiscard]] bool active() const noexcept { return active_; }

    /// Starts a new text at a canvas point.
    ///
    /// No layer is created until the first character arrives, so a click that
    /// the artist thought better of costs neither a layer nor an undo step.
    void begin(SDL_Window* window, double canvasX, double canvasY,
               sbl::StraightRgba8 colour);

    /// Reopens the text already on `id`, caret at the end. False if that layer
    /// carries none — including the case where its font is gone, which is
    /// reported through `status()` rather than by refusing to edit.
    bool resume(SDL_Window* window, sbl::Document& doc, sbl::LayerId id);

    /// True when the event was the text's.
    ///
    /// While a session is live this swallows the keyboard whole: `b` has to
    /// type a b, not switch to the brush. Escape ends the session and gives the
    /// shortcuts back, which is the one key that always means that.
    bool handleEvent(const SDL_Event& e, sbl::Document& doc, SDL_Window* window);

    /// Ends the session, pushing ONE undo step for the words and the glyphs
    /// alike. Safe to call when nothing is being edited.
    void finish(SDL_Window* window, sbl::Document& doc);

    /// Once a frame: keeps SDL's text input switched on (ImGui turns it off
    /// whenever one of its own fields loses focus), tells the input method
    /// where the caret is so its candidate window does not cover the word being
    /// typed, and draws the caret and preedit underline.
    void frame(SDL_Window* window, const View& view, float uiScale);

    /// Font, size, alignment and line spacing, for the tool panel.
    void drawPanel(sbl::Document& doc);

    /// Switches the font the text is drawn in, redrawing it. An empty path, or
    /// one that will not load, falls back to a usable font and says so through
    /// `status()` rather than leaving the artist with nothing on the canvas.
    bool useFont(sbl::Document& doc, const std::filesystem::path& path);

    /// Tiles whose pixels moved since the last call, for the texture cache.
    [[nodiscard]] std::vector<std::pair<sbl::LayerId, sbl::TileKey>> takeChanged();

    /// Empty unless something needs saying — a font that would not load, or one
    /// substituted for a missing one.
    [[nodiscard]] const std::string& status() const noexcept { return status_; }

    /// The colour to draw in. Kept in step with the colour panel while a
    /// session is live, which is what makes recolouring text free of any UI.
    void setColour(sbl::Document& doc, sbl::StraightRgba8 colour);

private:
    /// Creates the layer on first input, and pushes its own undo step.
    void ensureLayer(sbl::Document& doc);
    /// Re-rasterises the layer from `content_` plus any preedit.
    void redraw(sbl::Document& doc);
    /// Loads `path`, falling back to the first font on the machine.
    bool loadFont(const std::string& path);
    /// The text as it should currently appear: committed plus half-typed.
    [[nodiscard]] sbl::TextContent shown() const;
    /// Caret offset into `shown().utf8`.
    [[nodiscard]] std::size_t shownCaret() const;
    /// Handles one key press. Returns false only for keys that end the session.
    bool handleKey(const SDL_KeyboardEvent& key, sbl::Document& doc);

    bool          active_ = false;
    sbl::LayerId  layer_  = sbl::NO_LAYER;
    sbl::TextContent content_;
    std::size_t   caret_ = 0;            // byte offset into content_.utf8

    // The IME's half-finished character. Never part of the document: it is
    // drawn so the artist can see it, and replaced wholesale by the TEXT_INPUT
    // event that finishes it.
    std::string   composition_;
    std::size_t   compositionCaret_ = 0;

    std::optional<sbl::FontFace> face_;
    sbl::UndoRecord  session_;           // accumulates the whole edit
    sbl::LayerProps  sessionProps_;      // the layer before the session started
    std::vector<std::pair<sbl::LayerId, sbl::TileKey>> changed_;
    std::string   status_;
};
