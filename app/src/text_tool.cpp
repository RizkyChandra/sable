#include "text_tool.hpp"

#include <algorithm>
#include <cmath>

#include "imgui.h"

namespace {

/// How far Ctrl+arrow nudges the text block, in canvas pixels. Big enough to
/// move it across a canvas without a hundred presses, small enough to place it.
constexpr double kNudge = 8.0;

}  // namespace

// ------------------------------------------------------------------- session

void TextTool::begin(SDL_Window* window, double canvasX, double canvasY,
                     sbl::StraightRgba8 colour) {
    active_  = true;
    layer_   = sbl::NO_LAYER;
    caret_   = 0;
    composition_.clear();
    compositionCaret_ = 0;
    session_ = sbl::UndoRecord{};
    status_.clear();

    content_ = sbl::TextContent{};
    content_.x = canvasX;
    content_.y = canvasY;
    content_.colour = colour;
    if (!face_.has_value()) loadFont({});
    if (face_.has_value()) {
        content_.fontPath = face_->path().string();
        content_.fontName = face_->familyName();
    }
    SDL_StartTextInput(window);
}

bool TextTool::resume(SDL_Window* window, sbl::Document& doc, sbl::LayerId id) {
    sbl::Layer* layer = doc.layerById(id);
    if (layer == nullptr || !layer->text.has_value()) return false;

    active_  = true;
    layer_   = id;
    content_ = *layer->text;
    caret_   = content_.utf8.size();
    composition_.clear();
    compositionCaret_ = 0;
    session_      = sbl::UndoRecord{};
    sessionProps_ = sbl::propsOf(*layer);
    status_.clear();

    loadFont(content_.fontPath);
    doc.activeLayer = id;
    SDL_StartTextInput(window);
    return true;
}

void TextTool::finish(SDL_Window* window, sbl::Document& doc) {
    if (!active_) return;
    active_ = false;
    SDL_StopTextInput(window);

    // A preedit the input method never finished is not text the artist typed,
    // so the last thing drawn is the committed string on its own.
    const bool hadComposition = !composition_.empty();
    composition_.clear();
    compositionCaret_ = 0;
    if (hadComposition && layer_ != sbl::NO_LAYER) redraw(doc);

    if (layer_ != sbl::NO_LAYER) {
        session_.label = "Text";
        // The words travel with the pixels, in one record: undoing a text edit
        // has to put back what it said as well as how it looked, or the next
        // edit resumes from a string that is not on the canvas.
        session_.structure = sbl::LayerStructureDelta{
            sbl::LayerChange::Properties, layer_, 0, std::nullopt, sessionProps_};
        doc.undo.push(std::move(session_));
    }
    session_ = sbl::UndoRecord{};
    layer_   = sbl::NO_LAYER;
}

void TextTool::ensureLayer(sbl::Document& doc) {
    if (layer_ != sbl::NO_LAYER) return;

    sbl::UndoRecord rec = sbl::addLayerAbove(doc, doc.activeLayer, "Text");
    rec.label = "New text layer";
    layer_ = doc.activeLayer;                 // addLayerAbove made it active

    sbl::Layer* layer = doc.layerById(layer_);
    if (layer == nullptr) { layer_ = sbl::NO_LAYER; return; }
    layer->kind = sbl::LayerKind::Text;
    sessionProps_ = sbl::propsOf(*layer);     // empty text: what undo restores

    doc.undo.push(std::move(rec));
    doc.dirty = true;
}

// -------------------------------------------------------------------- drawing

sbl::TextContent TextTool::shown() const {
    sbl::TextContent out = content_;
    if (!composition_.empty()) out.utf8.insert(caret_, composition_);
    return out;
}

std::size_t TextTool::shownCaret() const {
    return caret_ + std::min(compositionCaret_, composition_.size());
}

void TextTool::redraw(sbl::Document& doc) {
    sbl::Layer* layer = doc.layerById(layer_);
    if (layer == nullptr || !face_.has_value()) return;

    sbl::UndoRecord rec =
        sbl::drawTextLayer(*layer, shown(), *face_, doc.width, doc.height);
    for (const sbl::TileSnapshot& snap : rec.tiles)
        changed_.emplace_back(layer_, snap.key);

    // The committed string only. The preedit is on screen, never in the file.
    layer->text = content_;
    sbl::mergeTextRecord(session_, std::move(rec));
    doc.dirty = true;
}

