# kbd-light

`aurora-kbd-light` drives an Apple silicon MacBook's keyboard backlight from its ambient light sensor, the way macOS does.
`aurora-display-light` does the same for the built-in display's brightness ([below](#display-brightness)).

- **Follows the room:** the keyboard is at its brightest in the dark and fades out as the room gets brighter. Above `ON_LUX` it stays off; once lit, it stays on until `OFF_LUX`. Changes fade over about half a second.
- **Respects manual changes:** a change made by anyone else wins. Turning the backlight off (for example from the desktop's slider, or the desktop's idle dimming) keeps it off until it is turned on again. Any other level holds until the room's light changes about fourfold. When the desktop restores the last automatic level after idle dimming, automatic control resumes.
- **Lid and display sleep:** with the lid closed, or once every connected display has been powered off for a second (as when the desktop blanks the screens), the backlight is off. It fades back in when a display wakes, to the held level if one was set.
- **Idle keyboard (optional):** with `AURORA_KBD_LIGHT_IDLE_SECONDS` set, the backlight fades down to `AURORA_KBD_LIGHT_IDLE_PERCENT` (off by default) after that many seconds without a key press on any keyboard, and back up at the next key press, after a resume from suspend, or when the lid opens or a display wakes. Only the timing of key events is read. It is off by default, since some desktops dim the keyboard backlight themselves when the session goes idle; it suits desktops that don't, such as Hyprland.

It reads the light sensor and writes the LED through sysfs, and reads the lid switch (and, for the idle setting, the keyboards) through evdev, so it needs nothing beyond libc. It works with any desktop and runs as a system service. It finds the first LED named `*kbd_backlight` and the first IIO device with `in_illuminance_input` (`aop-sensors-als` on Apple silicon). Without either it exits quietly.

## Settings

Set these in `/etc/default/aurora-kbd-light`:

| Variable | Default | Meaning |
| --- | --- | --- |
| `AURORA_KBD_LIGHT_FULL_LUX` | 2 | At or below this, the brightest automatic level |
| `AURORA_KBD_LIGHT_ON_LUX` | 60 | Below this the backlight comes on |
| `AURORA_KBD_LIGHT_OFF_LUX` | 120 | Above this it goes off again |
| `AURORA_KBD_LIGHT_MIN_PERCENT` | 10 | Level just after it comes on |
| `AURORA_KBD_LIGHT_MAX_PERCENT` | 50 | Level in the dark |
| `AURORA_KBD_LIGHT_IDLE_SECONDS` | 0 | Dim after this many seconds without a key press (0: never) |
| `AURORA_KBD_LIGHT_IDLE_PERCENT` | 0 | Level while dimmed, if lower than the current one (0: off) |

`aurora-kbd-light -v` logs each level it sets and each change it respects.

## Display brightness

`aurora-display-light` drives the built-in display's brightness from the same sensor, with macOS's auto-brightness model
(CoreBrightness's `CBAutoBrightnessModuleSKL`, read on a MacBook Air M5 running macOS 27.2):

- **Curve:** the light maps to a brightness in nits along a straight line, from `DARK_NITS` in the dark to `REF_NITS`
  at `REF_LUX`, and on up to the panel's maximum (lux is capped at 5000). The defaults are macOS's factory line.
- **Calm:** a target within `DEADBAND` (10%) of what is shown is ignored, so the screen doesn't move with small changes
  in the light.
- **Smooth:** changes run as ramps in perceptual brightness, on a log scale between `MIN_NITS` and the panel's maximum:
  - dimming takes `DIM_SECONDS` (5 s);
  - brightening takes `BRIGHTEN_SECONDS` (1.5 s) for a large step (a tenth of the range or more), and 5 s for a small
    one (up to 0.04), with a linear blend in between.

  A change of light during a ramp restarts it from where the screen is.
- **Learns:** a brightness you pick (the desktop's slider or the brightness keys) moves the line through it:
  - in a room darker than `PIVOT_LUX` (78.5 lx) the dark end moves;
  - in a brighter room the bright end moves.

  The line never falls as the light rises. It is saved in `/var/lib/aurora-display-light/curve`; delete that file to go
  back to the factory line.
- **Respects other changes:** idle dimming, or the level a desktop restores when a session starts, holds until it
  returns to the automatic level or the light changes about fourfold. These are not learned. A change counts as yours
  when a key, the touchpad or a mouse was used in the two seconds before it.
- **Lid and display sleep:** with the lid closed or every display powered off, the panel is left alone.

The backlight's brightness is taken to be in nits, as the Apple DCP driver reports it for the built-in panel (a
`platform` backlight). Other backlights can be named with `AURORA_DISPLAY_LIGHT_BACKLIGHT`.

Set these in `/etc/default/aurora-display-light` (every name starts with `AURORA_DISPLAY_LIGHT_`):

| Variable | Default | Meaning |
| --- | --- | --- |
| `DARK_NITS` | 53.75 | The line at 0 lux |
| `REF_LUX` | 262.5 | Where `REF_NITS` is given |
| `REF_NITS` | 170.89 | The line at `REF_LUX` |
| `PIVOT_LUX` | 78.5 | Learning below this moves the dark end, above it the bright end |
| `MIN_NITS` | 2 | The panel's darkest level: floor of the curve and of the perceptual scale |
| `DEADBAND` | 0.10 | Relative change below which a new target is ignored |
| `DIM_SECONDS` | 5 | Ramp length when dimming (and for small brightening steps) |
| `BRIGHTEN_SECONDS` | 1.5 | Ramp length for a large brightening step |
| `LEARN` | 1 | 0: never learn from your changes, hold them instead |
| `BACKLIGHT` | (auto) | The backlight's name under `/sys/class/backlight` |

A learned line overrides `DARK_NITS` and `REF_NITS` while its file exists. `aurora-display-light -v` logs each ramp,
each change it learns and each one it holds.

## Build

```sh
make
make install DESTDIR=... PREFIX=/usr
```

The services are `aurora-kbd-light.service` and `aurora-display-light.service`.
