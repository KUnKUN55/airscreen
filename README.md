<p align="center">
  <img src="src/app/logo.png" width="96" alt="AirScreen logo">
</p>

<h1 align="center">AirScreen: Free AirPlay Receiver for Windows</h1>

<p align="center">
  Mirror your iPhone to a Windows PC over AirPlay. No Apple TV, no cables, no app to install on the iPhone. Free and open source.
</p>

<p align="center">
<a href="https://diestro.dev/tools/airscreen/"><strong>Setup guide and download: diestro.dev/tools/airscreen</strong></a>
</p>

<p align="center">
  <a href="https://buymeacoffee.com/donerfolk"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" height="40" alt="Buy Me A Coffee"></a>
</p>

---

AirScreen turns a Windows PC into a wireless AirPlay receiver. Your iPhone lists it under **Control Center → Screen Mirroring**, just like an Apple TV, without needing one. Video and audio play in a resizable window or full screen, with no app to install on the iPhone.

## Features

- Shows up as a Screen Mirroring target, no app needed on the iPhone
- Hardware-accelerated H.264 decoding (Direct3D 11)
- Audio plays through your default Windows output device
- Rename the receiver, require a pairing PIN, keep the window on top, fill the screen or letterbox
- Light and dark idle screen
- Closing the window quits AirScreen; settings are also in the tray icon menu
- Optional Start with Windows: waits in the system tray, ready to mirror to

## Requirements

- Windows 10 or 11, 64-bit
- iOS 9.3 or later. Earlier versions speak an older AirPlay protocol that AirScreen doesn't implement, so they won't see the PC at all.
- iPhone and PC on the same network. Guest networks and routers with client isolation will hide the PC.

AirScreen is tested with iPhone. iPad and Mac use the same protocol and may work, but are untested.

## Install

