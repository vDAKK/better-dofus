#include "ui/item-search-policy.hpp"
#include <iostream>
#include <stdexcept>

using namespace itemSearch;

static void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

int main() {
    try {
        Check(Fold("Blé") == "ble" && Fold("Bois de Frêne") == "bois de frene", "Case and accents do not count");
        Check(Fold("Œuf") != "" && Fold("ÉCAILLE") == "ecaille", "Capital accents");
        Check(Fold("cœur") == "coeur", "The oe ligature");
        Check(Fold("Île") == "ile" && Fold("Çà") == "ca" && Fold("Où") == "ou", "Other accents");
        Check(Matches("Pain des Champs", "pain"), "One word");
        Check(Matches("Pain des Champs", "champs pain"), "Words in any order");
        Check(Matches("Bois de Frêne", "frene"), "Without its accent");
        Check(Matches("Blé", "BLÉ"), "An accent typed still matches");
        Check(!Matches("Pain des Champs", "pain ble"), "Every word must appear");
        Check(!Matches("Pain", ""), "Nothing typed matches nothing");
        Check(!Matches("Pain", "   "), "Spaces match nothing");
        Check(Matches("Pain des Champs", "  pain  "), "Spaces around are ignored");
        Check(!Searchable("") && !Searchable("a") && !Searchable(" a ") && Searchable("bl") && Searchable("b l"), "A search starts at two letters");
        std::cout << "PASS: item search folds case and accents, needs every word, starts at two letters\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
