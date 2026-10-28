<div align="center">

<img src="public/logo.png" width="128" alt="OpenDisplay app icon" />

# OpenDisplay Linux — niri fork

**Use an iPhone or iPad as an additional display on a Linux Wayland desktop.**

[Install the iOS/iPadOS receiver](https://peetzweg.github.io/opendisplay/) · [Linux guide](Linux/README.md) · [Linux fork](https://github.com/tixwho/opendisplay-linux) · [Upstream project](https://github.com/peetzweg/opendisplay)

</div>

This repository is a fork of
[tixwho/opendisplay-linux](https://github.com/tixwho/opendisplay-linux), which
is itself a fork of [peetzweg/opendisplay](https://github.com/peetzweg/opendisplay).
It adds a [niri](https://github.com/YaLTeR/niri) backend and a zero-copy VA-API
encode path on top of the Linux sender. See [Authors and credits](#authors-and-credits).

> [!WARNING]
> This is a highly experimental, vibe-coded personal fork. It is not expected
> to merge back into the upstream project, compatibility may break without
> notice, and maintenance will be limited. Use it only if you are comfortable
> diagnosing Linux, Wayland, PipeWire, portal, and hardware-encoding issues.

## Current scope

This fork adds a Linux sender while retaining the original OpenDisplay
iOS/iPadOS receiver and wire protocol. The Linux application supports:

- KDE Plasma Wayland through `xdg-desktop-portal-kde` and KScreen.
- Hyprland through `xdg-desktop-portal-hyprland` and headless outputs.
- niri (mirror mode) through niri's built-in Mutter ScreenCast D-Bus API, with
  zero-copy DMA-BUF → VA-API H.264 encoding (this fork).
- Wi-Fi discovery with Avahi and USB connectivity through `usbmuxd`.
- PipeWire capture and FFmpeg H.264 encoding, including supported hardware
  encoders.
- A command-line client and an experimental Qt Quick/Kirigami GUI.

Other compositors and non-Wayland sessions are currently unsupported. Portal
behavior also depends on compositor and desktop versions, so successful setup
on one system does not guarantee identical behavior elsewhere.

## Receiver application

No modified mobile application is required. Install and run the same upstream
OpenDisplay receiver on an iPhone or iPad from the
[OpenDisplay installation page](https://peetzweg.github.io/opendisplay/).
The receiver currently requires iOS/iPadOS 16 or newer. The public
[TestFlight beta](https://testflight.apple.com/join/3NYaY11c) remains available
when offered by the upstream project.

## Linux installation and usage

An Arch Linux `PKGBUILD` is provided under
[`Linux/packaging/arch/`](Linux/packaging/arch/PKGBUILD). Source builds and the
package include both `opendisplay-linux` and `opendisplay-gui`.

For dependencies, package-building commands, Wi-Fi and USB setup, monitor
placement and scaling options, KDE/Hyprland/niri notes, and troubleshooting, use the
dedicated [Linux README](Linux/README.md).

## Authors and credits

- **[peetzweg](https://github.com/peetzweg)** and the OpenDisplay contributors
  (including Patrick Alessi Krarup and Jamie Wong) — the original
  [OpenDisplay](https://github.com/peetzweg/opendisplay): macOS sender,
  iOS/iPadOS receiver, wire protocol and website.
- **[tixwho](https://github.com/tixwho)** — the
  [Linux port](https://github.com/tixwho/opendisplay-linux): Linux sender,
  KDE and Hyprland backends, Avahi/usbmuxd transport, PipeWire capture, FFmpeg
  encoding and the Kirigami GUI.
- **Dmitriy Osipenko** and **[DonSudak](https://github.com/DonSudak)**
  ([MushroomSquad](https://github.com/MushroomSquad)) — this fork: niri
  ScreenCast backend, LINEAR DMA-BUF capture, latency fixes and the zero-copy
  VA-API encoder.

Full authorship is preserved in the git history; this fork was branched from
`tixwho/opendisplay-linux` at `ebb2f26`.

## Upstream code and license

The original macOS sender, iOS/iPadOS receiver, website, and shared protocol
remain in this repository for compatibility and reference. Upstream development
belongs at [peetzweg/opendisplay](https://github.com/peetzweg/opendisplay), and
general Linux sender work at
[tixwho/opendisplay-linux](https://github.com/tixwho/opendisplay-linux).
This fork remains available under the repository's [GPL-3.0 license](LICENSE).
