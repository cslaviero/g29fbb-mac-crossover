# G29 FFB bridge for macOS + CrossOver

Once upon a time, there was no Force Feedback on CrossOver, at least on Logitech wheels (like G29 which I happen to own). People gathered around, claiming, praying for it. How on earth such blasphemy govern us, it is of the utmost despair that we are not allowed to play those called racing games with the absence of those forces pushing our hands back and forth! That's nonsense!

Well, thanks to AI (and lots of research and debugging), I think we now have it (sorta).

## BIG WARNING: this is completely and absolutely experimental. If you don't know the risks, do not attempt. 

I mean, I wouldn't bet my life on it for sure. But apart from that, it definetely will have (lots of) bugs. I just havent' had the time to play enough to find them. You probably will. Sorry for that. At least... hey, force feedb-oh, look, a new bug

Below a particularly well written description of the project, tks to the Power of AI (R). I added _italics_ so it looks like somelse wrote it (non-italics are my comments, of course):

*Bridge DirectInput force feedback from Windows games (via CrossOver/Wine) to a Logitech G29 on macOS. This project hooks DirectInput inside the Windows game, converts ConstantForce updates to UDP, and feeds them to a macOS daemon that talks to the wheel using Logitech's classic FFB protocol.*

## _How it works_

1) *`dinput8.dll` proxy is placed on same folder that `acs.exe` and intercepts DirectInput FFB calls.*
2) *When effects are set, the proxy sends UDP messages like `CONST 42` / `STOP` to the daemon.*
3) *A macOS daemon* (🎶 'she has daemons on the inside' 🎶 )*(`g29ffb`) receives UDP and sends HID output reports to the wheel*.

## *Current status* (don't quote me)

- *ConstantForce works (game FFB signal flows to the wheel).*
- *Damper/Spring/etc are not yet mapped to force* (but we have logs, and they show it. just a matter of v.0.2, Soon™️)
- *Works in CrossOver with Assetto Corsa (tested).*

## *Requirements*

- *macOS...* (such a Sherlock here)
- *... with a Logitech G29* (that's AI)
- *Swift toolchain (SwiftPM)*
- *CrossOver/Wine for the Windows game* (once Windows game, always Windows game...)
- *MinGW-w64 to build our proxy `dinput8.dll`*

## Running macOS daemon

*Pick the device index and report ID that worked in the local tests.* (ok that was on me. I've tested locally, it shows two "devices" , and report ID comes from HID USB magic - 0x00 worked for me; I guess it's universal so not changing that now - later maybe just keep some things default would be nice)

```bash
swift run g29ffb --daemon --device 1 --report-id 0x00 --max 127 --rate 80
```

Flags:

- `--device`: index from the CLI prompt
- `--report-id`: report ID used by the wheel (often `0x00`)
- `--max`: clamp force in [-127, 127]
- `--rate`: update rate in Hz (tested with 120, AI says 60-100 is a good range)

## *Build & install DirectInput proxy*

*You need to build and copy the DLL into the game folder. This script does just that:*

```bash
scripts/build-copy-dinput8.sh "/folder/where/there/is/crossover/Assetto Corsa/drive_c/Program Files (x86)/Steam/steamapps/common/assettocorsa"
```

If needed, add a Wine override, going to Control Panel, Wine config, than on Libraries, add `dinput8`. It will be added and display `dinput8(native, builtin`) . If so, save and close. 

## *Launch the game with env vars*

*On CrossOver, the safest way is adding `ffb_launch.bat` inside the bottle containing the following:*

```
@echo off
set FFB_LOG=0
set FFB_LOG_EVERY_MS=200
set FFB_HOST=127.0.0.1
set FFB_PORT=21999
start "" "C:\Program Files (x86)\Steam\steamapps\common\assettocorsa\acs.exe"
```

*Run it via CrossOver's "Run Command".*

*Env vars:*

- *`FFB_HOST` (default `127.0.0.1`)*
- *`FFB_PORT` (default `21999`)*
- *`FFB_LOG=0` disables proxy logging *
- *`FFB_LOG_EVERY_MS=200` throttles logging*

## Testing UDP without the game

Inside the bottle:

```bash
ffb_client.exe const 40
```

On macOS, you should see on console the daemon is running that force is being received; it may also work now on your wheel.

## Troubleshooting

- No force, but UDP arrives:

  - Verify `--device` and `--report-id` are correct for your wheel.
  - Try the other device index (G29 often appears twice).
- High CPU:

  - Disable logging with `FFB_LOG=0` or throttle with `FFB_LOG_EVERY_MS=200`.
  - Lower daemon rate to 60-80 Hz.
- No UDP:

  - Confirm `FFB_HOST` and `FFB_PORT`.
  - Test with `nc -klu 21999` on macOS to see messages.

## Project layout

- `Sources/g29ffb`: macOS daemon (HID + UDP)
- `clients/dinput8_proxy`: DirectInput proxy (Windows DLL)
- `clients/ffb_client`: simple UDP test client
- `Docs/`: project docs

## Safety

FFB can be strong. Keep your hands on the wheel and start with lower force (`--max 60`).
