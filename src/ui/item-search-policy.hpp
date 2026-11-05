#pragma once
#include <string>
#include <string_view>

// « Recherche d'objets »: how a typed text matches an item's name. Pure. Case and
// accents do not count, and every word typed must appear, in any order.
namespace itemSearch {
inline std::string Fold(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) { out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : static_cast<char>(c)); continue; }
        if (c == 0xC3 && i + 1 < text.size()) {
            const auto next = static_cast<unsigned char>(text[++i]);
            constexpr std::string_view Lower = "aaaaaaaceeeeiiii?nooooo?ouuuuy?";
            const auto index = (next >= 0xA0 ? next - 0xA0 : next >= 0x80 ? next - 0x80 : 0xFF);
            if (index < Lower.size() && Lower[index] != '?') {
                if (next == 0x86 || next == 0xA6) out += "ae";
                else out.push_back(Lower[index]);
            } else out.push_back('?');
            continue;
        }
        if (c == 0xC5 && i + 1 < text.size() && (static_cast<unsigned char>(text[i + 1]) == 0x92 || static_cast<unsigned char>(text[i + 1]) == 0x93)) { out += "oe"; ++i; continue; }
        out.push_back(static_cast<char>(c));
    }
    return out;
}
inline bool Matches(std::string_view name, std::string_view query) {
    const auto folded = Fold(name), wanted = Fold(query);
    size_t start = 0, words = 0;
    while (start < wanted.size()) {
        const auto end = wanted.find(' ', start);
        const auto word = std::string_view(wanted).substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!word.empty()) {
            ++words;
            if (folded.find(word) == std::string::npos) return false;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return words != 0;
}
// A search starts at two characters: one letter would list the whole inventory.
inline bool Searchable(std::string_view query) {
    size_t letters = 0;
    for (const auto c : Fold(query)) if (c != ' ') ++letters;
    return letters >= 2;
}
}
