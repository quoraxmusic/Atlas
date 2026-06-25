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

#pragma once

#include "synth_module.h"

#include <array>

namespace vital {

  class FrequencyShifterModule : public SynthModule {
    public:
      FrequencyShifterModule(std::string prefix = "");

      void init() override;
      void processWithInput(const poly_float* audio_in, int num_samples) override;
      void hardReset() override;
      void enable(bool enable) override;

      Processor* clone() const override { VITAL_ASSERT(false); return nullptr; }

    private:
      static constexpr int kHilbertTaps = 129;
      static constexpr int kHilbertDelay = kHilbertTaps / 2;
      static constexpr int kHilbertBufferSize = 256;

      void createHilbertCoefficients();
      poly_float getDelayedSample(int delay) const;
      poly_float processHilbert(poly_float audio);

      Output* shift_;
      Output* feedback_;
      Output* mix_;
      std::string prefix_;

      std::array<float, kHilbertTaps> hilbert_coefficients_;
      std::array<poly_float, kHilbertBufferSize> delay_buffer_;
      int write_index_;
      poly_float phase_;
      poly_float current_shift_;
      poly_float feedback_sample_;
      poly_float mix_amount_;

      JUCE_LEAK_DETECTOR(FrequencyShifterModule)
  };
} // namespace vital
