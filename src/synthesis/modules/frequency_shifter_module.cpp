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

#include "frequency_shifter_module.h"

#include "futils.h"
#include "poly_utils.h"

#include <cmath>

namespace vital {

  FrequencyShifterModule::FrequencyShifterModule(std::string prefix) :
      SynthModule(0, 1), shift_(nullptr), feedback_(nullptr), mix_(nullptr), prefix_(prefix),
      write_index_(0), phase_(0.0f), current_shift_(0.0f), feedback_sample_(0.0f), mix_amount_(1.0f) {
    createHilbertCoefficients();
    delay_buffer_.fill(0.0f);
  }

  void FrequencyShifterModule::init() {
    shift_ = createMonoModControl(prefix_ + "frequency_shifter_shift", true, true);
    feedback_ = createMonoModControl(prefix_ + "frequency_shifter_feedback");
    mix_ = createMonoModControl(prefix_ + "frequency_shifter_dry_wet");

    SynthModule::init();
  }

  void FrequencyShifterModule::createHilbertCoefficients() {
    for (int i = 0; i < kHilbertTaps; ++i) {
      int offset = i - kHilbertDelay;
      if (offset == 0 || (offset & 1) == 0) {
        hilbert_coefficients_[i] = 0.0f;
        continue;
      }

      float tap = 2.0f / (kPi * offset);
      float t = i / static_cast<float>(kHilbertTaps - 1);
      float window = 0.42f - 0.5f * std::cos(2.0f * kPi * t) + 0.08f * std::cos(4.0f * kPi * t);
      hilbert_coefficients_[i] = tap * window;
    }
  }

  poly_float FrequencyShifterModule::getDelayedSample(int delay) const {
    return delay_buffer_[(write_index_ - delay + kHilbertBufferSize) & (kHilbertBufferSize - 1)];
  }

  poly_float FrequencyShifterModule::processHilbert(poly_float audio) {
    delay_buffer_[write_index_] = audio;

    poly_float quadrature = 0.0f;
    for (int i = 0; i < kHilbertTaps; ++i)
      quadrature += getDelayedSample(i) * hilbert_coefficients_[i];

    poly_float real = getDelayedSample(kHilbertDelay);
    write_index_ = (write_index_ + 1) & (kHilbertBufferSize - 1);
    return real * utils::cos(phase_ * (2.0f * kPi)) - quadrature * utils::sin(phase_ * (2.0f * kPi));
  }

  void FrequencyShifterModule::processWithInput(const poly_float* audio_in, int num_samples) {
    static constexpr float kShiftSmoothingSeconds = 0.015f;

    SynthModule::process(num_samples);

    poly_float shift_smoothing = poly_float(1.0f) -
                                 futils::exp(poly_float(-1.0f) /
                                             (kShiftSmoothingSeconds * getSampleRate()));
    poly_float target_feedback = utils::clamp(feedback_->buffer[0], -0.95f, 0.95f);
    poly_float target_mix = utils::clamp(mix_->buffer[0], 0.0f, 1.0f);
    poly_float current_mix = mix_amount_;
    poly_float delta_mix = (target_mix - current_mix) * (1.0f / num_samples);
    poly_float* audio_out = output()->buffer;

    for (int i = 0; i < num_samples; ++i) {
      current_mix += delta_mix;
      current_shift_ += (shift_->buffer[i] - current_shift_) * shift_smoothing;
      phase_ = utils::mod(phase_ + current_shift_ * (1.0f / getSampleRate()));

      poly_float input = audio_in[i] + feedback_sample_ * target_feedback;
      poly_float shifted = processHilbert(input);

      feedback_sample_ = utils::clamp(shifted, -4.0f, 4.0f);
      audio_out[i] = utils::interpolate(audio_in[i], shifted, current_mix);
    }

    mix_amount_ = target_mix;
  }

  void FrequencyShifterModule::hardReset() {
    delay_buffer_.fill(0.0f);
    write_index_ = 0;
    phase_ = 0.0f;
    current_shift_ = 0.0f;
    feedback_sample_ = 0.0f;
    mix_amount_ = 1.0f;
  }

  void FrequencyShifterModule::enable(bool enable) {
    SynthModule::enable(enable);
    if (!enable)
      hardReset();
  }
} // namespace vital
