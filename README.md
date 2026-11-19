# Ender's Echo Helper

Nexus addon for the Dhuum CM encounter in Guild Wars 2. It draws a circle on the ground
around you and an arrow pointing at Ender's Echo. The circle is red when you stand in the
Echo's running path, yellow when you are close to it, and green when you are safe. While the
Echo stands at the wall, the circle is grey-blue: its next target hasn't been picked yet.

## Status

- Overlay, colours, settings: done.
- Simulated Echo for testing: done. Press `ALT+SHIFT+E`, or use the button in the Nexus
  options. A fake Echo spawns on a 45 m arena around you and runs at you every ~11 s, the
  same way the real one does.
- Distance in metres next to the arrow (toggle in the options): done.
- Auto-mark the Echo with the X (Cross) agent marker: the logic is done, but it **only
  works once live tracking exists**. It marks once per attempt, because placing the same
  marker again removes it. The preferred path calls the game's own marker function on the
  Echo directly, so nobody needs to target it. The fallback presses the "Set Agent Marker:
  Cross" bind while the Echo is your target. That bind must be set in the game and in
  Nexus' game keybinds. A test button in the options marks your current target.
- Live tracking of the real Echo: **not implemented**. ArcDPS's realtime API does not provide
  NPC positions, so this needs to read the game's memory.

## Build

Requires Visual Studio 2022 or newer with the C++ workload.

```
git submodule update --init
cmake -S . -B build -A x64
cmake --build build --config Release
```

Copy `build/Release/EndersEcho.dll` into `<Guild Wars 2>/addons/` and load it from the
Nexus addon library.

## Echo behaviour (measured from an Elite Insights log)

- Species ID 19628.
- Casts skill 47927 about every 11 s, about 1 s after reaching a wall.
- About 1.1–1.3 s after the cast, it runs in a straight line toward where a random player
  stood when it cast (matches within about 1°).
- Runs at about 400 in/s (≈10.2 m/s) until it hits the arena wall.
