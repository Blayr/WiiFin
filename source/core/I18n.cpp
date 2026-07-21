#include "I18n.h"
#include <ogc/conf.h>

namespace I18n {

static Lang currentLang = Lang::English;

void init() {
    currentLang = (CONF_GetLanguage() == CONF_LANG_FRENCH) ? Lang::French : Lang::English;
}

const char* t(Key key) {
    switch (key) {
        case Key::NoneInProgress:
            return currentLang == Lang::French ? "Rien en cours" : "Nothing in progress";
        case Key::NoFavorites:
            return currentLang == Lang::French ? "Pas de favoris" : "No favorites";
        case Key::NoMovieInProgress:
            return currentLang == Lang::French ? "Aucun film en cours" : "No movie in progress";
        case Key::Play:
            return currentLang == Lang::French ? "\xe2\x96\xb6 Lire" : "\xe2\x96\xb6 Play";
    }
    return "";
}

} // namespace I18n
