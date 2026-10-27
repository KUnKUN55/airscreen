#include "mic.h"
#include "settings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <initguid.h> // defines PKEY_Device_FriendlyName here rather than needing propsys
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace airscreen {
namespace {

std::string hr_text(const char *what, HRESULT hr) {
    char buf[96];
    if (hr == E_ACCESSDENIED) {
        snprintf(buf, sizeof(buf), "%s: access denied (Windows privacy settings block the microphone)", what);
    } else if (hr == (HRESULT) 0x88890004 /* AUDCLNT_E_DEVICE_INVALIDATED */ || hr == E_NOTFOUND) {
        snprintf(buf, sizeof(buf), "%s: the device is unplugged or disabled", what);
    } else {
        snprintf(buf, sizeof(buf), "%s failed (0x%08lx)", what, (unsigned long) hr);
    }
    return buf;
}

std::wstring friendly_name(IMMDevice *dev) {
    ComPtr<IPropertyStore> props;
    std::wstring name;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
            name = v.pwszVal;
        }
        PropVariantClear(&v);
    }
    return name;
}

struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    }
};

} // namespace

std::vector<MicCapture::Device> MicCapture::list_devices() {
    std::vector<Device> out;
    std::thread([&out] {
        // Own thread so the caller's COM apartment doesn't matter.
        ComScope com;
        ComPtr<IMMDeviceEnumerator> en;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
            return;
        }
        ComPtr<IMMDeviceCollection> coll;
        if (FAILED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll))) {
            return;
        }
        UINT n = 0;
        coll->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            ComPtr<IMMDevice> dev;
            LPWSTR id = nullptr;
            if (FAILED(coll->Item(i, &dev)) || FAILED(dev->GetId(&id))) {
                continue;
            }
            out.push_back({id, friendly_name(dev.Get())});
            CoTaskMemFree(id);
        }
    }).join();
    return out;
}

bool MicCapture::start(const std::wstring &device_id, Sink sink) {
    stop();
    sink_ = std::move(sink);
    error_.clear();
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool ok = false;
    run_ = true;
    thread_ = std::thread(&MicCapture::loop, this, device_id, (void *) ready, &ok);
    WaitForSingleObject(ready, INFINITE); // the thread writes `ok` before signalling, so wait it out
    CloseHandle(ready);
    if (!ok) {
        stop();
        return false;
    }
    return true;
}

void MicCapture::stop() {
    run_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
}

void MicCapture::loop(std::wstring device_id, void *ready_event, bool *ok) {
    ComScope com;
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (SUCCEEDED(hr)) {
        hr = device_id.empty() ? en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev)
                               : en->GetDevice(device_id.c_str(), &dev);
        if (FAILED(hr)) {
            error_ = hr_text("open microphone", hr);
        }
    } else {
        error_ = hr_text("audio devices", hr);
    }
    if (SUCCEEDED(hr)) {
        name_ = friendly_name(dev.Get());
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **) client.GetAddressOf());
        if (FAILED(hr)) {
            error_ = hr_text("activate microphone", hr);
        }
    }
    if (SUCCEEDED(hr)) {
        WAVEFORMATEX wf = {};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = kChannels;
        wf.nSamplesPerSec = kRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = (WORD) (kChannels * 4);
        wf.nAvgBytesPerSec = kRate * wf.nBlockAlign;
        // Let Windows resample and up/down-mix: mics are often mono, 44.1 kHz, or 16-bit.
        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000 /* 20 ms */, 0, &wf, nullptr);
        if (SUCCEEDED(hr)) {
            hr = client->SetEventHandle(ev);
        }
        if (SUCCEEDED(hr)) {
            hr = client->GetService(IID_PPV_ARGS(&capture));
        }
        if (SUCCEEDED(hr)) {
            hr = client->Start();
        }
        if (FAILED(hr)) {
            error_ = hr_text("start microphone", hr);
        }
    }
    *ok = SUCCEEDED(hr);
    SetEvent((HANDLE) ready_event); // `ok` and `ready_event` belong to start() and are not touched after this
    if (FAILED(hr)) {
        CloseHandle(ev);
        append_log(("mic: " + error_).c_str());
        return;
    }
    {
        char buf[300];
        WideCharToMultiByte(CP_UTF8, 0, name_.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
        append_log((std::string("mic: capturing from ") + buf).c_str());
    }

    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &task);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    std::vector<float> silence;
    while (run_) {
        WaitForSingleObject(ev, 50);
        UINT32 packet = 0;
        while (run_ && SUCCEEDED(hr = capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE *data = nullptr;
            UINT32 frames = 0;
            DWORD bflags = 0;
            UINT64 qpc_100ns = 0;
            hr = capture->GetBuffer(&data, &frames, &bflags, nullptr, &qpc_100ns);
            if (FAILED(hr)) {
                break;
            }
            int64_t at_us;
            if (qpc_100ns && !(bflags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)) {
                at_us = (int64_t) (qpc_100ns / 10);
            } else {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                at_us = (int64_t) (now.QuadPart / freq.QuadPart * 1000000 +
                                   now.QuadPart % freq.QuadPart * 1000000 / freq.QuadPart);
            }
            const float *pcm = (const float *) data;
            if (bflags & AUDCLNT_BUFFERFLAGS_SILENT) {
                silence.assign((size_t) frames * kChannels, 0.0f);
                pcm = silence.data();
            }
            if (frames && sink_) {
                sink_(pcm, (int) frames, at_us);
            }
            capture->ReleaseBuffer(frames);
        }
        if (FAILED(hr)) {
            error_ = hr_text("microphone", hr);
            append_log(("mic: stopped, " + error_).c_str());
            break;
        }
    }
    client->Stop();
    if (mmcss) {
        AvRevertMmThreadCharacteristics(mmcss);
    }
    CloseHandle(ev);
    run_ = false;
}

} // namespace airscreen
