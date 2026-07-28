#include "xml.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace sbl {
namespace {

/// Deep enough for any layer stack a person will make, shallow enough that a
/// hostile file cannot recurse the parser off the end of the stack.
constexpr int kMaxDepth = 64;

bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool isNameChar(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '_' || c == '-' || c == '.' || c == ':';
}

void skipSpace(std::string_view s, std::size_t& i) noexcept {
    while (i < s.size() && isSpace(s[i])) ++i;
}

std::string decodeEntities(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') { out += text[i]; continue; }

        const std::size_t end = text.find(';', i + 1);
        // An unterminated or absurdly long "&" is a literal ampersand, not a
        // reason to reject the file.
        if (end == std::string_view::npos || end - i > 12) { out += '&'; continue; }

        const std::string_view name = text.substr(i + 1, end - i - 1);
        if      (name == "amp")  out += '&';
        else if (name == "lt")   out += '<';
        else if (name == "gt")   out += '>';
        else if (name == "quot") out += '"';
        else if (name == "apos") out += '\'';
        else if (name.size() >= 2 && name[0] == '#') {
            const std::string digits(name.substr(name[1] == 'x' ? 2 : 1));
            const unsigned long code =
                std::strtoul(digits.c_str(), nullptr, name[1] == 'x' ? 16 : 10);
            // UTF-8, because every name in these files ends up in a std::string
            // that the UI draws as UTF-8.
            if (code < 0x80) {
                out += static_cast<char>(code);
            } else if (code < 0x800) {
                out += static_cast<char>(0xC0 | (code >> 6));
                out += static_cast<char>(0x80 | (code & 0x3F));
            } else if (code < 0x10000) {
                out += static_cast<char>(0xE0 | (code >> 12));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            } else {
                out += '?';
            }
        } else {
            out += '&';
            out += name;
            out += ';';
        }
        i = end;
    }
    return out;
}

/// Skips to just past `terminator`, respecting nothing else. Used for the
/// constructs these files contain but Sable ignores.
bool skipPast(std::string_view s, std::size_t& i, std::string_view terminator) {
    const std::size_t at = s.find(terminator, i);
    if (at == std::string_view::npos) return false;
    i = at + terminator.size();
    return true;
}

/// A prolog, comment, DOCTYPE or CDATA section at `i`. False if `i` is the
/// start of a real element instead.
bool skipNonElement(std::string_view s, std::size_t& i) {
    if (!s.substr(i).starts_with("<")) return false;
    if (s.substr(i).starts_with("<?"))       return skipPast(s, i, "?>");
    if (s.substr(i).starts_with("<!--"))     return skipPast(s, i, "-->");
    if (s.substr(i).starts_with("<![CDATA[")) return skipPast(s, i, "]]>");
    if (s.substr(i).starts_with("<!")) {
        // A DOCTYPE, whose public identifiers contain '>' inside quotes —
        // Krita's maindoc.xml has exactly that, so a naive find('>') is wrong.
        i += 2;
        char quote = '\0';
        for (; i < s.size(); ++i) {
            if (quote != '\0') { if (s[i] == quote) quote = '\0'; continue; }
            if (s[i] == '"' || s[i] == '\'') quote = s[i];
            else if (s[i] == '>') { ++i; return true; }
        }
        return false;
    }
    return false;
}

std::optional<XmlNode> parseElement(std::string_view s, std::size_t& i, int depth);

/// Everything up to the matching end tag.
bool parseChildren(XmlNode& node, std::string_view s, std::size_t& i, int depth) {
    while (true) {
        // Text between elements: neither format uses it, so it is skipped.
        const std::size_t next = s.find('<', i);
        if (next == std::string_view::npos) return false;
        i = next;

        if (s.substr(i).starts_with("</")) {
            return skipPast(s, i, ">");
        }
        if (skipNonElement(s, i)) continue;

        std::optional<XmlNode> child = parseElement(s, i, depth + 1);
        if (!child.has_value()) return false;
        node.children.push_back(std::move(*child));
    }
}

std::optional<XmlNode> parseElement(std::string_view s, std::size_t& i, int depth) {
    if (depth > kMaxDepth || i >= s.size() || s[i] != '<') return std::nullopt;
    ++i;

    XmlNode node;
    const std::size_t nameStart = i;
    while (i < s.size() && isNameChar(s[i])) ++i;
    if (i == nameStart) return std::nullopt;
    node.name = s.substr(nameStart, i - nameStart);

    while (true) {
        skipSpace(s, i);
        if (i >= s.size()) return std::nullopt;

        if (s[i] == '>') {
            ++i;
            return parseChildren(node, s, i, depth) ? std::optional{std::move(node)}
                                                    : std::nullopt;
        }
        if (s[i] == '/') {
            ++i;
            if (i >= s.size() || s[i] != '>') return std::nullopt;
            ++i;
            return node;
        }

        const std::size_t keyStart = i;
        while (i < s.size() && isNameChar(s[i])) ++i;
        if (i == keyStart) return std::nullopt;
        std::string key(s.substr(keyStart, i - keyStart));

        skipSpace(s, i);
        if (i >= s.size() || s[i] != '=') return std::nullopt;
        ++i;
        skipSpace(s, i);
        if (i >= s.size() || (s[i] != '"' && s[i] != '\'')) return std::nullopt;

        const char quote = s[i++];
        const std::size_t valueStart = i;
        while (i < s.size() && s[i] != quote) ++i;
        if (i >= s.size()) return std::nullopt;
        node.attributes.emplace_back(
            std::move(key), decodeEntities(s.substr(valueStart, i - valueStart)));
        ++i;
    }
}

}  // namespace

const std::string* XmlNode::attribute(std::string_view key) const noexcept {
    for (const auto& [k, v] : attributes)
        if (k == key) return &v;
    return nullptr;
}

std::string XmlNode::attributeOr(std::string_view key, std::string fallback) const {
    const std::string* found = attribute(key);
    return found != nullptr ? *found : std::move(fallback);
}

const XmlNode* XmlNode::child(std::string_view childName) const noexcept {
    for (const XmlNode& c : children)
        if (c.name == childName) return &c;
    return nullptr;
}

std::optional<XmlNode> parseXml(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        skipSpace(text, i);
        if (i >= text.size() || text[i] != '<') return std::nullopt;
        if (!skipNonElement(text, i)) return parseElement(text, i, 0);
    }
    return std::nullopt;
}

std::string xmlEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                // Control characters are not legal XML at all, and a layer
                // name is whatever the artist typed or another program wrote.
                if (static_cast<unsigned char>(c) < 0x20) out += ' ';
                else                                      out += c;
        }
    }
    return out;
}

}  // namespace sbl
