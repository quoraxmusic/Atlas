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

#include "dimension_expander_module.h"

#include "poly_utils.h"

#include <algorithm>
#include <cmath>

namespace vital {
namespace {
  constexpr mono_float kTwoPi = 6.28318530717958647692f;

  mono_float clampDimensionScalar(mono_float value, mono_float min, mono_float max) {
    return std::min(std::max(value, min), max);
  }
} // namespace

  DimensionExpanderModule::DimensionExpanderModule(std::string prefix) :
      SynthModule(0, 1), amount_(nullptr), low_cutoff_(nullptr), high_cutoff_(nullptr),
      prefix_(prefix), write_index_(0), amount_smoothed_(0.0f), low_cut_state_(0.0f),
      high_cut_state_(0.0f) {
    delay_buffer_.fill(0.0f);
  }

  void DimensionExpanderModule::init() {
    amount_ = createMonoModControl(prefix_ + "dimension_expander_amount");
    low_cutoff_ = createMonoModControl(prefix_ + "dimension_expander_low_cutoff", true, true);
    high_cutoff_ = createMonoModControl(prefix_ + "dimension_expander_high_cutoff", true, true);

    SynthModule::init();
  }

  mono_float DimensionExpanderModule::onePoleCoefficient(mono_float midi_cutoff, int sample_rate) {
    mono_float frequency = utils::midiNoteToFrequency(midi_cutoff);
    frequency = clampDimensionScalar(frequency, 5.0f, sample_rate * 0.49f);
    return 1.0f - std::exp(-kTwoPi * frequency / sample_rate);
  }

  mono_float DimensionExpanderModule::dimensionDelaySamples(int sample_rate) {
    mono_float delay = sample_rate * (22.0f + 4.0f * kDefaultDelayMs) - 200000.0f;
    delay = (delay / 208000.0f) * (delay / 208000.0f);
    return clampDimensionScalar(delay, 1.0f, kMaxDelaySamples - 2.0f);
  }

  poly_float DimensionExpanderModule::readDelay(mono_float delay_samples) const {
    int delay_floor = static_cast<int>(delay_samples);
    mono_float fraction = delay_samples - delay_floor;
    int index_a = (write_index_ - delay_floor + kMaxDelaySamples) & (kMaxDelaySamples - 1);
    int index_b = (index_a - 1 + kMaxDelaySamples) & (kMaxDelaySamples - 1);
    return utils::interpolate(delay_buffer_[index_a], delay_buffer_[index_b], fraction);
  }

  void DimensionExpanderModule::processWithInput(const poly_float* audio_in, int num_samples) {
    SynthModule::process(num_samples);

    const mono_float low_cutoff = low_cutoff_->buffer[0][0];
    const mono_float high_cutoff = high_cutoff_->buffer[0][0];
    const bool use_low_cut = low_cutoff > 8.5f;
    const bool use_high_cut = high_cutoff < 135.5f;
    const mono_float low_coefficient = use_low_cut ? onePoleCoefficient(low_cutoff, getSampleRate()) : 0.0f;
    const mono_float high_coefficient = use_high_cut ? onePoleCoefficient(high_cutoff, getSampleRate()) : 1.0f;
    const mono_float delay_samples = dimensionDelaySamples(getSampleRate());

    poly_float current_amount = amount_smoothed_;
    amount_smoothed_ = utils::clamp(amount_->buffer[0], 0.0f, 1.0f);
    poly_float amount_delta = (amount_smoothed_ - current_amount) * (1.0f / num_samples);
    poly_float* audio_out = output()->buffer;

    for (int i = 0; i < num_samples; ++i) {
      current_amount += amount_delta;

      poly_float band = audio_in[i];
      if (use_low_cut) {
        poly_float low = processLowPass(band, low_cut_state_, low_coefficient);
        band -= low;
      }
      if (use_high_cut)
        band = processLowPass(band, high_cut_state_, high_coefficient);

      poly_float swapped = utils::swapVoices(band);
      poly_float mono = (band + swapped) * 0.5f;
      poly_float delayed = readDelay(delay_samples);
      delay_buffer_[write_index_] = mono;
      write_index_ = (write_index_ + 1) & (kMaxDelaySamples - 1);

      mono_float amount_percent = current_amount[0] * 100.0f;
      mono_float fxamount = 0.5f + amount_percent / 115.0f;
      mono_float direct = (1.5f - fxamount) * fxamount;
      mono_float delayed_amount = (fxamount - 0.5f) * fxamount;

      poly_float expanded = (band + mono * direct) * 0.7f;
      expanded += poly_float(-delayed_amount, delayed_amount, 0.0f, 0.0f) * delayed * 0.7f;

      audio_out[i] = audio_in[i] + (expanded - band) * current_amount;
    }
  }

  void DimensionExpanderModule::hardReset() {
    delay_buffer_.fill(0.0f);
    write_index_ = 0;
    amount_smoothed_ = 0.0f;
    low_cut_state_ = 0.0f;
    high_cut_state_ = 0.0f;
  }

  void DimensionExpanderModule::enable(bool enable) {
    SynthModule::enable(enable);
    if (!enable)
      hardReset();
  }
} // namespace vital
