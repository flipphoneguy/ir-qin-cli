# ir

Control your IR devices (air conditioners, TVs, speakers...) from the Termux command line, using the IR blaster of phones that ship with the Duoqin Remote app (such as the Qin F25).

You add a device once in the Remote app as usual. After that, `ir` sends the exact same signals the app would, without the app being open, so you can control devices from a script, a shortcut, or a quick command.

```
$ ir ls
  1  tv           Living room TV     tv
  2  bedroom      Bedroom AC         AC   on  cool  24°C  fan high  swing
$ ir bedroom cool 22
on  cool  22°C  fan high  swing
$ ir tv vol+*3
```

## Requirements

- A rooted Android phone with an IR blaster and the Duoqin Remote app (`com.duoqin.remote`)
- [Termux](https://termux.dev) with `sudo`
- The Termux:API app plus the `termux-api` package (used to send IR)
- Packages: `pkg install python lua54 fzf termux-api`, then `pip install javaobj-py3`
- To rebuild the helper binary only: `pkg install clang openjdk-17`

## Install

1. Clone the repo anywhere and put `ir` on your PATH:
   ```
   git clone <this repo> ir
   ln -s "$PWD/ir/ir" $PREFIX/bin/ir
   ```
2. In the Duoqin Remote app, add your devices and open each one once, so the app downloads its codes.
3. Import them:
   ```
   ir sync
   ```

A prebuilt `host/kkhost` is included. To rebuild it, run `host/build.sh`.

## Usage

Run `ir help` for the full list.

### Picking a device

`ir ls` lists your devices with a number and a short alias. Any of these work: the number (`ir 2`), the alias (`ir bedroom`) or a unique prefix (`ir bed`). Numbers come from the Remote app and never change. Aliases are generated from the device names, and you can rename them with `ir alias 2 bed`.

### Air conditioners

An AC remote sends its whole state with every press, so `ir` remembers each AC's current state. Whatever you don't mention stays as it was, and changing any setting also turns the AC on.

```
ir bed on | off | toggle
ir bed cool | heat | auto | dry | mode fan
ir bed 24             # °C; also 75f, +1, -2, up, down
ir bed fan high       # auto, low, mid, high (or 0-3)
ir bed swing          # swing on; also swing off, swing next, vane 3
ir bed cool 24 fan high
ir bed send           # resend the current state
ir bed status
```

`ir` knows each model's limits (temperature range per mode, which modes allow fan or temperature control, available vane positions) and rejects anything the AC doesn't support, with a message saying what's allowed.

If someone uses the physical remote, `ir`'s idea of the state goes stale. Sending a full command like `ir bed cool 24 fan auto` puts it back in sync.

### Other devices

```
ir tv keys            # list the buttons
ir tv power           # press a button
ir tv vol+*5 mute     # repeat with *N, several buttons in order
```

Buttons can be given by name, by the label shown in the app, or by a unique prefix.

### Interactive mode

Run `ir` with no arguments to pick a device with fzf, or `ir bed` to open a menu for one device. The menu stays open so you can press several things in a row, until you hit Esc.

### Scripting

- `-n` / `--dry-run`: print `<frequency> <pattern>` instead of sending (the state isn't changed)
- `-j` / `--json`: JSON output for `ls` and `status`
- `-q` / `--quiet`: no output on success
- Exit codes: `0` success, `2` invalid input, `1` failure

When stdout isn't a terminal, `ir` and `ir <device>` print the device list or status instead of opening a menu.

### Adding or removing devices

Make the change in the Remote app (and open new devices there once), then run `ir sync`. Existing aliases and AC states are kept.

## How it works

The Remote app is built on the Kookong IR SDK. When you add a device, it downloads that remote's codes from Kookong and caches them on the phone. `ir sync` copies that cache (it needs root, since it lives in the app's private data).

- **TVs, speakers and similar** store ready-made signals for every button, which `ir` sends as they are.
- **Air conditioners** store a protocol description instead. The app turns a state such as "cool, 24°C, fan high" into bytes with its native library `libkksdk.so`, then runs a small per-model Lua script over them and converts the bytes to IR timings. `ir` uses that same library for the first step and repeats the other steps in Python, so the result is identical to what the app sends.

`libkksdk.so` is a 32-bit Android library meant to be called from Java. `host/kkhost` is a small program that loads it directly by pretending to be the Java runtime, implementing only the few functions the library uses. The library checks that it runs inside the Remote app, so `kkhost` gives it the app's package name and signing certificate. It also tries to contact Kookong's server while starting up, but that call goes nowhere: no network request is made, and encoding works offline.

During `ir sync`, every possible state of every AC is encoded once and stored. After that, commands are simple lookups, and `ir` itself runs in about 0.3 seconds. Sending through Termux:API adds roughly 2 seconds.

## Files

| Path | What it is |
|---|---|
| `ir` | The command line tool |
| `kk.py` | Reads the app's device files and builds IR signals |
| `host/kkhost.c`, `host/crt.c`, `host/build.sh` | The helper that runs `libkksdk.so` |
| `host/libkksdk.so`, `host/cert.der` | The encoder library and signing certificate, taken from the Remote app's APK |
| `tests/verify.py` | Checks the encoder against signals captured from the real app |

Data `ir` keeps:

| Path | Contents |
|---|---|
| `~/.config/ir/aliases.json` | Device aliases |
| `~/.local/state/ir/` | Current state of each AC |
| `~/.cache/ir/` | Copies of the app's files and precomputed signals, rebuilt by `ir sync` |

## Testing

```
python tests/verify.py ~/.cache/ir/app
```

This replays 31 signals captured while the real app controlled an AC and checks that `ir` produces each one bit for bit. It also checks that precomputed signals match live encoding, and that each synced AC's limits match what the app itself computed.

## Limitations

- AC extras such as sleep, turbo and timers aren't supported yet. The app only shows them for some models, and none were available for testing.
- A few remote models use a BeanShell script instead of Lua. `ir` doesn't support those and says so.
- The bundled `libkksdk.so` and certificate come from Duoqin Remote 4.8. Other phones with a different build of the app may need their own copies, taken from the APK's `lib/armeabi-v7a/` and `META-INF/CERT.RSA`.
