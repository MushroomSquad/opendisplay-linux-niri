# OpenDisplay for Linux

This target connects the existing, unmodified iOS/iPadOS receiver to a KDE or
Hyprland Wayland session. It discovers `_opensidecar._tcp` services with Avahi
or opens the receiver's port through usbmuxd, captures with PipeWire, and
streams Annex B H.264 produced by FFmpeg. The command-line client and an
initial Kirigami control application share the same connection engine.
This fork also supports niri in mirror mode; see [niri](#niri).

## Arch Linux dependencies

```sh
sudo pacman -S --needed base-devel cmake qt6-base qt6-declarative qt6-wayland \
  kirigami pipewire avahi libusbmuxd usbmuxd ffmpeg libkscreen wayland \
  xdg-desktop-portal xdg-desktop-portal-kde
```

For Hyprland, also install `hyprland xdg-desktop-portal-hyprland`. niri needs
no extra portal: OpenDisplay talks to niri's built-in ScreenCast service. The package
keeps the KDE dependencies so one binary can select either backend.

For hardware encoding, install the driver for the GPU: `libva-mesa-driver` for AMD,
`intel-media-driver` for modern Intel GPUs, or `nvidia-utils` for NVIDIA.
`libva-utils` is useful for checking VA-API with `vainfo`. The process must be
able to open `/dev/dri/renderD128`; normal Arch installations grant this through
the active desktop session.

USB requires the device to be unlocked and paired with the machine. Accept the
device's Trust prompt, then verify the usbmuxd path before starting OpenDisplay:

```sh
idevice_id -l
idevicepair pair       # first connection only
idevicepair validate
```

If `idevice_id` cannot see the device, reconnect the cable and inspect
`journalctl -u usbmuxd.service`; OpenDisplay cannot use a device that usbmuxd
has not finished adding. A journal `lockdown error -8` is a failed mux
preflight: unlock and disconnect the device, restart `usbmuxd.service`, then
reconnect and accept the Trust prompt. Start the service if socket activation
does not do so automatically. Wi-Fi discovery requires a running Avahi daemon;
installing the package does not enable system services:

```sh
sudo systemctl enable --now avahi-daemon.service
systemctl status avahi-daemon.service
```

## Build and test

```sh
cmake -S Linux -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux -j
ctest --test-dir build/linux --output-on-failure
```

## Build and install the Arch package

The `-git` package follows the pushed `main` branch of
[MushroomSquad/opendisplay-linux-niri](https://github.com/MushroomSquad/opendisplay-linux-niri). From the
repository root, use a clean package build so an older clone of `main` cannot
remain in makepkg's `src/` directory:

```sh
(cd Linux/packaging/arch && makepkg --cleanbuild --syncdeps --install)
```

The short equivalent is `makepkg -Csi`. Building the package consumes the
pushed branch, not uncommitted files in the current worktree. Push a commit to
`origin/main` before packaging when testing new local changes.

## Run

Open the existing OpenDisplay app on the iOS device, then run:

```sh
./build/linux/opendisplay-gui
./build/linux/opendisplay-linux --transport auto --mode extend
```

The GUI exposes the CLI defaults and common monitor-layout overrides. It uses
the desktop's StatusNotifier tray when available. On a compositor without a
compatible tray host it remains a normal launcher window, and closing the
window exits instead of leaving an invisible background process. On Hyprland,
a StatusNotifier-capable bar such as Waybar enables tray behavior.

The compositor backend is detected automatically; override it with
`--compositor kde`, `--compositor hyprland` or `--compositor niri`. The first run displays the
desktop's capture permission dialog. Other useful forms are:

```sh
./build/linux/opendisplay-linux --list
./build/linux/opendisplay-linux --transport usb --udid DEVICE_UDID
./build/linux/opendisplay-linux --transport wifi --host 192.168.1.40
./build/linux/opendisplay-linux --encoder vaapi --vaapi-device /dev/dri/renderD128
./build/linux/opendisplay-linux --encoder nvenc --mode mirror --no-input
./build/linux/opendisplay-linux --compositor hyprland --reference-monitor eDP-1
```

### Virtual monitor layout

In extend mode, OpenDisplay queries the selected compositor before opening its
portal. One enabled monitor is selected automatically; with two or more, pass
its connector name or current backend id explicitly:

```sh
opendisplay-linux --reference-monitor DP-1
opendisplay-linux --reference-monitor eDP-1 --extend-to right --align-to bottom
opendisplay-linux --extend-to bottom --align-to right
```

The default is bottom-right: extend right and align bottom. Left/right extension
accepts `top`, `bottom`, or `center` alignment; top/bottom extension accepts
`left`, `right`, or `center`. Positions use compositor logical coordinates.

The automatic mode starts with the iPad's native pixels. It uses physical DPI
when both panel sizes are available, otherwise the iOS-reported native scale;
the result is rounded to a 0.05 scale step. Pixel dimensions are nudged to the
nearest values that produce integer logical dimensions; widths are also made
divisible by eight for KDE's CVT-generated custom modes. Hyprland keeps native
pixel dimensions when they already produce an integer logical size. Detection
and any part of the calculation can be overridden:

```sh
opendisplay-linux --virtual-resolution 2420x1668 --display-scale 1.25
opendisplay-linux --virtual-refresh 60 --ipad-size-mm 263x181
opendisplay-linux --reference-resolution 3840x2160 --reference-scale 1.5 \
  --reference-size-mm 597x336
opendisplay-linux --reference-geometry 2560x1440+0+0
```

`--scale` remains the video encoder resolution multiplier and does not change
desktop display scaling. KDE custom virtual modes require Plasma/libkscreen
6.6 or newer. OpenDisplay uses libkscreen in-process so output identity and
detailed configuration errors remain reliable while KDE adds or renumbers
outputs.

On Hyprland, OpenDisplay creates a named headless output with `hyprctl`, applies
the same resolution, scaling, and placement calculation, and asks the
ScreenCast portal to capture a monitor. Select the displayed `OpenDisplay-PID`
monitor in the share picker. Touch input uses Hyprland's
`zwlr_virtual_pointer_manager_v1` support and does not require the unsupported
RemoteDesktop portal. Current Hyprland Lua monitor rules are used first, with
the pre-0.55 `keyword monitor` command as a compatibility fallback. The
headless output is removed when the session stops. Before XDPH opens its share
picker, OpenDisplay restores focus and cursor placement to the selected
reference monitor so the authorization prompt remains reachable. The reference
output is temporarily pinned to its detected geometry while the headless output
exists, preventing Hyprland `auto` rules from reversing their relative order;
the user's configuration is reloaded after teardown.

### niri

The niri backend (`--compositor niri`) supports only `--mode mirror`: niri has
no API for creating outputs at runtime, so the output that goes to the iPad must
already exist. Use a real connector (for example a dummy HDMI/DP plug or a
capture card), configure its mode, scale and position in niri's `config.kdl`,
and pick it with `--reference-monitor` when more than one output is enabled:

```kdl
output "DP-2" {
    mode custom=true "2360x1640@60"
    scale 1.5
}
```

OpenDisplay lists outputs with `niri msg --json outputs` and records the chosen
one through niri's `org.gnome.Mutter.ScreenCast` D-Bus service, so no portal
share picker appears. Touch input uses `zwlr_virtual_pointer_manager_v1`.

niri offers only DMA-BUF buffers with an explicit modifier; OpenDisplay requests
LINEAR buffers. With `--encoder vaapi` (or `auto` on a VA-API GPU) they are
imported straight into VA-API, scaled with `scale_vaapi` and encoded in-process,
avoiding slow CPU reads from VRAM. If the zero-copy path fails, the sender falls
back to mmap + the FFmpeg subprocess. `--verbose` logs per-second capture,
encode latency and bitrate. Measured on niri 26.04 / AMD Navi 14 to an iPad Air
over USB: up to 62 fps and 6–14 ms capture→encoded.

To start it with the session and reconnect automatically when the cable is
plugged back in, run it as a systemd user service bound to `niri.service`:

```ini
[Unit]
Description=OpenDisplay: stream a niri output to the iPad over USB
PartOf=niri.service
After=niri.service
StartLimitIntervalSec=0

[Service]
ExecStart=%h/.local/bin/opendisplay-linux --compositor niri --mode mirror --transport usb --encoder vaapi --reference-monitor DP-2
Restart=always
RestartSec=3

[Install]
WantedBy=niri.service
```

The process exits when no device is attached; `Restart=always` is what
reconnects it.

If Bonjour discovery is unavailable, confirm that the phone app is open and
inspect the advertised service with `avahi-browse -rt _opensidecar._tcp`.
You can bypass Avahi when the receiver's address is known:

```sh
opendisplay-linux --transport wifi --host 192.168.1.40 --port 9000
```

`--encoder auto` prefers VA-API, then NVENC, and falls back to `libx264`.
Use `Linux/tools/fake_receiver.py` to exercise the TCP framing without an iOS
device; it does not decode video or advertise Bonjour.

## Current MVP limits

KDE Plasma Wayland, Hyprland and niri (mirror only) are supported. Hyprland's portal picker cannot
yet be preselected by output name, so the requested monitor must be chosen in
the dialog. Audio,
clipboard, cursor sprites, encryption, reconnection, and multi-device sessions
remain out of scope. Compositor-neutral layout lives in `display_layout`, while
KScreen, Hyprland and niri output operations remain isolated in their respective
controllers. Later compositor backends need not change transport, protocol,
layout planning, or encoding code.
