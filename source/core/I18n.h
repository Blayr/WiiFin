#pragma once

// Minimal translation mechanism. English is the default for every system
// language except French (the original UI text was hardcoded in French —
// this preserves that experience for French-language consoles while
// defaulting everyone else to English).
//
// Add a new UI language: add an enum value to Lang, a branch in detect(),
// and a case in each Key's string in I18n.cpp.
namespace I18n {
    enum class Lang { English, French };

    // Reads the Wii's system language via CONF_GetLanguage() and caches the
    // result. Call once at startup before any t() calls.
    void init();

    enum class Key {
        NoneInProgress,     // "Nothing in progress" / "Rien en cours"
        NoFavorites,        // "No favorites" / "Pas de favoris"
        NoMovieInProgress,  // "No movie in progress" / "Aucun film en cours"
        Play,               // "Play" / "Lire"
    };

    const char* t(Key key);
}
