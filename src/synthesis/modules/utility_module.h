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

namespace vital {

  class UtilityModule : public SynthModule {
    public:
      UtilityModule(std::string prefix = "");
      virtual ~UtilityModule() = default;

      virtual void init() override;
      virtual void hardReset() override;
      virtual void processWithInput(const poly_float* audio_in, int num_samples) override;
      virtual Processor* clone() const override { return new UtilityModule(*this); }

    private:
      static mono_float onePoleCoefficient(mono_float midi_cutoff, int sample_rate);
      static force_inline poly_float processLowPass(poly_float input, poly_float& state, mono_float coefficient) {
        state += (input - state) * coefficient;
        return state;
      }

      std::string prefix_;
      Output* input_gain_;
      Output* output_gain_;
      Output* width_;
      Output* low_cutoff_;
      Output* high_cutoff_;
      Value* slope_;

      poly_float input_gain_smoothed_;
      poly_float output_gain_smoothed_;
      poly_float width_smoothed_;
      poly_float low_cut_state_1_;
      poly_float low_cut_state_2_;
      poly_float high_cut_state_1_;
      poly_float high_cut_state_2_;

      JUCE_LEAK_DETECTOR(UtilityModule)
  };
} // namespace vital
