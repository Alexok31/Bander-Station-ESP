# Countdown on the mouth

Relative timers use the existing reminder scheduler. The mouth displays the
nearest relative deadline, including timers created through the web UI. Daily
reminders do not occupy the display. Below one hour the format is `MM:SS`;
at one hour or above it is `H HH:MM`. A due timer stays at `00:00` until the
existing reminder delivery completes or the timer is cancelled.

The countdown is composed over the mouth only, preserving the underlying face
buffer. AI activity, games, volume/battery overlays, mode selection and network
search have priority. Eyes continue their usual animations. Removing the last
relative timer restores the normal mouth. Existing automatic sleep protection
for active reminders remains in effect. Relative timers live in RAM and do not
survive a restart.

The server planner recognizes explicit timer requests without requiring a task,
for example “Поставь таймер на пять минут”. Ordinary reminders with a missing
task still ask what to remind about. Deployment requires the firmware update and
a server restart; the display itself requires no network requests.
