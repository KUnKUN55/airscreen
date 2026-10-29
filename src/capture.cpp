#include "capture.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <unknwn.h>
#include <inspectable.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <algorithm>
#include <cstdio>
#include <mutex>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "dwmapi.lib")

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

namespace airscreen {
namespace {

std::string hr_text(HRESULT hr) {
    char b[48];
    snprintf(b, sizeof(b), "0x%08lX", (unsigned long) hr);
    return b;
}

std::string narrow(const winrt::hstring &s) {
    return winrt::to_string(s);
}

BOOL CALLBACK monitor_proc(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    auto *out = (std::vector<CaptureTarget> *) lp;
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) {
        return TRUE;
    }
    CaptureTarget t;
    t.kind = CaptureTarget::Display;
    t.monitor = mon;
    t.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    // "\\.\DISPLAY2" is display 2 in Windows' own numbering (Settings > Display > Identify).
    const wchar_t *digits = mi.szDevice + wcslen(mi.szDevice);
    while (digits > mi.szDevice && iswdigit(digits[-1])) {
        --digits;
    }
    t.number = _wtoi(digits);
    if (t.number <= 0) {
        t.number = (int) out->size() + 1;
    }
    t.x = mi.rcMonitor.left;
    t.y = mi.rcMonitor.top;
    t.w = mi.rcMonitor.right - mi.rcMonitor.left;
    t.h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    wchar_t name[96];
    swprintf_s(name, L"Display %d  ·  %d × %d%s", t.number, t.w, t.h, t.primary ? L"  (main)" : L"");
    t.name = name;
    out->push_back(t);
    return TRUE;
}

BOOL CALLBACK window_proc(HWND h, LPARAM lp) {
    auto *out = (std::vector<CaptureTarget> *) lp;
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) {
        return TRUE;
    }
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) {
        return TRUE;
    }
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
        return TRUE; // on another virtual desktop, or a suspended store app
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) {
        return TRUE;
    }
    wchar_t title[256] = {};
    if (GetWindowTextW(h, title, 256) <= 0) {
        return TRUE;
    }
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"Shell_TrayWnd")) {
        return TRUE;
    }
    RECT rc = {};
    GetWindowRect(h, &rc);
    CaptureTarget t;
    t.kind = CaptureTarget::Window;
    t.window = h;
    t.name = title;
    t.minimised = IsIconic(h) != FALSE;
    t.x = rc.left;
    t.y = rc.top;
    t.w = rc.right - rc.left;
    t.h = rc.bottom - rc.top;
    if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH] = {};
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(proc, 0, path, &n)) {
            const wchar_t *slash = wcsrchr(path, L'\\');
            t.detail = slash ? slash + 1 : path;
        }
        CloseHandle(proc);
    }
    out->push_back(t);
    return TRUE;
}

} // namespace

std::vector<CaptureTarget> list_displays() {
    std::vector<CaptureTarget> out;
    EnumDisplayMonitors(nullptr, nullptr, monitor_proc, (LPARAM) &out);
    std::sort(out.begin(), out.end(), [](const CaptureTarget &a, const CaptureTarget &b) { return a.number < b.number; });
    return out;
}

std::vector<CaptureTarget> list_windows() {
    std::vector<CaptureTarget> out;
    EnumWindows(window_proc, (LPARAM) &out);
    return out;
}

struct ScreenCapture::Impl {
    std::mutex mu; // held while a frame is handed over, and by stop() to end that
    bool alive = true;
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device{nullptr};
    winrt::event_token frame_token{};
    winrt::event_token closed_token{};
    winrt::Windows::Graphics::SizeInt32 size{};
    FrameFn on_frame;
    ClosedFn on_closed;
};

ScreenCapture::ScreenCapture() = default;

ScreenCapture::~ScreenCapture() {
    stop();
}

bool ScreenCapture::supported() {
    try {
        return wgc::GraphicsCaptureSession::IsSupported();
    } catch (...) {
        return false;
    }
}

