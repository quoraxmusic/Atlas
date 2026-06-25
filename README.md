# Atlas

Atlas is my accessible macOS build of Vital.

It keeps the sound and workflow that made Vital useful, but changes the parts that made it hard or impossible to use with VoiceOver. The main idea is simple: every important part of the synth should be reachable, named clearly, and usable from the keyboard without having to guess what the visual interface is doing.

This is based on the open source Vital codebase. It is not the official Vital app, and presets made here are meant for Atlas.

## What Atlas Adds

- A VoiceOver-friendly layout with clear groups for presets, oscillators, routing, filters, envelopes, LFOs, effects, modulations, macros, zones, and global controls.
- Six sound sources: four wavetable oscillators, one sample oscillator, and one granular oscillator.
- Two filters with accessible type, style, mode, routing, and filter destination controls.
- Six envelopes, eight LFOs, four random modulators, and sixteen renamable macros.
- Accessible LFO editing, including point editing, curve choices, grid controls, and keyboard-driven navigation.
- Better sample and wavetable browsing, with load, previous, next, and folder browser controls.
- Three effect busses, bus routing, source routing, filter routing, main effects routing, and direct out options.
- More effects than the original Vital build: utility, limiter, frequency shifter, phase shift, and dimension expander, alongside the original chorus, compressor, delay, distortion, EQ, filter, flanger, phaser, and reverb.
- Keyboard shortcuts to jump around the synth faster instead of tabbing through everything.
- AU and VST3 builds for macOS.

## Accessibility

Atlas has a separate accessible layout instead of trying to make VoiceOver follow the original visual UI. Controls are grouped by the way you actually use the synth: oscillators together, filters together, busses together, and so on.

Sliders can be adjusted from the keyboard, values can be typed in, defaults can be restored, macros can be renamed, and modulation assignment is designed to be reachable without using a mouse.

The goal is not just that VoiceOver can see the plugin. The goal is that the synth can actually be programmed.

## Install

Download the latest `.pkg` from the releases page and run it.

The installer includes:

- Audio Unit
- VST3
- the accessible layout guide

The guide is installed to:

`~/Documents/Alessio Plugins/Atlas/Accessible Layout Guide.html`

More details are in [INSTALL.md](INSTALL.md).

## Build

Atlas builds with CMake and JUCE on macOS.

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target Atlas_All -j2
./installer/build-pkg.sh
```

More details are in [BUILDING.md](BUILDING.md).

## File Formats

- Presets: `.atp`
- Banks: `.atb`
- LFO/envelope shapes: `.ats`
- FX presets: `.atf`
- Wavetables: `.att`

Older `.vital`, `.vitalbank`, `.vitallfo`, and `.vitaltable` files can still be loaded, but Atlas presets are not meant to be loaded back into Vital.

## Notes

- Release builds are strongly recommended. Debug builds can use much more CPU.
- Atlas does not connect to Vital accounts, the Vital store, or Vital online services.
- Factory Vital preset content is not included.

## License

Atlas follows Vital's GPLv3 license. See [LICENSE](LICENSE).

Do not use the Vital name, Vital Audio name, Matt Tytel name, or original Vital branding to market this build.
