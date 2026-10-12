#include "core/Language.hpp"

#include <vector>

namespace {

    struct LanguageEntry {
        Language language; // enum value for internal use
        const char* canonical; // canonical name for database and API
        std::vector<const char*> aliases; // all accepted spellings, including canonical name
    };

    // Return a static table of all supported languages and their aliases.
    const std::vector<LanguageEntry>& languageTable() {
        static const std::vector<LanguageEntry> table = {
            {
                Language::Cpp,
                "cpp",
                {"c++", "cpp17"}
            },
            {
                Language::Python,
                "python",
                {"python3", "py"}
            },
        };
        return table;
    }

} // namespace

std::optional<Language> parseLanguage(const std::string& name) {
    for (const LanguageEntry& entry : languageTable()) {
        if (name == entry.canonical) return entry.language;

        for (const char* alias : entry.aliases) {
            if (name == alias) return entry.language;
        }
    }

    return std::nullopt; // No match found
}

const char* canonicalName(Language language) {
    for (const LanguageEntry& entry : languageTable()) {
        if (entry.language == language) return entry.canonical;
    }

    return "unknown"; // This should never happen if the enum and table are kept in sync.
}

std::string supportedLanguageNames() {
    std::string names;
    for (const LanguageEntry& entry : languageTable()) {
        if (!names.empty()) names += ", ";
        names += entry.canonical;
        for (const char* alias : entry.aliases) {
            names += ", ";
            names += alias;
        }
    }
    return names;
}
