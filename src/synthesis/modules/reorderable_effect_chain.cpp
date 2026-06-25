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

#include "reorderable_effect_chain.h"

#include "chorus_module.h"
#include "compressor_module.h"
#include "delay_module.h"
#include "dimension_expander_module.h"
#include "distortion_module.h"
#include "equalizer_module.h"
#include "frequency_shifter_module.h"
#include "flanger_module.h"
#include "filter_module.h"
#include "limiter_module.h"
#include "phase_shift_module.h"
#include "phaser_module.h"
#include "reverb_module.h"
#include "synth_strings.h"
#include "utility_module.h"

#include <algorithm>

namespace vital {

  class FilterFxModule : public SynthModule {
    public:
      enum {
        kAudio,
        kKeytrack,
        kNumInputs
      };

      FilterFxModule(const Output* keytrack, const std::string& prefix = "") : SynthModule(kNumInputs, 1) {
        filter_ = new FilterModule(prefix + "filter_fx");
        filter_->setCreateOnValue(false);
        filter_->setMono(true);
        addSubmodule(filter_);
        addProcessor(filter_);
        filter_->useOutput(output());
        filter_->plug(&input_, FilterModule::kAudio);
        filter_->plug(keytrack, FilterModule::kKeytrack);
      }

      void processWithInput(const poly_float* audio_in, int num_samples) override {
        utils::copyBuffer(input_.buffer, audio_in, num_samples);
        filter_->process(num_samples);
      }

      void setOversampleAmount(int oversampling) override {
        input_.ensureBufferSize(kMaxBufferSize * oversampling);
        SynthModule::setOversampleAmount(oversampling);
      }

    private:
      FilterModule* filter_;
      Output input_;
  };

  ReorderableEffectChain::ReorderableEffectChain(const Output* beats_per_second, const Output* keytrack,
                                                 std::string prefix) :
      vital::SynthModule(kNumInputs, 1), equalizer_memory_(nullptr),
      beats_per_second_(beats_per_second), keytrack_(keytrack), prefix_(prefix),
      post_effect_order_(nullptr), last_order_(0.0f) {
    for (int i = 0; i < constants::kNumEffects; ++i) {
      SynthModule* effect_module = createEffectModule(i);
      VITAL_ASSERT(effect_module);

      addSubmodule(effect_module);
      addProcessor(effect_module);
      effects_on_[i] = createBaseControl(prefix_ + strings::kEffectOrder[i] + "_on");
      effect_order_slots_[i] = createBaseControl(prefix_ + "effect_chain_slot_" + std::to_string(i + 1));
      effects_[i] = effect_module;
      effect_order_[i] = i;
    }

    post_effect_order_ = createBaseControl(prefix_ + "post_effect_order");
    last_order_ = utils::encodeOrderToFloat(effect_order_, constants::kNumReorderableEffects);
  }

  SynthModule* ReorderableEffectChain::createEffectModule(int index) {
    switch(index) {
      case constants::kChorus:
        return new ChorusModule(beats_per_second_, prefix_);
      case constants::kCompressor:
        return new CompressorModule(prefix_);
      case constants::kDelay:
        return new DelayModule(beats_per_second_, prefix_);
      case constants::kDistortion:
        return new DistortionModule(prefix_);
      case constants::kEq: {
        EqualizerModule* eq = new EqualizerModule(prefix_);
        equalizer_memory_ = eq->getAudioMemory();
        return eq;
      }
      case constants::kFlanger:
        return new FlangerModule(beats_per_second_, prefix_);
      case constants::kFilterFx:
        return new FilterFxModule(keytrack_, prefix_);
      case constants::kPhaser:
        return new PhaserModule(beats_per_second_, prefix_);
      case constants::kReverb:
        return new ReverbModule(prefix_);
      case constants::kUtility:
        return new UtilityModule(prefix_);
      case constants::kPhaseShift:
        return new PhaseShiftModule(prefix_);
      case constants::kLimiter:
        return new LimiterModule(prefix_);
      case constants::kFrequencyShifter:
        return new FrequencyShifterModule(prefix_);
      case constants::kDimensionExpander:
        return new DimensionExpanderModule(prefix_);
      default:
        return nullptr;
    }
  }

  void ReorderableEffectChain::process(int num_samples) {
    const poly_float* audio_in = input(kAudio)->source->buffer;
    processWithInput(audio_in, num_samples);
  }

  namespace {
    bool isValidEffectOrder(const int* order) {
      bool seen[constants::kNumEffects] = {};
      for (int i = 0; i < constants::kNumEffects; ++i) {
        if (order[i] < 0 || order[i] >= constants::kNumEffects || seen[order[i]])
          return false;
        seen[order[i]] = true;
      }
      return true;
    }
  }

  void ReorderableEffectChain::processWithInput(const poly_float* audio_in, int num_samples) {
    mono_float max_order = utils::factorial(constants::kNumReorderableEffects) - 1;
    mono_float float_order = utils::clamp(std::round(input(kOrder)->at(0)[0]), 0.0f, max_order);
    int slot_order[constants::kNumEffects];
    bool slots_are_default = true;
    for (int i = 0; i < constants::kNumEffects; ++i) {
      slot_order[i] = utils::iclamp(static_cast<int>(effect_order_slots_[i]->value() + 0.5f),
                                    0, constants::kNumEffects - 1);
      slots_are_default = slots_are_default && slot_order[i] == i;
    }

    if (!slots_are_default && isValidEffectOrder(slot_order)) {
      for (int i = 0; i < constants::kNumEffects; ++i)
        effect_order_[i] = slot_order[i];
    }
    else {
      if (float_order != last_order_)
        utils::decodeFloatToOrder(effect_order_, float_order, constants::kNumReorderableEffects);
      for (int i = constants::kNumReorderableEffects; i < constants::kNumEffects; ++i)
        effect_order_[i] = i;

      if (post_effect_order_->value() >= 0.5f)
        std::swap(effect_order_[constants::kFrequencyShifter], effect_order_[constants::kLimiter]);
    }
    last_order_ = float_order;

    for (int i = 0; i < constants::kNumEffects; ++i) {
      VITAL_ASSERT(utils::isFinite(audio_in, num_samples));

      int index = effect_order_[i];
      bool on = effects_on_[index]->value();
      bool enabled = effects_[index]->enabled();
      if (on != enabled)
        effects_[index]->enable(on);

      if (on) {
        effects_[index]->processWithInput(audio_in, num_samples);
        audio_in = effects_[index]->output(0)->buffer;
      }
    }

    VITAL_ASSERT(utils::isFinite(audio_in, num_samples));
    utils::copyBuffer(output()->buffer, audio_in, num_samples);
  }

  void ReorderableEffectChain::hardReset() {
    for (int i = 0; i < constants::kNumEffects; ++i)
      effects_[i]->hardReset();
  }

  void ReorderableEffectChain::correctToTime(double seconds) {
    for (int i = 0; i < constants::kNumEffects; ++i)
      effects_[i]->correctToTime(seconds);
  }
} // namespace vital
