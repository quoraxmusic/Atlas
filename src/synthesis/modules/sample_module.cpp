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

#include "sample_module.h"

#include "synth_constants.h"

namespace vital {

  SampleModule::SampleModule() : SynthModule(kNumInputs, kNumOutputs), on_(nullptr) {
    sampler_ = new SampleSource();
    was_on_ = std::make_shared<bool>(true);
  }

  void SampleModule::init() {
    on_ = createBaseControl("sample_on");
    Value* random_phase = createBaseControl("sample_random_phase");
    Value* loop = createBaseControl("sample_loop");
    Value* bounce = createBaseControl("sample_bounce");
    Value* playback_mode = createBaseControl("sample_playback_mode");
    Value* keytrack = createBaseControl("sample_keytrack");
    Value* root_key = createBaseControl("sample_root_key");
    Value* transpose_quantize = createBaseControl("sample_transpose_quantize");
    Value* transpose_quantize_key = createBaseControl("sample_transpose_quantize_key");
    Value* transpose_quantize_scale = createBaseControl("sample_transpose_quantize_scale");
    Value* transpose_quantize_mode = createBaseControl("sample_transpose_quantize_mode");
    Output* start = createPolyModControl("sample_start");
    Output* end = createPolyModControl("sample_end");
    Output* loop_start = createPolyModControl("sample_loop_start");
    Output* loop_end = createPolyModControl("sample_loop_end");
    Output* loop_crossfade = createPolyModControl("sample_loop_crossfade");
    Output* low_cutoff = createPolyModControl("sample_low_cutoff", true, true);
    Output* high_cutoff = createPolyModControl("sample_high_cutoff", true, true);
    Output* transpose = createPolyModControl("sample_transpose");
    Output* tune = createPolyModControl("sample_tune");
    Output* level = createPolyModControl("sample_level", true, true);
    Output* pan = createPolyModControl("sample_pan");

    sampler_->useInput(input(kReset), SampleSource::kReset);
    sampler_->useInput(input(kRetrigger), SampleSource::kRetrigger);
    sampler_->useInput(input(kVoiceEvent), SampleSource::kVoiceEvent);
    sampler_->useInput(input(kMidi), SampleSource::kMidi);
    sampler_->useInput(input(kNoteCount), SampleSource::kNoteCount);
    sampler_->useInput(input(kNotePressed), SampleSource::kNotePressed);
    sampler_->plug(random_phase, SampleSource::kRandomPhase);
    sampler_->plug(keytrack, SampleSource::kKeytrack);
    sampler_->plug(root_key, SampleSource::kRootKey);
    sampler_->plug(loop, SampleSource::kLoop);
    sampler_->plug(bounce, SampleSource::kBounce);
    sampler_->plug(playback_mode, SampleSource::kPlaybackMode);
    sampler_->plug(start, SampleSource::kStart);
    sampler_->plug(end, SampleSource::kEnd);
    sampler_->plug(loop_start, SampleSource::kLoopStart);
    sampler_->plug(loop_end, SampleSource::kLoopEnd);
    sampler_->plug(loop_crossfade, SampleSource::kLoopCrossfade);
    sampler_->plug(low_cutoff, SampleSource::kLowCutoff);
    sampler_->plug(high_cutoff, SampleSource::kHighCutoff);
    sampler_->plug(transpose, SampleSource::kTranspose);
    sampler_->plug(transpose_quantize, SampleSource::kTransposeQuantize);
    sampler_->plug(transpose_quantize_key, SampleSource::kTransposeQuantizeKey);
    sampler_->plug(transpose_quantize_scale, SampleSource::kTransposeQuantizeScale);
    sampler_->plug(transpose_quantize_mode, SampleSource::kTransposeQuantizeMode);
    sampler_->plug(tune, SampleSource::kTune);
    sampler_->plug(level, SampleSource::kLevel);
    sampler_->plug(pan, SampleSource::kPan);
    sampler_->useOutput(output(kRaw), SampleSource::kRaw);
    sampler_->useOutput(output(kLevelled), SampleSource::kLevelled);

    addProcessor(sampler_);
    SynthModule::init();
  }

  void SampleModule::process(int num_samples) {
    bool on = on_->value();

    if (on)
      SynthModule::process(num_samples);
    else if (*was_on_) {
      output(kRaw)->clearBuffer();
      output(kLevelled)->clearBuffer();
      getPhaseOutput()->buffer[0] = 0.0f;
    }

    *was_on_ = on;
  }
} // namespace vital