bool TextTool::loadFont(const std::string& path) {
    if (face_.has_value() && !path.empty() && face_->path() == path) return true;
    status_.clear();      // whatever went wrong last time is not this font's

    if (!path.empty()) {
        if (auto loaded = sbl::FontFace::load(path); loaded.has_value()) {
            face_.emplace(std::move(*loaded));
            return true;
        }
    }
    // A font that has been uninstalled since the file was saved must not stop
    // the artist editing the text — the drawing itself is pixels and is already
    // safe. Substitute, and say which one.
    //
    // Preferred first: "whatever sorts first alphabetically" is how you end up
    // defaulting to a decorative face someone installed once.
    const std::vector<sbl::FontEntry>& fonts = sbl::systemFonts();
    const auto substitute = [&](const sbl::FontEntry& entry) {
        auto loaded = sbl::FontFace::load(entry.path);
        if (!loaded.has_value()) return false;
        face_.emplace(std::move(*loaded));
        // Silent when there was nothing to substitute for — the first font of
        // a fresh session is a default, not a failure.
        if (!path.empty())
            status_ = "The font this text was written in is missing; showing " +
                      face_->familyName() + ".";
        return true;
    };
    // Preferred names first: "whatever sorts first alphabetically" is how you
    // end up defaulting to a decorative face someone installed once.
    for (const char* wanted : {"NotoSans-Regular", "DejaVuSans",
                               "LiberationSans-Regular", "Arial", "Helvetica"})
        for (const sbl::FontEntry& entry : fonts)
            if (entry.name == wanted && substitute(entry)) return true;
    for (const sbl::FontEntry& entry : fonts)
        if (substitute(entry)) return true;

    face_.reset();
    status_ = "No usable font was found on this machine.";
    return false;
}

bool TextTool::useFont(sbl::Document& doc, const std::filesystem::path& path) {
    if (!loadFont(path.string())) return false;
    content_.fontPath = face_->path().string();
    content_.fontName = face_->familyName();
    redraw(doc);
    return true;
}

void TextTool::setColour(sbl::Document& doc, sbl::StraightRgba8 colour) {
    if (!active_ || content_.colour == colour) return;
    content_.colour = colour;
    redraw(doc);
}

std::vector<std::pair<sbl::LayerId, sbl::TileKey>> TextTool::takeChanged() {
    return std::exchange(changed_, {});
}

// --------------------------------------------------------------------- input

namespace {

/// Where a byte offset sits along its line, in canvas pixels.
double caretX(const sbl::FontFace& face, const sbl::TextContent& content,
              const sbl::TextLine& line, std::size_t offset) {
    const std::size_t at = std::clamp(offset, line.begin, line.end);
    return line.x + face.advanceWidth(
        std::string_view(content.utf8).substr(line.begin, at - line.begin),
        content.sizePx);
}

/// The offset on `line` nearest to a target x — what Up and Down have to find
/// if the caret is to stay under the artist's eye on proportional type.
std::size_t nearestOffset(const sbl::FontFace& face, const sbl::TextContent& content,
                          const sbl::TextLine& line, double x) {
    std::size_t best = line.begin;
    double bestDistance = 1e30;
    for (std::size_t at = line.begin;; at = sbl::utf8Next(content.utf8, at)) {
        const double distance = std::abs(caretX(face, content, line, at) - x);
        if (distance < bestDistance) { bestDistance = distance; best = at; }
        if (at >= line.end) break;
    }
    return best;
}

}  // namespace

