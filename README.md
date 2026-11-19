# Ender's Echo Helper

A [Nexus](https://raidcore.gg/Nexus) addon for **Dhuum CM** that helps you avoid **Ender's Echo**.

- **Circle around you.** The circle is **red** if you stand in the Echo's dash, **yellow** if
  you're close to it, and **green** if you're safe. It turns grey-blue while the Echo waits at
  a wall, because its next target isn't chosen yet.
- **Arrow to the Echo,** with its distance in metres at the arrow tip.
- **The Echo's dash on the floor.** A band as wide as the danger zone, from the Echo to the wall
  where the dash will end.
- **Time until it hits you** under your character, shown while you're in the red.

It works on its own from the start of the fight. ArcDPS isn't needed.

## Install

1. Install [Nexus](https://raidcore.gg/Nexus) if you don't have it yet.
2. Download `EndersEcho.dll` from the
   [latest release](https://github.com/Lexo2011/Ender-s-Echo-Helper/releases/latest).
3. Put it into `Guild Wars 2\addons\`.
4. In game, open Nexus, find **Ender's Echo Helper** in the addon library and load it.

Nexus checks this repository for new versions and offers updates by itself.

## Using it

There's nothing to do in the fight. The addon arms itself in Hall of Chains and starts when
combat starts.

- **First seconds of each attempt:** the addon has to find the Echo first. Expect the arrow
  about 3–4 seconds after the pull, once the Echo has started its first dash. If it picks the
  wrong target at the start, it corrects itself at the next wall bounce at the latest.
- **First few attempts:** the addon learns where the Echo spawns and where the arena edge is.
  Until then the floor path is a fixed 50 m long. After a few attempts it ends at the wall.
- **Practice anywhere:** press **ALT+SHIFT+E**, or use the button in the options, to spawn a
  fake Echo that dashes at you on a round arena. Good for getting used to the colours and
  the path.

## Settings

Open the Nexus options and select **Ender's Echo Helper**.

| Setting | What it does |
| --- | --- |
| Red below / Yellow below | How close to the Echo's path counts as "in the path" (red) or "close" (yellow). Also sets the width of the floor band. |
| Show the Echo's path | Floor band (default) or a thin line, with colour and edge thickness. |
| Show distance / time until hit | The text at the arrow tip and under your character. |
| Circle and arrow | Size and thickness of the circle and the arrow. |
| Only in Hall of Chains | Hides the addon everywhere else. Practice mode always works. |

Settings, the learned spawn and the learned arena edge are stored in
`Guild Wars 2\addons\EndersEcho\`. If the path ever looks wrong for several attempts in a row,
delete `echo_spawn.txt` and `echo_arena.txt` there, and the addon relearns them.

## Good to know

- **How it works:** the addon reads the Echo's position from the game's memory. It only reads
  and never changes anything in the game. Reading game memory is a grey area under ArenaNet's
  third-party tool policy, so use it at your own discretion.
- **After a game patch:** the Echo tracking doesn't depend on the game version, so it should
  keep working after patches. If it doesn't, check for an update.
- **Not affiliated:** this addon has no connection to ArenaNet or Raidcore.

## Building from source

Requires Visual Studio 2022 or newer with the C++ workload.

```
git clone --recursive https://github.com/Lexo2011/Ender-s-Echo-Helper
cmake -S . -B build -A x64
cmake --build build --config Release
```

The DLL ends up in `build/Release/EndersEcho.dll`.

`-DENDERSECHO_DEV=ON` builds the developer version. It adds the tools used to find the Echo in
memory: memory diagnostic, Echo hunt, test species and an unfinished auto-mark.