bool ScreenCapture::start(ID3D11Device *device, const CaptureTarget &target, bool cursor, FrameFn on_frame,
                          ClosedFn on_closed, std::string *err) {
    stop();
    auto fail = [&](const std::string &msg) {
        if (err) {
            *err = msg;
        }
        return false;
    };
    if (!device) {
        return fail("no graphics device");
    }
    if (!supported()) {
        return fail("screen capture needs Windows 10 version 1903 or later");
    }
    auto impl = std::make_shared<Impl>();
    impl->on_frame = std::move(on_frame);
    impl->on_closed = std::move(on_closed);
    try {
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        HRESULT hr;
        if (target.kind == CaptureTarget::Window) {
            if (!IsWindow((HWND) target.window)) {
                return fail("that window has been closed");
            }
            hr = interop->CreateForWindow((HWND) target.window, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                          winrt::put_abi(impl->item));
        } else {
            hr = interop->CreateForMonitor((HMONITOR) target.monitor, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                           winrt::put_abi(impl->item));
        }
        if (FAILED(hr) || !impl->item) {
            return fail("Windows would not share it (" + hr_text(hr) + ")");
        }
        winrt::com_ptr<IDXGIDevice> dxgi;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(dxgi.put())))) {
            return fail("no DXGI device");
        }
        winrt::com_ptr<::IInspectable> insp;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put());
        if (FAILED(hr)) {
            return fail("could not share the graphics device (" + hr_text(hr) + ")");
        }
        impl->device = insp.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
        impl->size = impl->item.Size();
        impl->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            impl->device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, impl->size);
        impl->session = impl->pool.CreateCaptureSession(impl->item);
        try {
            impl->session.IsCursorCaptureEnabled(cursor); // Windows 10 2004 and later
        } catch (...) {
        }

        std::weak_ptr<Impl> weak = impl;
        impl->frame_token = impl->pool.FrameArrived([weak](wgc::Direct3D11CaptureFramePool const &sender,
                                                           winrt::Windows::Foundation::IInspectable const &) {
            auto self = weak.lock();
            if (!self) {
                return;
            }
            try {
                auto frame = sender.TryGetNextFrame();
                if (!frame) {
                    return;
                }
                auto content = frame.ContentSize();
                auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                winrt::com_ptr<ID3D11Texture2D> tex;
                if (FAILED(access->GetInterface(IID_PPV_ARGS(tex.put())))) {
                    return;
                }
                std::lock_guard<std::mutex> lock(self->mu);
                if (!self->alive) {
                    return;
                }
                if (content.Width != self->size.Width || content.Height != self->size.Height) {
                    // The window was resized: new buffers at the new size, from the next frame on.
                    self->size = content;
                    sender.Recreate(self->device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, content);
                }
                D3D11_TEXTURE2D_DESC td = {};
                tex->GetDesc(&td);
                int w = (std::min)((int) td.Width, (int) content.Width) & ~1;
                int h = (std::min)((int) td.Height, (int) content.Height) & ~1;
                if (w > 0 && h > 0 && self->on_frame) {
                    self->on_frame(tex.get(), w, h);
                }
            } catch (...) {
                // A frame that can't be read is skipped; the next one usually can.
            }
        });
        impl->closed_token = impl->item.Closed([weak](wgc::GraphicsCaptureItem const &,
                                                      winrt::Windows::Foundation::IInspectable const &) {
            if (auto self = weak.lock()) {
                ClosedFn fn;
                {
                    std::lock_guard<std::mutex> lock(self->mu);
                    if (!self->alive) {
                        return;
                    }
                    fn = self->on_closed;
                }
                if (fn) {
                    fn();
                }
            }
        });
        impl->session.StartCapture();
    } catch (const winrt::hresult_error &e) {
        return fail("capture failed: " + narrow(e.message()) + " (" + hr_text(e.code()) + ")");
    } catch (...) {
        return fail("capture failed");
    }
    impl_ = impl;
    return true;
}

void ScreenCapture::stop() {
    auto impl = std::move(impl_);
    if (!impl) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(impl->mu); // waits out a frame being handed over
        impl->alive = false;
    }
    try {
        if (impl->pool) {
            impl->pool.FrameArrived(impl->frame_token);
        }
        if (impl->item) {
            impl->item.Closed(impl->closed_token);
        }
        if (impl->session) {
            impl->session.Close();
        }
        if (impl->pool) {
            impl->pool.Close();
        }
    } catch (...) {
    }
}

} // namespace airscreen
