#pragma once

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <regex>

namespace ircord {

/**
 * @brief Manages reserved nicknames/identifiers that cannot be registered by regular users.
 * 
 * This class prevents users from registering names like "admin", "root", "operator"
 * and their variations (leet speak, unicode homoglyphs, pattern matches).
 */
class ReservedIdentity {
public:
    // The fixed user ID for the server owner
    static constexpr const char* OWNER_ID = "admin";
    
    /**
     * @brief Check if a nickname is reserved.
     * @param nickname The nickname to check
     * @return true if the nickname is reserved
     */
    static bool is_reserved(std::string_view nickname);
    
    /**
     * @brief Check if a nickname is reserved (including additional patterns from config).
     * @param nickname The nickname to check
     * @param additional_patterns Additional regex patterns from config
     * @return true if the nickname is reserved
     */
    static bool is_reserved(std::string_view nickname, 
                           const std::vector<std::string>& additional_patterns);
    
    /**
     * @brief Normalize a nickname for comparison.
     * Converts to lowercase and removes special characters.
     */
    static std::string normalize(std::string_view nickname);
    
    /**
     * @brief Normalize leet speak to regular text.
     * 4->a, 1->i, 3->e, 0->o, @->a, etc.
     */
    static std::string normalize_leet(std::string_view input);
    
    /**
     * @brief Check if the user ID is the server owner.
     */
    static bool is_owner(std::string_view user_id);
    
    /**
     * @brief Check if the nickname contains unicode homoglyphs.
     * (e.g., Cyrillic 'а' that looks like Latin 'a')
     */
    static bool contains_unicode_homoglyphs(std::string_view nickname);
    
    /**
     * @brief Get the list of exact reserved words.
     */
    static const std::unordered_set<std::string>& get_exact_reserved();

private:
    // Prevent instantiation - static class
    ReservedIdentity() = delete;
    
    // Initialize static data
    static const std::unordered_set<std::string> exact_reserved_;
    static const std::vector<std::regex> pattern_reserved_;
    static const std::unordered_set<char> leet_chars_;
};

} // namespace ircord
