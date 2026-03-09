/**
 * 
 * 
 */

#pragma once
#include <string>
#include <unordered_map>
#include <boost/algorithm/string/replace.hpp>

namespace ircord::utils {

    typedef std::unordered_map<std::string, std::string> template_map;

    inline std::string format_template(std::string tmpl, const template_map &vars)
    {
        for (const auto &[key, value] : vars)
        {
            std::string placeholder = "${" + key + "}";
            boost::replace_all(tmpl, placeholder, value);
        }
        return tmpl;
    }
}