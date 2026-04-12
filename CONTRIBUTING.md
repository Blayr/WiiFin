# 🤝 Contributing to WiiFin

Thank you for your interest in the project!  
**WiiFin** is an experimental Jellyfin client for the Nintendo Wii.  
Any help is welcome — but please note that the project is still under active development and some features may be incomplete or unstable on real hardware.

---

## 🚧 Project Status

- ✅ Authentication (username/password and QuickConnect)
- ✅ Library browsing (movies, TV shows, music) with cover art
- ✅ Video and music playback via MPlayer CE (server-side transcoding)
- ✅ Player overlay (seek, volume, next/prev, audio/subtitle tracks, intro skip)
- ✅ Playback reporting to the Jellyfin server
- ✅ HTTPS/TLS via mbedTLS (self-signed certificates supported)
- ✅ Wiimote IR pointer and virtual on-screen keyboard
- 🔄 Server discovery (not yet implemented)

---

## 🧰 Requirements

- A properly configured Wii development environment (devkitPro, devkitPPC, libogc, GRRLIB, etc.)
- A build system that supports `make` (Linux or MSYS2 recommended)
- Dolphin Emulator for quick testing
- A real Wii with the Homebrew Channel for final testing

---

## 📁 Project Structure

- `source/core/` – App lifecycle, background music, sound effects, utilities
- `source/input/` – Wiimote and USB keyboard input
- `source/jellyfin/` – Jellyfin HTTP API client (HTTPS via mbedTLS)
- `source/player/` – MPlayer CE integration and player overlay HUD
- `source/ui/` – All views: Connect, Library, Profile, Settings, MusicPlayer
- `data/` – Graphical assets (PNG, TTF, sounds)
- `libs/` – Bundled mbedTLS
- `tools/` – WAD packager, banner generator
- `apps/WiiFin/` – Homebrew Channel metadata
- `Makefile` – devkitPro-compatible build script

---

## 🧪 How to Contribute

1. **Fork** the repository and create a new branch.
2. **Make clear and atomic commits.**
3. **Test your code in Dolphin and/or on a real Wii if possible.**
4. **Open a Pull Request** to the `main` branch.

---

## 🧭 Guidelines

- Use clear, descriptive variable names.
- Follow existing code style and indentation.
- Keep Wii limitations in mind (low RAM, 640x480 resolution, etc.).

---

## 📝 License

This project is licensed under **GPLv3**.  
By contributing, you agree that your work will be released under this license.

---

Thanks 🙌
