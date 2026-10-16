# Tilt eyes

Settings → Eyes and mouth contains “Глаза реагируют на наклон”, enabled by
default and persisted in NVS as `tilt_eyes`. Save settings restarts the device.
The existing wave tare button is now labelled “Центр волны и взгляда”: it updates
the shared reference immediately and persists it for both effects.

Awake idle eyes follow left/right tilt with a maximum two-pixel displacement.
The mounted sensor axis and sign match the liquid mouth. Strong sustained tilt
adds a mild squint. Filtering, a neutral dead zone and a 250 ms return delay
prevent jitter. Sharp impacts and stale MPU readings cannot drive the gaze;
it eases back to neutral. At rest the existing idle gaze remains active.

Games, sleeping eyes, low-battery eyes, AI expressions, encoder-press expressions
and shake reactions retain priority. No network calls, extra MPU reads or audio
are involved. The effect advances with the existing 150 ms eye refresh.