bool TextTool::handleKey(const SDL_KeyboardEvent& key, sbl::Document& doc) {
    // While the input method is composing, the keys belong to it. Acting on
    // them here is what makes a CJK candidate list impossible to steer.
    if (!composition_.empty()) return true;

    const bool ctrl = (key.mod & SDL_KMOD_CTRL) != 0;

    switch (key.key) {
        case SDLK_BACKSPACE:
            if (caret_ > 0) {
                const std::size_t from = sbl::utf8Prev(content_.utf8, caret_);
                content_.utf8.erase(from, caret_ - from);
                caret_ = from;
                redraw(doc);
            }
            return true;

        case SDLK_DELETE:
            if (caret_ < content_.utf8.size()) {
                const std::size_t to = sbl::utf8Next(content_.utf8, caret_);
                content_.utf8.erase(caret_, to - caret_);
                redraw(doc);
            }
            return true;

        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            ensureLayer(doc);
            content_.utf8.insert(caret_, "\n");
            ++caret_;
            redraw(doc);
            return true;

        case SDLK_LEFT:
            // Ctrl moves the whole block, along the CANVAS axes: the text is
            // laid out on those, so a turned view turns the text with it and
            // "left" still means the direction the letters run back towards.
            if (ctrl) { content_.x -= kNudge; redraw(doc); }
            else      caret_ = sbl::utf8Prev(content_.utf8, caret_);
            return true;

        case SDLK_RIGHT:
            if (ctrl) { content_.x += kNudge; redraw(doc); }
            else      caret_ = sbl::utf8Next(content_.utf8, caret_);
            return true;

        case SDLK_UP:
        case SDLK_DOWN: {
            if (ctrl) {
                content_.y += key.key == SDLK_UP ? -kNudge : kNudge;
                redraw(doc);
                return true;
            }
            if (!face_.has_value()) return true;
            const sbl::TextLayout layout = sbl::layoutText(*face_, content_);
            const std::size_t here = sbl::lineOf(layout, caret_);
            const std::size_t there = key.key == SDLK_UP
                ? (here == 0 ? 0 : here - 1)
                : std::min(here + 1, layout.lines.size() - 1);
            if (there == here) return true;
            const double x = caretX(*face_, content_, layout.lines[here], caret_);
            caret_ = nearestOffset(*face_, content_, layout.lines[there], x);
            return true;
        }

        case SDLK_HOME:
        case SDLK_END: {
            if (!face_.has_value()) return true;
            const sbl::TextLayout layout = sbl::layoutText(*face_, content_);
            const sbl::TextLine& line = layout.lines[sbl::lineOf(layout, caret_)];
            caret_ = key.key == SDLK_HOME ? line.begin : line.end;
            return true;
        }

        default:
            // Everything else is swallowed on purpose. A tool shortcut firing
            // mid-sentence would switch tools and lose the session.
            return true;
    }
}

bool TextTool::handleEvent(const SDL_Event& e, sbl::Document& doc,
                           SDL_Window* window) {
    if (!active_) return false;

    switch (e.type) {
        case SDL_EVENT_TEXT_INPUT: {
            if (e.text.text == nullptr || *e.text.text == '\0') return true;
            ensureLayer(doc);
            const std::string typed = e.text.text;
            content_.utf8.insert(caret_, typed);
            caret_ += typed.size();
            // The input method has finished with it; the preedit is now real
            // text and must not be drawn twice.
            composition_.clear();
            compositionCaret_ = 0;
            redraw(doc);
            return true;
        }

        case SDL_EVENT_TEXT_EDITING: {
            composition_ = e.edit.text != nullptr ? e.edit.text : "";
            // SDL reports the cursor in characters; a byte offset is what we
            // slice strings with, so walk that many characters in.
            compositionCaret_ = 0;
            for (std::int32_t i = 0; i < e.edit.start && compositionCaret_ < composition_.size();
                 ++i)
                compositionCaret_ = sbl::utf8Next(composition_, compositionCaret_);
            if (e.edit.start < 0) compositionCaret_ = composition_.size();
            if (!composition_.empty()) ensureLayer(doc);
            if (layer_ != sbl::NO_LAYER) redraw(doc);
            return true;
        }

        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_ESCAPE ||
                ((e.key.mod & SDL_KMOD_CTRL) != 0 &&
                 (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER))) {
                finish(window, doc);
                return true;
            }
            return handleKey(e.key, doc);

        case SDL_EVENT_KEY_UP:
            return true;      // its key down was ours, so this is too

        default:
            return false;
    }
}

// ------------------------------------------------------------------- overlay

