# Kitty DOOM

A pure C port of DOOM that renders directly in your terminal using the [Kitty Graphics Protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/).
Play the legendary first-person shooter in terminals that support the protocol.

Supported Terminals
- [Kitty](https://sw.kovidgoyal.net/kitty/) - Full support, best performance

## Features

- Terminal graphics rendering via [Kitty Graphics Protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/)
- Pure C99 implementation with POSIX threading
- Mouse support via SGR 1006 protocol (view rotation + button actions)
- Sound effects (SFX) via [miniaudio](https://miniaud.io/) + PureDOOM audio buffer
- Lock-free atomic operations for input handling
- SIMD-optimized base64 encoding (NEON/SSE3)
- Standard VT input sequences for keyboard control
- Cross-platform support for Linux and macOS
- Based on the [PureDOOM](https://github.com/Daivuk/PureDOOM) single-header port

## Technical Notes

### Rendering Pipeline
- Resolution: 320x200 framebuffer (classic DOOM resolution)
- Color format: RGB24 (indexed palette to RGB conversion)
- Transfer: Base64-encoded in 4KB chunks with SIMD optimization
  * Arm64: NEON intrinsics (6.9x speedup over scalar)
  * x86-64: SSSE3 intrinsics for base64 encoding
    - Processes 12 bytes → 16 base64 chars per iteration
    - Uses pshufb for bit extraction and table lookup
- Frame skipping: SSE2/SSE4.2 optimized frame difference detection
  * Compares 48 bytes (16 RGB24 pixels) per iteration
  * Uses POPCNT instruction for fast bit counting
  * Skips transmission if < 5% pixels changed (reduces bandwidth)
- Protocol: Kitty Graphics Protocol with frame-by-frame transmission
- Display: First frame uses `a=T` (transmit), subsequent frames use `a=f` (frame update)

### Input System
- Threading: Dedicated pthread for input handling
- Parsing: VT sequence state machine (ground → esc → csi/ss3)
- Key tracking: Lock-free atomic bitmap (C11 atomics)
  * 256-bit bitmap using 4x64-bit atomic words
  * Zero contention, 5-20x faster than mutex-based approach
- Key behavior: Unified 50ms release delay for all keys
  * Prioritizes menu responsiveness over movement smoothness
  * Repeat detection prevents duplicate key events from terminal auto-repeat
  * Trade-off: Fast menu navigation, acceptable movement fluidity
- Mouse support: SGR 1006 extended mouse protocol
  * Relative movement tracking with 10x sensitivity multiplier
  * Delta clamping (±100 cells) prevents coordinate jumps
  * Button mapping: Left=Fire, Right=Use, Middle=Run
  * Wheel events ignored (no action)
  * Thread-safe integration with PureDOOM API
- Fire key: F or I keys (Ctrl is difficult to capture in terminal environments)

### Engine
- Based on [PureDOOM](https://github.com/Daivuk/PureDOOM) single-header port
- Frame rate: 35 FPS (original DOOM timing)
- No sound support (display-only implementation)

## Requirements

- C11-compatible compiler (GCC 4.9+ or Clang 3.1+)
- GNU Make 3.81 or later
- POSIX pthread library
- curl or wget for dependency download
- Terminal with Kitty Graphics Protocol support (e.g., Kitty)

## Quick Start

```bash
git clone https://github.com/jserv/kitty-doom
cd kitty-doom
make
make run
```

The build system automatically downloads `PureDOOM.h` and `DOOM1.WAD` (shareware version) on first build.

## Build Targets

```bash
make                  # Build the project (downloads dependencies automatically)
make run              # Build and run the game
make check            # Run all tests and performance regression checks
make profile          # Build with profiling enabled (for performance analysis)
make download-assets  # Manually download DOOM1.WAD and PureDOOM.h
make clean            # Remove build artifacts
make distclean        # Remove all generated files including downloads
```

### Performance Testing

```bash
# Run all tests including performance regression
make check

# Development: Real-time profiling (requires Kitty/Ghostty terminal)
make profile
./build/kitty-doom 2> profile.log
```

## Running the Game

```bash
# Basic usage
./build/kitty-doom

# Or use the convenience target
make run
```

For detailed controls and gameplay options, see [USAGE.md](USAGE.md).

## License

This project is released under GPL-2.0. See [LICENSE](LICENSE) for details.

The GPL is inherited from:
- DOOM Engine: Released by id Software under GPL-2.0 in 1997
- PureDOOM: Derived from id Software's GPL code
- This Project: As a derivative work, also under GPL-2.0

GPL ensures that DOOM and its derivatives remain free and open source.
