// A minimal XML element reader, engine-internal.
//
// It exists because the two interop formats Sable reads — OpenRaster's
// stack.xml and Krita's maindoc.xml — are the only XML in the project, and
// both are small attribute-only documents written by a program. Pulling in a
// full parser for that would be a new dependency (D-003) for perhaps two
// hundred lines of work.
//
// What it does NOT do, deliberately: namespaces, entity declarations, DTD
// validation, mixed content. Text between elements is skipped, because
// neither format carries any. If a third format needs more than this, that is
// the moment to reach for a real parser rather than to grow this one.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sbl {

struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<XmlNode> children;

    /// Null when absent — the caller decides whether a missing attribute is a
    /// default or an error, and the two are not the same thing in ORA.
    [[nodiscard]] const std::string* attribute(std::string_view key) const noexcept;
    [[nodiscard]] std::string attributeOr(std::string_view key,
                                          std::string fallback) const;
    /// The first direct child with this name, or null.
    [[nodiscard]] const XmlNode* child(std::string_view childName) const noexcept;
};

/// The root element, or nullopt for anything malformed. Never throws: this
/// parses files an artist did not write, so rubbish input is the normal case
/// and has to come back as an error rather than as a crash.
[[nodiscard]] std::optional<XmlNode> parseXml(std::string_view text);

/// The five predefined entities, plus control characters replaced. Layer names
/// come from the artist and a stray '&' would otherwise produce a file no
/// other application can open.
[[nodiscard]] std::string xmlEscape(std::string_view text);

}  // namespace sbl
