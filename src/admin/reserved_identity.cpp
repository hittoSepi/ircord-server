#include "reserved_identity.hpp"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <sstream>

namespace ircord {

// Exact reserved words (case-insensitive)
const std::unordered_set<std::string> ReservedIdentity::exact_reserved_ = {
    "admin",
    "administrator", 
    "root",
    "sysop",
    "operator",
    "ircord",
    "server",
    "system",
    "owner",
    "founder",
    "boss",
    "god",
    "master",
    "sysadmin",
    "webmaster",
    "postmaster",
    "hostmaster",
    "abuse",
    "security",
    "support",
    "help",
    "info",
    "service",
    "bot",
    " IRCord",  // With common prefixes/suffixes
    "mod",
    "moderator",
    "superuser",
    "su",
};

// Pattern matches (regex)
const std::vector<std::regex> ReservedIdentity::pattern_reserved_ = {
    std::regex("^adm.*", std::regex::icase),       // starts with adm
    std::regex(".*admin.*", std::regex::icase),    // contains admin
    std::regex(".*adm1n.*", std::regex::icase),    // contains adm1n (leet)
    std::regex(".*4dmin.*", std::regex::icase),    // contains 4dmin (leet)
    std::regex("^root.*", std::regex::icase),      // starts with root
    std::regex("^sys.*", std::regex::icase),       // starts with sys
    std::regex("^oper.*", std::regex::icase),      // starts with oper
    std::regex("^ircord.*", std::regex::icase),    // starts with ircord
    std::regex("^server.*", std::regex::icase),    // starts with server
    std::regex("^sys.*admin.*", std::regex::icase), // sysadmin variations
    std::regex("^web.*master.*", std::regex::icase),
    std::regex("^host.*master.*", std::regex::icase),
};

// Leet speak characters that trigger additional checking
const std::unordered_set<char> ReservedIdentity::leet_chars_ = {
    '4', '@', '1', '3', '0', '5', '7', '$', '€', '8', '9'
};

bool ReservedIdentity::is_reserved(std::string_view nickname) {
    return is_reserved(nickname, {});
}

bool ReservedIdentity::is_reserved(std::string_view nickname,
                                   const std::vector<std::string>& additional_patterns) {
    if (nickname.empty()) {
        return false;
    }
    
    // 1. Normalize (lowercase)
    auto normalized = normalize(nickname);
    
    // 2. Check exact matches
    if (exact_reserved_.find(normalized) != exact_reserved_.end()) {
        spdlog::debug("ReservedIdentity: '{}' matches exact reserved list", nickname);
        return true;
    }
    
    // 3. Check pattern matches
    for (const auto& pattern : pattern_reserved_) {
        if (std::regex_match(normalized, pattern)) {
            spdlog::debug("ReservedIdentity: '{}' matches pattern", nickname);
            return true;
        }
    }
    
    // 4. Check additional patterns from config
    for (const auto& pattern_str : additional_patterns) {
        try {
            std::regex pattern(pattern_str, std::regex::icase);
            if (std::regex_match(normalized, pattern)) {
                spdlog::debug("ReservedIdentity: '{}' matches additional pattern '{}'", 
                             nickname, pattern_str);
                return true;
            }
        } catch (const std::regex_error& e) {
            spdlog::warn("Invalid regex pattern in config: {} - {}", pattern_str, e.what());
        }
    }
    
    // 5. Check leet speak variations
    auto leet_normalized = normalize_leet(normalized);
    if (leet_normalized != normalized) {
        // If normalization changed something, check against reserved again
        if (exact_reserved_.find(leet_normalized) != exact_reserved_.end()) {
            spdlog::debug("ReservedIdentity: '{}' matches leet variation of reserved word", 
                         nickname);
            return true;
        }
        
        // Check patterns again with leet-normalized version
        for (const auto& pattern : pattern_reserved_) {
            if (std::regex_match(leet_normalized, pattern)) {
                spdlog::debug("ReservedIdentity: '{}' matches pattern via leet normalization", 
                             nickname);
                return true;
            }
        }
    }
    
    // 6. Check for suspicious patterns (starts with number followed by reserved)
    // e.g., "4dmin", "0perator"
    if (normalized.length() > 1 && std::isdigit(normalized[0])) {
        auto without_prefix = normalized.substr(1);
        if (exact_reserved_.find(without_prefix) != exact_reserved_.end() ||
            std::regex_match(without_prefix, pattern_reserved_[0])) {  // ^adm.*
            spdlog::debug("ReservedIdentity: '{}' has digit prefix before reserved word", 
                         nickname);
            return true;
        }
    }
    
    return false;
}

std::string ReservedIdentity::normalize(std::string_view nickname) {
    std::string result;
    result.reserve(nickname.size());
    
    for (char c : nickname) {
        // Convert to lowercase ASCII
        if (c >= 'A' && c <= 'Z') {
            result += static_cast<char>(c + ('a' - 'A'));
        } else if (c >= 'a' && c <= 'z') {
            result += c;
        } else if (c >= '0' && c <= '9') {
            result += c;
        }
        // Skip special characters and unicode for basic normalization
    }
    
    return result;
}

std::string ReservedIdentity::normalize_leet(std::string_view input) {
    std::string result(input);
    
    // Replace leet characters with their ASCII equivalents
    for (char& c : result) {
        switch (c) {
            case '4':
            case '@':
                c = 'a';
                break;
            case '1':
            case '!':
            case '|':
                c = 'i';
                break;
            case '3':
            case '€':
                c = 'e';
                break;
            case '0':
                c = 'o';
                break;
            case '5':
            case '$':
                c = 's';
                break;
            case '7':
                c = 't';
                break;
            case '8':
                c = 'b';
                break;
            case '9':
            case 'g':  // Sometimes 9 is used for g
                c = 'g';
                break;
            case '2':
                c = 'z';
                break;
            case '+':
                c = 't';
                break;
            case '(':
                c = 'c';
                break;
            case ')':
                c = 'd';
                break;
            default:
                break;
        }
    }
    
    return result;
}

bool ReservedIdentity::is_owner(std::string_view user_id) {
    return user_id == OWNER_ID;
}

bool ReservedIdentity::contains_unicode_homoglyphs(std::string_view nickname) {
    // Check for common unicode homoglyphs (Cyrillic, Greek that look like Latin)
    for (char c : nickname) {
        unsigned char uc = static_cast<unsigned char>(c);
        
        // Check for non-ASCII characters
        if (uc > 127) {
            // This is a simplified check - in production, use ICU library
            // or a comprehensive homoglyph detection library
            spdlog::debug("ReservedIdentity: '{}' contains non-ASCII characters", nickname);
            return true;
        }
        
        // Check for specific problematic bytes that suggest UTF-8 encoding
        // of Cyrillic or other scripts that look like ASCII
        if ((uc & 0xE0) == 0xC0 ||  // 2-byte UTF-8 start
            (uc & 0xF0) == 0xE0 ||  // 3-byte UTF-8 start
            (uc & 0xF8) == 0xF0) {  // 4-byte UTF-8 start
            spdlog::debug("ReservedIdentity: '{}' contains UTF-8 multi-byte sequence", nickname);
            return true;
        }
    }
    
    return false;
}

const std::unordered_set<std::string>& ReservedIdentity::get_exact_reserved() {
    return exact_reserved_;
}

} // namespace ircord
