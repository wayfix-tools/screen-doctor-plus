# Screen Doctor Plus

Extended version of [Screen Doctor](https://github.com/shurarama/screen-doctor) with single-screen mode.

## Extra feature: GRAB_OVERRIDE_SCREEN

Set `GRAB_OVERRIDE_SCREEN=N` to expose only the Nth monitor to the application. The app will see a single screen and take a single screenshot.

- `GRAB_OVERRIDE_SCREEN=0` — show only primary monitor
- `GRAB_OVERRIDE_SCREEN=1` — show only secondary monitor
- unset — show all monitors (default, same as public version)

This works by intercepting `xcb_randr_get_monitors_reply` and returning only the selected monitor with its position reset to (0,0). The `xcb_copy_area` intercept then translates virtual coordinates back to the real screen position.

## Usage

```ini
Exec=env QT_QPA_PLATFORM=xcb WAYLAND_DISPLAY= GRAB_OVERRIDE_SCREEN=0 LD_PRELOAD=/path/to/grab_override.so /path/to/app
```

## Building

```bash
make
```

## Logs

`/tmp/grab_override.log`:

```
[17:44:35 pid=222338] GRAB_OVERRIDE_SCREEN=0 (single screen mode)
[17:44:35 pid=222338] Filtering monitors: keeping only #0 (2560x1440+0+0), hiding 1 others
[17:44:52 pid=222338] xcb_copy_area from ROOT (single-screen mode): virtual(0,0) -> real(0,0) 2560x1440
[17:44:53 pid=222338] Wrote 2560x1440 screenshot to pixmap
```