Download the latest installer from [Releases](https://github.com/KUnKUN55/airscreen/releases), or [build from source](#building-from-source).

Every release is built from this repository by [GitHub Actions](https://github.com/KUnKUN55/airscreen/actions), with SHA-256 checksums in `SHA256SUMS.txt`.

The installer isn't code-signed yet, so Windows SmartScreen may show "Windows protected your PC". Click **More info → Run anyway**.

## Usage

1. Start AirScreen. If Windows Firewall asks, allow access on private networks.
2. On your iPhone, open **Control Center → Screen Mirroring** and pick **AirScreen**.
3. If a PIN appears on the PC, enter it on the iPhone.

To stop, end mirroring on the iPhone. Close the window to quit AirScreen. Right-click the window or the tray icon for settings.

## Teaching studio

For online classes and tutoring, AirScreen can compose what students see from the iPad, this PC's screen, text and logos, and switch between layouts while you teach. Share the AirScreen window in Zoom, Teams or Meet (or record it), and control it from a small panel that stays out of the way.

- **Teaching controls** (Ctrl+T, or right-click → Teaching controls): what's on air, the scenes, quick buttons (iPad / Computer / Both / Text by default) and recording with pause. Closing the panel never stops sharing or recording. It can stay on top, shrink to a compact view, and is kept out of screen captures, so sharing a whole display doesn't show your controls.
- **Computer screen**: press **Computer…** to share a whole display or a single app window. A preview is shown first, and you're warned when you pick a whole display. Windows outlines whatever is being captured. If the window closes, AirScreen says so and carries on.
- **Scenes**: six samples to start from (iPad Teaching, Computer Presentation, Dual Screen, Title & Teaching, Minimal, Break). **Edit scenes** opens the editor: add iPad or computer screens, text, pictures and frames; drag and resize them on the live preview with snapping guides; set opacity, order and lock per layer. Text supports Thai and English, wraps and shrinks to fit, with a background that can be solid, outline, translucent, frosted (blurs only what's behind the box), or a gradient.
- **Preview**: see a scene before putting it on air, the output itself, or one screen as it arrives. Edits to the scene on air show at once, or wait for **Apply** if you prefer.
- **Profiles**: keep a set of scenes and saved text per subject (Teaching settings → Profiles). Everything is saved automatically to `%APPDATA%\AirScreen\profiles`.
- **Shortcuts**: Ctrl+Alt with a digit or letter (scenes, iPad, Computer, Both, text, panel, record). They work in AirScreen's windows, and in every app if you turn that on in Teaching settings. Start with `--teach` to open the controls straight away.

Recording uses the same composed picture students see, so scene changes go into one continuous file, and pauses leave no gap.

## Troubleshooting

**AirScreen doesn't appear on the iPhone**
- Make sure both devices are on the same Wi-Fi network (not a guest network).
- Turn off any VPN on the PC or the iPhone.
- Right-click the tray icon → **Allow through firewall**.

**Stuttering or delay.** Use 5 GHz Wi-Fi or connect the PC by Ethernet. Other devices streaming on the same network can add delay.

**No sound.** Check the default playback device in Windows sound settings. Apps that take exclusive control of the device can block AirScreen.

**Black window after connecting.** Stop mirroring and connect again. If it keeps happening, right-click the tray icon → **Open log file** and include the log in a [bug report](https://github.com/KUnKUN55/airscreen/issues).

Settings, pairing keys and the log live in `%APPDATA%\AirScreen`.

## Building from source

Requires Visual Studio 2022 with the *Desktop development with C++* workload.

```powershell
winget install Kitware.CMake
winget install ShiningLight.OpenSSL.Dev
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
```

`build.ps1` downloads FFmpeg into `third_party\prebuilt`, configures CMake and builds `build\Release\AirScreen.exe` with the required DLLs next to it. OpenSSL is found through `OPENSSL_ROOT_DIR`, the Shining Light install location, or vcpkg.

To build an installer, run [Inno Setup](https://jrsoftware.org/isinfo.php)'s `iscc` on `scripts\airscreen.iss` after a Release build. Pass `/DCrtDir=` pointing at the `x64\Microsoft.VC143.CRT` folder of your Visual Studio redistributables so the C++ runtime ships with the app.

### Project layout

| Path | Contents |
|---|---|
| `src/receiver.cpp` | AirPlay session handling on top of UxPlay |
| `src/video.cpp` | FFmpeg decoding and Direct3D 11 presentation |
| `src/audio.cpp` | AAC/ALAC decoding and WASAPI playback |
| `src/main.cpp`, `src/ui.cpp` | Window, tray menu and idle screen |
| `src/scene.cpp`, `src/studio.cpp` | Teaching scenes and profiles; the GPU compositor that draws them |
| `src/capture.cpp` | Computer screen capture (Windows.Graphics.Capture) |
| `src/control.cpp`, `src/editor.cpp`, `src/dialogs.cpp` | Teaching control panel, scene editor and preview, settings |
| `src/settings.cpp` | Settings file, firewall rule, log |
| `third_party/uxplay` | AirPlay protocol stack (RAOP, mirroring, mDNS) |
| `third_party/libplist` | Property list parsing |

## FAQ

**Is AirScreen free?** Yes, completely free and open source (GPL-3.0).

**Do I need an Apple TV?** No. AirScreen replaces an Apple TV for screen mirroring to your Windows PC.

**Do I need to install anything on my iPhone?** No. Use the built-in Screen Mirroring in Control Center. AirScreen shows up automatically on the same Wi-Fi network.

**Does it work with iPad or Mac?** iPad and Mac use the same AirPlay protocol and generally work, though they are untested.

**Does it work wirelessly?** Yes, entirely over Wi-Fi. Both devices must be on the same network (not a guest network).

## Credits

AirScreen's protocol support comes from [UxPlay](https://github.com/FDH2/UxPlay) and the projects it builds on (RPiPlay, shairplay, playfair). It also uses [FFmpeg](https://ffmpeg.org/), [OpenSSL](https://www.openssl.org/), [libplist](https://github.com/libimobiledevice/libplist) and llhttp.

AirScreen is not affiliated with Apple. AirPlay and iPhone are trademarks of Apple Inc.

## License

[GPL-3.0](LICENSE). Third-party licenses are listed in [NOTICE](NOTICE).
