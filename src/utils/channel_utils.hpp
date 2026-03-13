#pragma once

#include <string>
#include <algorithm>
#include <cctype>

namespace ircord::utils {

/**
 * Valid IRC channel name characters (after the prefix):
 * - Alphanumeric characters (a-z, A-Z, 0-9)
 * - Special characters: - _ [ \ ] { } ^ ` | ~
 * 
 * Channel prefixes: # & ! +
 */

/**
 * Check if a character is a valid IRC channel prefix.
 * Valid prefixes: # & ! +
 */
inline bool is_channel_prefix(char c) {
    return c == '#' || c == '&' || c == '!' || c == '+';
}

/**
 * Check if a character is valid for use in a channel name.
 * Valid characters: alphanumeric, -, _, [, ], {, }, ^, `, |, ~
 */
inline bool is_valid_channel_char(char c) {
    // Alphanumeric
    if (std::isalnum(static_cast<unsigned char>(c))) {
        return true;
    }
    // Allowed special characters
    switch (c) {
        case '-':
        case '_':
        case '[':
        case ']':
        case '{':
        case '}':
        case '^':
        case '`':
        case '|':
        case '~':
            return true;
        default:
            return false;
    }
}

/**
 * Sanitize a channel name according to IRC standards:
 * 1. Auto-add # prefix if missing (or any other channel prefix)
 * 2. Strip invalid characters
 * 3. Remove duplicate prefixes (keep only first #)
 * 4. Trim whitespace
 * 
 * @param input The raw channel name input from user
 * @return Sanitized channel name with # prefix
 * 
 * Examples:
 *   "general"       -> "#general"
 *   "#general"      -> "#general"
 *   "general chat"  -> "#generalchat"
 *   "##test"        -> "#test"
 *   "#general!"     -> "#general"
 *   "  #general  "  -> "#general"
 */
inline std::string sanitize_channel_name(const std::string& input) {
    if (input.empty()) {
        return "#";
    }

    std::string result;
    result.reserve(input.size() + 1);  // +1 for potential # prefix

    bool has_prefix = false;
    bool prefix_added = false;

    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];

        // Skip leading whitespace
        if (!prefix_added && std::isspace(static_cast<unsigned char>(c))) {
            continue;
        }

        // Handle channel prefix (# & ! +)
        if (is_channel_prefix(c)) {
            if (!has_prefix) {
                // First prefix character - use # as the canonical prefix
                result.push_back('#');
                has_prefix = true;
                prefix_added = true;
            }
            // Skip duplicate prefixes (e.g., ##test -> #test)
            continue;
        }

        // Now we're past any potential prefix
        prefix_added = true;

        // Skip invalid characters (spaces, control chars, punctuation, etc.)
        if (!is_valid_channel_char(c)) {
            continue;
        }

        result.push_back(c);
    }

    // If no prefix was found, add # at the beginning
    if (!has_prefix) {
        result.insert(result.begin(), '#');
    }

    // Handle edge case: result is just "#" (no valid characters)
    if (result == "#") {
        return "#";
    }

    return result;
}

/**
 * Check if a channel name is valid (has content after the prefix).
 * 
 * @param channel The channel name to validate
 * @return true if valid (has content after #), false otherwise
 */
inline bool is_valid_channel_name(const std::string& channel) {
    if (channel.empty()) {
        return false;
    }
    // Must start with a valid prefix
    if (!is_channel_prefix(channel[0])) {
        return false;
    }
    // Must have at least one character after the prefix
    return channel.length() > 1;
}

} // namespace ircord::utils