void TextTool::frame(SDL_Window* window, const View& view, float uiScale) {
    if (!active_ || !face_.has_value()) return;

    // ImGui's SDL3 backend switches text input off whenever one of its own
    // fields loses focus, which is exactly what happens when the artist clicks
    // from a panel back onto the canvas. Re-assert it rather than discover the
    // keyboard has gone quiet mid-sentence.
    if (!SDL_TextInputActive(window)) SDL_StartTextInput(window);

    const sbl::TextContent display = shown();
    const sbl::TextLayout layout = sbl::layoutText(*face_, display);
    const sbl::TextLine& line = layout.lines[sbl::lineOf(layout, shownCaret())];
    const double x = caretX(*face_, display, line, shownCaret());

    // Through the view transform like everything else on the canvas, so the
    // caret leans with the picture when the canvas is turned.
    const auto at = [&](double cx, double cy) {
        return ImVec2(static_cast<float>(toScreenX(view, cx, cy)),
                      static_cast<float>(toScreenY(view, cx, cy)));
    };
    const ImVec2 top    = at(x, line.baseline - layout.ascent);
    const ImVec2 bottom = at(x, line.baseline + layout.descent);

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    // Two-tone, like the selection outline: readable over dark and light art.
    draw->AddLine(top, bottom, IM_COL32(0, 0, 0, 160), 3.0f * uiScale);
    draw->AddLine(top, bottom, IM_COL32(255, 255, 255, 230), 1.0f * uiScale);

    if (!composition_.empty()) {
        // The underline every input method draws under an unfinished word. It
        // is UI, not paint, so it lives here and not in the layer.
        const double from = caretX(*face_, display, line, caret_);
        const double to   = caretX(*face_, display, line,
                                   caret_ + composition_.size());
        const double under = line.baseline + layout.descent * 0.4;
        draw->AddLine(at(from, under), at(to, under),
                      IM_COL32(120, 200, 255, 230), 2.0f * uiScale);
    }

    // Where the input method should put its candidate window: beside the
    // caret, never on top of the characters being chosen.
    const SDL_Rect area{
        static_cast<int>(std::lround(std::min(top.x, bottom.x))),
        static_cast<int>(std::lround(std::min(top.y, bottom.y))),
        std::max(1, static_cast<int>(std::lround(std::abs(bottom.x - top.x)))),
        std::max(1, static_cast<int>(std::lround(std::abs(bottom.y - top.y))))};
    SDL_SetTextInputArea(window, &area, 0);
}

// --------------------------------------------------------------------- panel

void TextTool::drawPanel(sbl::Document& doc) {
    const std::vector<sbl::FontEntry>& fonts = sbl::systemFonts();

    if (!active_) {
        ImGui::TextDisabled("Click the canvas, or press Enter, to place text.");
        if (fonts.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                               "No fonts were found on this machine.");
        return;
    }

    bool changed = false;

    const std::string current =
        face_.has_value() ? face_->familyName() : std::string("(none)");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##font", current.c_str())) {
        for (const sbl::FontEntry& entry : fonts) {
            const bool selected = face_.has_value() && face_->path() == entry.path;
            if (ImGui::Selectable(entry.name.c_str(), selected))
                changed |= useFont(doc, entry.path);
        }
        ImGui::EndCombo();
    }

    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderFloat("##textsize", &content_.sizePx, 6.0f, 400.0f,
                                  "%.0f px", ImGuiSliderFlags_Logarithmic);
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::SliderFloat("##textspacing", &content_.lineSpacing, 0.6f, 3.0f,
                                  "line spacing %.2f");

    int align = static_cast<int>(content_.align);
    changed |= ImGui::RadioButton("Left", &align, 0);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Centre", &align, 1);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Right", &align, 2);
    content_.align = static_cast<sbl::TextAlign>(align);

    ImGui::TextDisabled("Escape finishes. Ctrl+arrows move it.");
    ImGui::TextDisabled("The colour is the foreground colour.");
    if (!status_.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", status_.c_str());

    // The trap this tool exists to avoid: typing Japanese into a font that has
    // none of it produces a row of empty boxes and no explanation at all.
    if (face_.has_value() &&
        face_->firstMissingGlyph(content_.utf8) != std::string::npos)
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                           "This font has no glyph for some of these characters.");

    if (changed) redraw(doc);
}
