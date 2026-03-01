# Screen Doctor Plus

Extended version of [Screen Doctor](https://github.com/shurarama/screen-doctor) with monitor filtering.

## Extra feature: GRAB_OVERRIDE_SCREEN

Set `GRAB_OVERRIDE_SCREEN` to control which monitors the application sees.

- `GRAB_OVERRIDE_SCREEN=0` — show only monitor #0
- `GRAB_OVERRIDE_SCREEN=1` — show only monitor #1
- `GRAB_OVERRIDE_SCREEN=0,2` — show monitors #0 and #2, hide the rest
- unset — show all monitors (default, same as public version)

Selected monitors are placed left-to-right in a virtual layout with no gaps.

### Finding monitor indices

Run the included helper script:

```bash
./list-monitors.sh
```

Output:
```
Available monitors:
-------------------
  #0: DP-1 2560/597x1440/336+0+0 (primary)
  #1: eDP-1 1920/309x1080/174+2560+360

Usage: GRAB_OVERRIDE_SCREEN=0      - show only monitor #0
       GRAB_OVERRIDE_SCREEN=0,2    - show monitors #0 and #2
```

## Usage

```ini
Exec=env QT_QPA_PLATFORM=xcb GRAB_OVERRIDE_SCREEN=0 LD_PRELOAD=/path/to/grab_override.so /path/to/app
```

## Building

```bash
make
```

## Logs

`/tmp/grab_override.log`:

```
[17:44:35 pid=222338] GRAB_OVERRIDE_SCREEN=0 (1 screen)
[17:44:35 pid=222338] Keeping monitor #0 (2560x1440+0+0) -> virtual +0+0
[17:44:35 pid=222338] Filtered: 2 -> 1 monitors, hiding 1
[17:44:52 pid=222338] xcb_copy_area from ROOT (filtered): virtual(0,0) -> real(0,0) 2560x1440
[17:44:53 pid=222338] Wrote 2560x1440 screenshot to pixmap
```
