#pragma once

#include <string>
#include <vector>
#include <cctype>

namespace ircord::utils {

/**
 * Convert a nickname to lowercase for case-insensitive comparison.
 * IRCord nicknames use case-insensitive matching ("Sepi" == "sepi").
 */
inline std::string normalize_nickname(const std::string& nick) {
    std::string result;
    result.reserve(nick.size());
    for (unsigned char c : nick) {
        result.push_back(static_cast<char>(std::tolower(c)));
    }
    return result;
}

/**
 * Check if two nicknames match (case-insensitive).
 */
inline bool nicknames_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != 
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

/**
 * Generate alternative nickname suggestions when the desired nickname is taken.
 * 
 * Suggestions include:
 * - Appending numbers (e.g., "Sepi" -> "Sepi1", "Sepi2", "Sepi3")
 * - Appending underscore (e.g., "Sepi" -> "Sepi_")
 * 
 * @param desired The requested nickname
 * @param count Number of suggestions to generate (default: 3)
 * @return Vector of suggested nicknames
 */
inline std::vector<std::string> generate_nick_suggestions(
    const std::string& desired, 
    size_t count = 3) 
{
    std::vector<std::string> suggestions;
    suggestions.reserve(count + 1);
    
    // Suggest appending numbers (Sepi1, Sepi2, Sepi3, ...)
    for (size_t i = 1; i <= count && suggestions.size() < count; ++i) {
        suggestions.push_back(desired + std::to_string(i));
    }
    
    // If we still need more, suggest underscore suffix
    if (suggestions.size() < count) {
        suggestions.push_back(desired + "_");
    }
    
    return suggestions;
}

/**
 * Format suggestions into a human-readable string for error messages.
 */
inline std::string format_nick_suggestions(const std::vector<std::string>& suggestions) {
    if (suggestions.empty()) return "";
    
    std::string result = " Try: ";
    for (size_t i = 0; i < suggestions.size(); ++i) {
        if (i > 0) {
            result += (i == suggestions.size() - 1) ? ", or " : ", ";
        }
        result += suggestions[i];
    }
    return result;
}

} // namespace ircord::utils
