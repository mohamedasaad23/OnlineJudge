#pragma once

#include <string>
#include <optional>

enum class Language { // Supported programming languages.
    Cpp,
    Python
};

std::optional<Language> parseLanguage(const std::string& name);

const char* canonicalName(Language language);

std::string supportedLanguageNames();
