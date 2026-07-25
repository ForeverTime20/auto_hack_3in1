#pragma once

#include <string>

namespace gta5::app::l10n {

enum class Language {
  Chinese = 0,
  English = 1,
};

void SetLanguage(Language language);
Language CurrentLanguage();
std::wstring Text(const char* key);

}  // namespace gta5::app::l10n
