/* Copyright 2013-2019 Matt Tytel
 *
 * vital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * vital is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with vital.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "granular_source.h"

#include "futils.h"
#include "one_pole_filter.h"
#include "synth_constants.h"

namespace vital {
  namespace {
    constexpr int kGranularTransposeQuantizeCustomScale = 14;
    constexpr int kGranularTransposeQuantizeScaleMasks[] = {
      0,
      (1 << 12) - 1,
      (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),
      (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),
      (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 11),
      (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),
      (1 << 0) | (1 << 2) | (1 << 4) | (1 << 7) | (1 << 9),
      (1 << 0) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 10),
      (1 << 0) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 10),
      (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),
      (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),
      (1 << 0) | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 9) | (1 << 11),
      (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),
      (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 8) | (1 << 10),
      -1,
    };

    int rotateGranularTransposeQuantizeMask(int mask, int key) {
      int result = 0;
      key = std::max(0, std::min(kNotesPerOctave - 1, key));
      for (int i = 0; i < kNotesPerOctave; ++i) {
        if ((mask >> i) & 1)
          result |= 1 << ((i + key) % kNotesPerOctave);
      }
      return result;
    }
  }

  void GranularSource::LaneState::reset() {
    playhead = 0.0;
    samples_until_next_grain = 0.0;
    modulation_phase = 0.0;
    for (auto& grain : grains)
      grain.active = false;
  }

  GranularSource::GranularSource() :
      Processor(kNumInputs, kNumOutputs), pan_amplitude_(0.0f), transpose_quantize_(0),
      last_quantized_transpose_(0.0f), low_cut_state_(0.0f), high_cut_state_(0.0f), phase_(0.0f),
      phase_output_(std::make_shared<cr::Output>()), random_generator_(0.0f, 1.0f),
      sample_(std::make_shared<Sample>()) { }

  poly_float GranularSource::onePoleCoefficient(poly_float midi_cutoff, int sample_rate) {
    poly_float frequency = utils::midiNoteToFrequency(midi_cutoff);
    frequency = utils::clamp(frequency, 5.0f, sample_rate * 0.49f);
    return OnePoleFilter<>::computeCoefficient(frequency, sample_rate);
  }

  mono_float GranularSource::wrapPosition(mono_float value, mono_float length) {
    if (length <= 0.0f)
      return 0.0f;

    value = std::fmod(value, length);
    return value < 0.0f ? value + length : value;
  }

  mono_float GranularSource::inputValueAt(int input_index, int frame, int lane) const {
    const auto* source = input(input_index)->source;
    const int index = source->buffer_size > frame ? frame : 0;
    return source->buffer[index][lane];
  }

  void GranularSource::resetLane(int lane, mono_float start) {
    lanes_[lane].reset();
    mono_float manual_position = input(kPosition)->at(0)[lane];
    bool manual = input(kMode)->at(0)[0] >= 0.5f;
    lanes_[lane].playhead = manual ? manual_position : 0.0;
    phase_.set(lane, lanes_[lane].playhead);
  }

  mono_float GranularSource::readSampleAt(int lane, double position, double increment) const {
    int active_length = sample_->activeLength();
    if (active_length <= Sample::kMinSize)
      return 0.0f;

    int active_index = sample_->getActiveIndex(std::max<mono_float>(std::abs(increment), 1.0f));
    int buffer_length = std::max(Sample::kMinSize, active_length >> active_index);
    mono_float scaled_position = wrapPosition(position / (1 << active_index), buffer_length);
    int first = static_cast<int>(scaled_position);
    int second = (first + 1) % buffer_length;
    mono_float blend = scaled_position - first;

    const mono_float* buffer = (lane % 2) ? sample_->getActiveRightLoopBuffer(active_index) :
                                           sample_->getActiveLeftLoopBuffer(active_index);
    buffer += Sample::kBufferSamples;
    return utils::interpolate(buffer[first], buffer[second], blend);
  }

  void GranularSource::spawnGrain(int lane, int frame, mono_float start, mono_float end,
                                  mono_float normalized_position, mono_float pitch_ratio) {
    LaneState& state = lanes_[lane];
    int maximum_active = utils::iclamp(static_cast<int>(input(kGrainCount)->at(0)[0] + 0.5f), 1, kMaxGrains);
    int active = 0;
    for (const Grain& grain : state.grains) {
      if (grain.active)
        active++;
    }
    if (active >= maximum_active)
      return;

    Grain* target = nullptr;
    for (Grain& grain : state.grains) {
      if (!grain.active) {
        target = &grain;
        break;
      }
    }
    if (target == nullptr)
      return;

    const mono_float region = std::max<mono_float>(Sample::kMinSize, end - start);
    mono_float random_bipolar = random_generator_.next() * 2.0f - 1.0f;
    mono_float normalized = normalized_position;
    normalized += random_bipolar * inputValueAt(kRandomPosition, frame, lane) * 0.005f;
    normalized = wrapPosition(normalized, 1.0f);

    mono_float semitones = random_bipolar * inputValueAt(kRandomPitch, frame, lane);
    if (random_generator_.next() * 100.0f < inputValueAt(kIntervalChance, frame, lane))
      semitones += inputValueAt(kInterval, frame, lane);

    int direction = utils::iclamp(static_cast<int>(input(kDirection)->at(0)[0] + 0.5f), 0, 2);
    bool reverse = direction == 1 || (direction == 2 && random_generator_.next() >= 0.5f);
    mono_float grain_pitch = pitch_ratio * utils::centsToRatio(semitones * kCentsPerNote);
    mono_float source_increment = grain_pitch * sample_->activeSampleRate() / getSampleRate() *
                                  (1 << Sample::kUpsampleTimes) * (reverse ? -1.0 : 1.0);
    int grain_length = std::max(1, static_cast<int>(inputValueAt(kGrainSize, frame, lane) *
                                                    0.001f * getSampleRate()));
    mono_float source_span = std::abs(source_increment) * std::max(0, grain_length - 1);
    mono_float safe_position = start + normalized * region;
    if (source_span < region - 1.0f) {
      if (reverse)
        safe_position = std::max<mono_float>(safe_position, start + source_span);
      else
        safe_position = std::min<mono_float>(safe_position, start + region - 1.0f - source_span);
    }

    mono_float volume_variation = random_generator_.next() * inputValueAt(kRandomVolume, frame, lane) * 0.01f;
    mono_float random_pan = random_bipolar * inputValueAt(kRandomPan, frame, lane) * 0.01f;
    mono_float pan_gain = (lane % 2) ? std::sqrt(0.5f * (1.0f + random_pan)) :
                                      std::sqrt(0.5f * (1.0f - random_pan));

    target->active = true;
    target->source_position = safe_position;
    target->source_increment = source_increment;
    target->region_start = start;
    target->region_length = region;
    target->age = 0;
    target->length = grain_length;
    target->gain = (1.0f - volume_variation) * pan_gain;
  }

  void GranularSource::process(int num_samples) {
    sample_->markUsed();

    poly_float current_pan_amplitude = pan_amplitude_;
    poly_float input_pan = utils::clamp(input(kPan)->at(0), -1.0f, 1.0f);
    pan_amplitude_ = futils::panAmplitude(input_pan);

    poly_float input_midi = 0.0f;
    if (input(kKeytrack)->at(0)[0])
      input_midi = input(kMidi)->at(0) - input(kRootKey)->at(0);

    int transpose_quantize = static_cast<int>(input(kTransposeQuantize)->at(0)[0]);
    int quantize_scale = static_cast<int>(input(kTransposeQuantizeScale)->at(0)[0]);
    if (quantize_scale != kGranularTransposeQuantizeCustomScale) {
      quantize_scale = std::max(0, std::min(kGranularTransposeQuantizeCustomScale - 1, quantize_scale));
      int key = static_cast<int>(input(kTransposeQuantizeKey)->at(0)[0]);
      int mode = static_cast<int>(input(kTransposeQuantizeMode)->at(0)[0]);
      transpose_quantize = rotateGranularTransposeQuantizeMask(kGranularTransposeQuantizeScaleMasks[quantize_scale],
                                                               key);
      if (mode >= 1)
        transpose_quantize |= 1 << kNotesPerOctave;
    }

    poly_float transpose = snapTranspose(input_midi, input(kTranspose)->at(0), transpose_quantize);
    transpose = utils::clamp(transpose + input(kTune)->at(0), kMinTranspose, kMaxTranspose);
    poly_float pitch_ratio = utils::centsToRatio(transpose * kCentsPerNote);

    int audio_length = sample_->activeLength();
    mono_float audio_end = audio_length;
    mono_float minimum_region = Sample::kMinSize * (1 << Sample::kUpsampleTimes);
    poly_float sample_start = utils::clamp(input(kStart)->at(0), 0.0f, 1.0f) * audio_end;
    poly_float sample_end = utils::clamp(input(kEnd)->at(0), 0.0f, 1.0f) * audio_end;
    sample_start = utils::floor(utils::clamp(sample_start, 0.0f, audio_end - minimum_region));
    sample_end = utils::floor(utils::clamp(sample_end, sample_start + minimum_region, audio_end));

    poly_mask reset_mask = getResetMask(kReset);
    for (int lane = 0; lane < poly_float::kSize; ++lane) {
      if (reset_mask[lane])
        resetLane(lane, sample_start[lane]);
    }

    mono_float sample_inc = 1.0f / num_samples;
    poly_float delta_pan_amplitude = (pan_amplitude_ - current_pan_amplitude) * sample_inc;

    poly_float* raw_output = output(kRaw)->buffer;
    utils::zeroBuffer(raw_output, num_samples);

    for (int frame = 0; frame < num_samples; ++frame) {
      poly_float frame_output = 0.0f;
      for (int lane = 0; lane < poly_float::kSize; ++lane) {
        LaneState& state = lanes_[lane];
        mono_float start = sample_start[lane];
        mono_float end = sample_end[lane];
        mono_float region = std::max<mono_float>(Sample::kMinSize, end - start);
        bool manual = input(kMode)->at(0)[0] >= 0.5f;
        mono_float density = input(kMidiDensity)->at(0)[0] >= 0.5f ?
                             utils::midiNoteToFrequency(input(kMidi)->at(0)[lane]) :
                             inputValueAt(kDensity, frame, lane);
        mono_float grain_period = getSampleRate() / std::max<mono_float>(1.0f, density);
        mono_float lfo = std::sin(state.modulation_phase * kPi * 2.0);
        mono_float target_position = manual ? inputValueAt(kPosition, frame, lane) :
                                             static_cast<mono_float>(state.playhead);
        target_position += lfo * inputValueAt(kPositionMod, frame, lane) * 0.005f;
        target_position = wrapPosition(target_position, 1.0f);

        if (state.samples_until_next_grain <= 0.0) {
          spawnGrain(lane, frame, start, end, target_position, pitch_ratio[lane]);
          state.samples_until_next_grain += grain_period;
        }

        state.samples_until_next_grain -= 1.0;
        state.modulation_phase += inputValueAt(kPositionModRate, frame, lane) / getSampleRate();
        state.modulation_phase -= std::floor(state.modulation_phase);

        if (!manual) {
          state.playhead += inputValueAt(kSpeed, frame, lane) * sample_->activeSampleRate() /
                            getSampleRate() / region;
          state.playhead -= std::floor(state.playhead);
        }

        mono_float lane_output = 0.0f;
        for (Grain& grain : state.grains) {
          if (!grain.active)
            continue;

          mono_float phase = static_cast<mono_float>(grain.age) / std::max(1, grain.length);
          mono_float window = 0.5f - 0.5f * std::cos(phase * kPi * 2.0f);
          mono_float sample = readSampleAt(lane, grain.source_position, grain.source_increment);
          lane_output += sample * window * grain.gain;

          grain.source_position += grain.source_increment;
          const double region_end = grain.region_start + grain.region_length;
          if (++grain.age >= grain.length || grain.source_position < grain.region_start ||
              grain.source_position >= region_end)
            grain.active = false;
        }

        frame_output.set(lane, lane_output);
        phase_.set(lane, utils::clamp(target_position, 0.0f, 1.0f));
      }
      raw_output[frame] = frame_output;
      VITAL_ASSERT(utils::isContained(raw_output[frame]));
    }

    poly_float low_cutoff = utils::clamp(input(kLowCutoff)->at(0), 8.0f, 136.0f);
    poly_float high_cutoff = utils::clamp(input(kHighCutoff)->at(0), 8.0f, 136.0f);
    poly_mask use_low_cut = poly_float::greaterThan(low_cutoff, 8.5f);
    poly_mask use_high_cut = poly_float::lessThan(high_cutoff, 135.5f);
    poly_float low_coefficient = onePoleCoefficient(low_cutoff, getSampleRate());
    poly_float high_coefficient = onePoleCoefficient(high_cutoff, getSampleRate());

    for (int i = 0; i < num_samples; ++i) {
      poly_float value = raw_output[i];
      poly_float low = processLowPass(value, low_cut_state_, low_coefficient);
      value = utils::maskLoad(value, value - low, use_low_cut);
      poly_float high = processLowPass(value, high_cut_state_, high_coefficient);
      raw_output[i] = utils::maskLoad(value, high, use_high_cut);
    }

    const poly_float* level_input = input(kLevel)->source->buffer;
    poly_float* levelled_output = output(kLevelled)->buffer;
    for (int i = 0; i < num_samples; ++i) {
      current_pan_amplitude += delta_pan_amplitude;
      poly_float level = utils::clamp(level_input[i], poly_float(0.0f), poly_float(kMaxAmplitude));
      levelled_output[i] = current_pan_amplitude * level * level * raw_output[i];
    }

    phase_output_->buffer[0] = utils::encodePhaseAndVoice(phase_, input(kNoteCount)->at(0));
    sample_->markUnused();
  }

  poly_float GranularSource::snapTranspose(poly_float input_midi, poly_float transpose, int quantize) {
    if (quantize == 0)
      return input_midi + transpose;

    bool global_transpose = utils::isTransposeQuantizeGlobal(quantize);
    poly_float pre_add = 0.0f;
    poly_float post_add = input_midi;
    if (global_transpose) {
      pre_add = input_midi;
      post_add = 0.0f;
    }

    poly_float snapped = utils::snapTranspose(pre_add + transpose, quantize);
    last_quantized_transpose_ = snapped;
    transpose_quantize_ = quantize;
    return post_add + snapped;
  }
} // namespace vital
