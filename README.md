Nix flake providing **kwin-input-bridge**, a Linux tool that replays [Sunshine](https://github.com/LizardByte/Sunshine)'s uinput devices into a headless `kwin_wayland --virtual` session through KWin's EIS RemoteDesktop interface ([libei](https://gitlab.freedesktop.org/libinput/libei)). The source is first-party C++ carried in this flake and built against nixpkgs; there is no upstream project.

## Outputs

- `packages.<system>.kwin-input-bridge` — the package, installing the `kwin-input-bridge` binary at `bin/kwin-input-bridge`.
- `packages.<system>.default` — the same package.

Packages are the flake's only outputs, defined for `x86_64-linux` and `aarch64-linux`, which is also what the package's `meta.platforms` declares.

Consume the package from a flake input:

```nix
kwin-input-bridge.packages.${system}.kwin-input-bridge
```

Or build it directly:

```bash
nix build "github:jgus-org/kwin-input-bridge-flake#default"
```

## What it is for

A `kwin_wayland --virtual` session brings up no libinput seat, so evdev devices have no route into it; EIS is the only input channel that backend honours. The bridge reads the evdev devices Sunshine creates and writes the same input into the session as an EIS sender named `sunshine-input-bridge`.

## Running it

Forwarding, long-running:

```
kwin-input-bridge [--swallow-key CODE]...
```

`CODE` is an evdev key code from `linux/input-event-codes.h` (`29` = `KEY_LEFTCTRL`); `--swallow-key` may be repeated. Listed keys are read from the device but not forwarded — the point is a chord key that a separate `--tap` invocation consumes, so the desktop does not also see it. The process waits for KWin's EIS interface, then runs until it receives `SIGTERM` or `SIGINT`, reconnecting whenever the EIS session ends.

Chord injection, one-shot:

```
kwin-input-bridge --tap CODE[+CODE...]
```

Connects, presses the codes in order, releases them in reverse, disconnects. Exit 2 on an unparseable chord spec, 1 when no EIS session can be established. `--tap` takes exactly one argument and no other options.

## Input sources

The forwarder scans `/dev/input/event*` and adopts the newest device whose evdev name is exactly one of:

| Device name                    | Forwarded as                              |
| ------------------------------ | ----------------------------------------- |
| `Keyboard passthrough`         | keyboard keys                             |
| `Mouse passthrough`            | relative pointer: motion, buttons, scroll |
| `Mouse passthrough (absolute)` | absolute pointer position                 |

Those are the names Sunshine's virtual devices publish. Nothing is configurable: a device whose name differs is not forwarded, and a device whose name matches is forwarded no matter which program created it. Devices that cannot be opened are skipped without a message. The scan repeats every 2 seconds, so the devices Sunshine recreates for a new client are picked up without a restart, and a device from an older connection is dropped as soon as a newer one of the same role appears.

## Runtime requirements

Linux only: the tool needs evdev (`/dev/input`), `epoll`, `mlockall`, and a systemd user session. No macOS package is published.

It talks to KWin over the **user** session bus, so it must run as the user that owns the KDE session and with that session's `$DBUS_SESSION_BUS_ADDRESS`. A KWin that does not expose `org.kde.KWin.EIS.RemoteDesktop` at `/org/kde/KWin/EIS/RemoteDesktop` — an older KWin, a different compositor, or the wrong bus — is not an error: the bridge logs `waiting for KWin EIS: …` once a second and keeps trying.

KWin must run with `KWIN_WAYLAND_NO_PERMISSION_CHECKS=1` in its own environment. Without it KWin gates `connectToEIS` behind its input-takeover consent check, which a headless virtual session has nobody to grant, and the session never receives the input.

Reading `/dev/input/event*` needs access to the device nodes, normally the `input` group.

## Behaviour worth knowing

- Key codes arriving on a pointer-role device are sent as mouse buttons. Only the keyboard-role device produces key events, and only its keys are affected by `--swallow-key`.
- Autorepeat (`EV_KEY` value 2) is dropped; KWin generates its own repeat.
- Scrolling comes from the high-resolution wheel events (`REL_WHEEL_HI_RES`, `REL_HWHEEL_HI_RES`) only. A device emitting only detent `REL_WHEEL` events scrolls nothing.
- KWin exposes relative and absolute pointer as separate EIS devices, and relative motion sent to the absolute device is dropped. Absolute positions are scaled from the source device's reported range, so a device reporting a zero absolute range — Sunshine's absolute mouse unless it is configured with a real range — forwards no absolute motion.
- Startup attempts `mlockall` and `setpriority(-5)` to harden against jitter. Both are best effort: failure is logged and the bridge continues unprivileged.
