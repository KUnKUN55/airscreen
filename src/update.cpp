/* Update check: one HTTPS GET of the GitHub "latest release" endpoint on a worker thread. It never downloads
 * or runs anything; it only tells the window that a newer version exists and where its release page is. */

#include "update.h"
#include "settings.h"

#include <winhttp.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace airscreen {
namespace {

std::vector<int> parse_version(const std::string &v) {
    std::vector<int> out;
    size_t i = 0;
    while (i < v.size() && !isdigit((unsigned char) v[i])) {
        ++i; // "v1.3.0"
    }
    while (i < v.size() && isdigit((unsigned char) v[i])) {
        int n = 0;
        while (i < v.size() && isdigit((unsigned char) v[i])) {
            n = n * 10 + (v[i] - '0');
            if (n > 100000) {
                n = 100000;
            }
            ++i;
        }
        out.push_back(n);
        if (i < v.size() && v[i] == '.') {
            ++i;
        } else {
            break;
        }
    }
    return out;
}

// The string value of `"key": "..."` in a small, well-formed JSON object (GitHub's release record).
std::string json_string(const std::string &body, const char *key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = body.find(k);
    if (p == std::string::npos) {
        return {};
    }
    p = body.find(':', p + k.size());
    if (p == std::string::npos) {
        return {};
    }
    p = body.find('"', p + 1);
    if (p == std::string::npos) {
        return {};
    }
    std::string out;
    for (size_t i = p + 1; i < body.size() && out.size() < 512; ++i) {
        char c = body[i];
        if (c == '\\' && i + 1 < body.size()) {
            out += body[++i];
        } else if (c == '"') {
            return out;
        } else {
            out += c;
        }
    }
    return {};
}

std::wstring widen(const std::string &s) {
    if (s.empty()) {
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
    std::wstring w((size_t) n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), w.data(), n);
    return w;
}

bool fetch_latest(const std::string &current, std::string &body) {
    std::wstring agent = L"AirScreen/" + widen(current);
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        // Windows before 8.1 has no automatic proxy type.
        session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session) {
        return false;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 15000);
    bool ok = false;
    HINTERNET conn = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", L"/repos/KUnKUN55/airscreen/releases/latest", nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                         : nullptr;
    if (req && WinHttpSendRequest(req, L"Accept: application/vnd.github+json\r\n", (DWORD) -1L,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &len, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0 || body.size() > (1u << 20)) {
                    break;
                }
                std::string chunk(avail, '\0');
                DWORD got = 0;
                if (!WinHttpReadData(req, chunk.data(), avail, &got) || got == 0) {
                    break;
                }
                body.append(chunk.data(), got);
            }
            ok = !body.empty();
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "update check: GitHub answered %lu", (unsigned long) status);
            append_log(msg);
        }
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "update check: no connection (error %lu)", (unsigned long) GetLastError());
        append_log(msg);
    }
    if (req) {
        WinHttpCloseHandle(req);
    }
    if (conn) {
        WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    return ok;
}

} // namespace

bool version_newer(const std::string &candidate, const std::string &current) {
    std::vector<int> a = parse_version(candidate), b = parse_version(current);
    if (a.empty()) {
        return false;
    }
    size_t n = (std::max)(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) {
            return x > y;
        }
    }
    return false;
}

void check_for_update_async(HWND hwnd, UINT msg, const std::string &current) {
    try {
        std::thread([hwnd, msg, current] {
            std::string body;
            if (!fetch_latest(current, body)) {
                return;
            }
            std::string tag = json_string(body, "tag_name");
            std::string url = json_string(body, "html_url");
            // Only ever point at this project's own release pages.
            const std::string prefix = "https://github.com/KUnKUN55/airscreen/releases/";
            if (url.compare(0, prefix.size(), prefix) != 0) {
                url = "https://github.com/KUnKUN55/airscreen/releases/latest";
            }
            if (tag.empty() || !version_newer(tag, current)) {
                return;
            }
            std::string v = tag[0] == 'v' || tag[0] == 'V' ? tag.substr(1) : tag;
            append_log(("update check: " + v + " is available").c_str());
            auto *payload = new std::wstring(widen(v) + L"\n" + widen(url));
            if (!IsWindow(hwnd) || !PostMessageW(hwnd, msg, 0, (LPARAM) payload)) {
                delete payload;
            }
        }).detach();
    } catch (...) {
        append_log("update check: could not start a thread");
    }
}

} // namespace airscreen
