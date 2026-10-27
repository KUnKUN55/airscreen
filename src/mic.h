#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace airscreen {

// Captures a microphone (or any recording device) through WASAPI shared mode. Windows converts whatever the
// device delivers to 48 kHz stereo float, so callers get one format regardless of the hardware.
class MicCapture {
public:
    static constexpr int kRate = 48000;
    static constexpr int kChannels = 2;

    // Interleaved stereo float at kRate, plus the local QPC time (microseconds) of the first frame.
    using Sink = std::function<void(const float *pcm, int frames, int64_t local_us)>;

    struct Device {
        std::wstring id;
        std::wstring name;
    };
    static std::vector<Device> list_devices();

    MicCapture() = default;
    ~MicCapture() { stop(); }

    // `device_id` empty = the Windows default recording device. Returns false (with error()) if the device
    // can't be opened; the capture thread then isn't running.
    bool start(const std::wstring &device_id, Sink sink);
    void stop();
    bool running() const { return run_.load(); }
    const std::string &error() const { return error_; }
    std::wstring device_name() const { return name_; }

private:
    void loop(std::wstring device_id, void *ready_event, bool *ok);

    std::thread thread_;
    std::atomic<bool> run_{false};
    Sink sink_;
    std::string error_;
    std::wstring name_;
};

} // namespace airscreen
