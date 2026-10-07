#pragma once
#include <string>
#include <string_view>
#include <sstream>
#include <iomanip>
namespace lhdc {
inline std::string json_string(std::string_view text) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : text) {
        if (c=='"' || c=='\\') out << '\\' << static_cast<char>(c);
        else if (c<32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(c);
        else out << static_cast<char>(c);
    }
    out << '"';
    return out.str();
}
}
