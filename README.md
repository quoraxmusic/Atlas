# Atlas

Atlas is an accessible software synthesizer based on the open source code of [Vital.](https://github.com/mtytel/vital)

The goal of Atlas is to keep the power and flexibility of Vital while making the instrument fully usable with screen readers on both Windows and macOS. It adds a  keyboard focused UI, accessible editing tools, and several new synthesis, modulation, routing, and effects features.

## Main Features

Atlas includes 4 wavetable oscillators, an improved sample oscillator, a granular oscillator, expanded modulation, more effects, and a more flexible routing system.

The sample oscillator includes control over sample start and end points, retrigger behavior, high and low cut filters, and looping windows.

The granular oscillator includes 2 playback modes, playthrough and manual.

Atlas also expands the modulation system with 8 LFOs, 6 envelopes, 4 random modulators, and 16 macros. Macros can be renamed as well.

## Oscillators

Atlas includes 4 wavetable oscillators.

It also includes an improved sample oscillator with additional controls for start position, end position, retrigger modes, filtering, and loop windows.

The granular oscillator adds another sound source with playthrough and manual playback modes.

## Accessibility

Accessibility is one of the main reasons Atlas exists.

The interface is built to be usable with screen readers on both Windows and macOS. Keyboard shortcuts are included for easier navigation, editing, and sound design.

## Accessible LFO Editor

The LFO editor can be used fully from the keyboard.

You can add and remove points, select multiple points, make non continuous selections, change point values, adjust curves between points, move points in time, change LFO smoothing, set LFO timing in synced or free mode, and adjust the starting phase.

The goal is to make detailed LFO design possible for screen reader users without reducing the depth of the original LFO system.

## Accessible Wavetable Editor

Atlas includes an accessible wavetable editor that can be controlled from the keyboard.

You can adjust harmonics, change harmonic intensity, remove the fundamental, set harmonics, and add harmonics in an accessible way.

This makes it possible to shape wavetables directly without needing to rely on visual editing.

## Modulation

Atlas includes:

* 8 LFOs
* 6 envelopes
* 4 random modulators
* 16 renamable macros

The modulation system is designed to give enough flexibility for complex patches while keeping the controls reachable and understandable through keyboard and screen reader workflows.

## Effects

Atlas includes 14 effects in total.

It includes Vital's 9 stock effects:
* Delay
* Chorus
* Compressor
* EQ
* Filter
* Flanger
* Phaser
* Distortion
* Reverb

Atlas also adds:
* Frequency shifter
* Limiter
* Utility
* Phase shift (inspired by disperser style phase processing)
* Dimension expander

The Utility effect includes control over input gain, output gain, filtering, and stereo width.

## Routing

Atlas includes a more flexible routing system than the original Vital structure.

There are 3 busses, which can be sent to the main effects, direct out, or used as part of more complex routing setups.

Oscillators can be routed directly to filters, busses, main effects, or direct out.

Filters can also be routed to busses, main effects, or direct out, allowing more flexible sound design chains.

This makes it possible to build more advanced patches, layered sounds, parallel processing chains, and cleaner output routing.

## License

Atlas is based on Vital and is licensed under the GPL 3.0 license.

Please see the license file for more details.

## Important Note About Vital Content

Vital wavetables and samples are not included in this GitHub repository.

Atlas is based on Vital's open source code, but this repository does not redistribute Vital's factory wavetables or sample content.
