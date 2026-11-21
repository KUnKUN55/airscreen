#pragma once

#include <windows.h>
#include <string>

namespace airscreen {

// Where releases are published; the update check asks GitHub for the newest one here.
constexpr const wchar_t *kReleasesUrl = L"https://github.com/KUnKUN55/airscreen/releases";

// "1.3.0" (or "v1.3.0") newer than "1.2.2"? Missing parts count as 0; anything after the numbers is ignored.
bool version_newer(const std::string &candidate, const std::string &current);

// Asks GitHub for the latest release on a background thread. If it is newer than `current`, posts `msg` to
// `hwnd` with lParam a heap std::wstring* (the receiver deletes it) holding "<version>\n<release page url>".
// Nothing is sent but a plain HTTPS request with a generic user agent; failures are only logged.
void check_for_update_async(HWND hwnd, UINT msg, const std::string &current);

} // namespace airscreen
