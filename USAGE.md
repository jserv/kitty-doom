# kitty-doom Usage Guide

kitty-doom is a port of DOOM that runs in terminals supporting the Kitty Graphics Protocol.
This guide covers the controls and gameplay features.

## Controls

### Movement

| Key | Action |
|-----|--------|
| Up Arrow | Move forward |
| Down Arrow | Move backward |
| Left Arrow | Turn left |
| Right Arrow | Turn right |
| , (comma) | Strafe left |
| . (period) | Strafe right |

Note: Arrow keys use intelligent key repeat detection (80ms window) - when you hold down a key, it stays continuously pressed.
This matches terminal key repeat behavior and provides smooth movement. Single taps work in menus - the first press moves one item, and the key automatically releases if you don't hold it down.

### Mouse Controls

| Action | Control |
|--------|---------|
| Look around | Move mouse |
| Fire weapon | Left mouse button |
| Use / Open doors | Right mouse button |
| Run | Middle mouse button |

Note: Mouse support uses the SGR 1006 extended mouse protocol. Mouse movement controls view rotation (look left/right/up/down).
For continuous firing, click repeatedly - buttons use a 50ms release delay matching keyboard action keys.

### Actions

| Key | Action |
|-----|--------|
| Space, F, I | Fire weapon (any of these keys work) |
| E | Use / Open doors / Activate switches |
| Right Alt | Strafe mode (hold to strafe with arrow keys) |
| Right Shift | Run (hold to move faster) |

Note: F and I keys are used for firing because Ctrl is difficult to capture in terminal environments.
This follows the convention used in other terminal DOOM implementations.

### Weapons

Press number keys to switch weapons:

| Key | Weapon |
|-----|--------|
| 1 | Fist / Chainsaw |
| 2 | Pistol |
| 3 | Shotgun / Super Shotgun |
| 4 | Chaingun |
| 5 | Rocket Launcher |
| 6 | Plasma Rifle |
| 7 | BFG 9000 |

### Menu & System

| Key | Action |
|-----|--------|
| Esc | Open menu / Exit menu |
| Enter | Select menu item / Send message (in chat) |
| Tab | Toggle automap |
| Pause | Pause game |
| F1 | Help (opens menu) |
| F2 | Save game (opens menu) |
| F3 | Load game (opens menu) |
| F5 | Toggle crosshair |
| F6 | Quick save |
| F7 | End game |
| F8 | Toggle messages on/off |
| F9 | Quick load |
| F10 | Quit game |
| F11 | Toggle gamma correction (brightness) |

### Automap Controls

When the automap is open (Tab):

| Key | Action |
|-----|--------|
| - (minus) | Zoom out |
| + (plus) / = | Zoom in |
| 0 | Maximum zoom out |
| Arrow keys | Pan the map |
| f | Follow mode toggle |
| g | Grid toggle |
| m | Mark location |
| c | Clear all marks |

### Multiplayer Chat

In multiplayer games, press the following keys to send messages to specific players:

| Key | Action |
|-----|--------|
| g | Send message to Green player |
| i | Send message to Indigo player |
| b | Send message to Brown player |
| r | Send message to Red player |
| Enter | Send typed message |
| Esc | Cancel message |

## Gameplay Tips

1. Arrow keys automatically stay pressed when held - smooth continuous movement without lag.
2. Use mouse for aiming - move the mouse to rotate view, left-click to fire.
3. Use Space, F, or I keys to fire your weapon (easier than Ctrl in terminals).
4. The game runs at 35 FPS (original DOOM timing).
5. Press ESC to access the menu at any time.
6. Press F11 to cycle through brightness levels if the game is too dark.
7. The automap (Tab) shows your current position and the level layout.
8. In menus, tap arrow keys to move selection - key repeat detection ensures single-item movement.
9. Mouse support requires Kitty terminal - other terminals may have issues.

### Terminal Compatibility

**Kitty is the only fully supported terminal.**

- **Kitty**: Full support with all features (keyboard + mouse)
  * Best performance and complete Kitty Graphics Protocol implementation
  * Recommended terminal for optimal gameplay experience
  * Download: https://sw.kovidgoyal.net/kitty/

- **Other terminals**: Limited or no support
  * Ghostty, WezTerm: Partial Kitty Graphics Protocol support with known issues
  * Most terminals lack proper protocol implementation
  * May experience visual corruption, rendering issues, or missing features

**Important**: This game requires the Kitty Graphics Protocol. Running in unsupported terminals will cause display problems. For the best experience, use Kitty terminal.

## Configuration

kitty-doom uses default DOOM keybindings. The game stores its configuration in the home directory.

### Default Settings

- Show Messages: On
- Always Run: Off

**Note**: kitty-doom currently does not support audio output. Sound and music settings are ignored.

## Command Line Options

kitty-doom supports standard DOOM command line parameters:

```bash
./build/kitty-doom -skill 4                 # Set difficulty (1-5)
./build/kitty-doom -episode 1               # Start at specific episode (DOOM 1)
./build/kitty-doom -warp 1 1                # Warp to level E1M1 (DOOM 1)
./build/kitty-doom -warp 1                  # Warp to MAP01 (DOOM 2)
./build/kitty-doom -file mycustom.wad       # Load PWAD file
./build/kitty-doom -playdemo demo1          # Play demo
./build/kitty-doom -record demo1            # Record demo
./build/kitty-doom -timedemo demo1          # Time demo playback
./build/kitty-doom -config myconfig.cfg     # Use custom config file
./build/kitty-doom -nomonsters              # No monsters mode
./build/kitty-doom -respawn                 # Monsters respawn
./build/kitty-doom -fast                    # Fast monsters
```

### IWAD Detection

kitty-doom automatically searches for IWAD files in the current directory and common locations:
- `doom1.wad` (Shareware)
- `doom.wad` (Registered)
- `doomu.wad` (Ultimate DOOM)
- `doom2.wad` (DOOM 2)
- `plutonia.wad` (Final DOOM: The Plutonia Experiment)
- `tnt.wad` (Final DOOM: TNT: Evilution)

Place the IWAD file in the same directory as the executable or set the `DOOMWADDIR` environment variable to specify the IWAD location.

### Skill Levels

| Skill | Name | Description |
|-------|------|-------------|
| 1 | I'm Too Young to Die | Very easy |
| 2 | Hey, Not Too Rough | Easy |
| 3 | Hurt Me Plenty | Normal |
| 4 | Ultra-Violence | Hard |
| 5 | Nightmare! | Very hard |

## Terminal Requirements

### Required Features
- Kitty Graphics Protocol support
- VT sequence parsing (arrow keys, function keys)
- Minimum terminal size: 80x24 cells (larger recommended for better experience)

### Supported Terminal
- **Kitty only**: This is the only fully supported terminal
  * Download: https://sw.kovidgoyal.net/kitty/
  * Other terminals are not recommended and may not work correctly

### Display
- True color (24-bit) support recommended
- Hardware-accelerated rendering for smoother framerates

## Troubleshooting

- **Using the wrong terminal?** Kitty is the only supported terminal. Other terminals will have issues.
- If the screen appears corrupted after exit, run `reset` in your terminal
- If keys don't respond, ensure your terminal is in focus
- **Performance issues?** Make sure you're using Kitty with hardware-accelerated rendering enabled
