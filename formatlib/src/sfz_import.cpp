#include "velora/format/sfz_import.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>

#include "layout.hpp"
#include "parser/Parser.h"
#include "parser/ParserListener.h"

namespace velora::format {
namespace fs = std::filesystem;

const char* to_string(SfzIssueKind k) {
  switch (k) {
    case SfzIssueKind::kUnsupportedOpcode: return "unsupported opcode";
    case SfzIssueKind::kUnsupportedHeader: return "unsupported header";
    case SfzIssueKind::kUnsupportedValue: return "unsupported value";
    case SfzIssueKind::kInvalidValue: return "invalid value";
    case SfzIssueKind::kParse: return "parse error";
    case SfzIssueKind::kMissingSample: return "missing sample";
    case SfzIssueKind::kMissingSampleOpcode: return "region without sample";
  }
  return "?";
}

bool is_unsupported_feature(SfzIssueKind k) {
  return k == SfzIssueKind::kUnsupportedOpcode || k == SfzIssueKind::kUnsupportedHeader ||
         k == SfzIssueKind::kUnsupportedValue;
}

const std::vector<SfzOpcodeEntry>& sfz_opcode_table() {
  using C = OpcodeClass;
  static const std::vector<SfzOpcodeEntry> t = {
      {"sample", C::kSupported, "required in every region"},
      {"default_path", C::kSupported, "<control> only"},
      {"lokey", C::kSupported, "note number or name"},
      {"hikey", C::kSupported, ""},
      {"key", C::kSupported, "lokey = hikey = pitch_keycenter"},
      {"lovel", C::kSupported, ""},
      {"hivel", C::kSupported, ""},
      {"pitch_keycenter", C::kSupported, ""},
      {"tune", C::kSupported, "cents"},
      {"transpose", C::kSupported, "semitones"},
      {"volume", C::kSupported, "dB"},
      {"pan", C::kSupported, "-100..100"},
      {"offset", C::kSupported, "frames"},
      {"seq_length", C::kSupported, "round robin"},
      {"seq_position", C::kSupported, "round robin"},
      {"lorand", C::kSupported, ""},
      {"hirand", C::kSupported, ""},
      {"loop_mode", C::kSupported, "no_loop, one_shot, loop_continuous, loop_sustain"},
      {"loop_start", C::kSupported, "frames"},
      {"loop_end", C::kSupported, "frames, inclusive"},
      {"ampeg_delay", C::kSupported, "s"},
      {"ampeg_start", C::kSupported, "%"},
      {"ampeg_attack", C::kSupported, "s"},
      {"ampeg_hold", C::kSupported, "s"},
      {"ampeg_decay", C::kSupported, "s"},
      {"ampeg_sustain", C::kSupported, "%"},
      {"ampeg_release", C::kSupported, "s"},
      {"trigger", C::kSupported, "attack, release, first, legato, release_key (Story 13.3)"},
      {"end", C::kSupported, "frames, exclusive: last played frame = min(end, length) - 1; 0 disables the region (Story 13.3)"},
      {"amp_veltrack", C::kSupported, "percent -100..100 (Story 13.4)"},
      {"amp_velcurve_<N>", C::kSupported, "N = velocity 0..127, gain 0..1, linear between points, at most 32 points per region (Story 13.4)"},
      {"amplitude", C::kSupported, "percent 0..10000 (Story 13.4)"},
      {"global_amplitude", C::kSupported, "percent, multiplies amplitude (Story 13.4)"},
      {"master_amplitude", C::kSupported, "percent, multiplies amplitude (Story 13.4)"},
      {"group_amplitude", C::kSupported, "percent, multiplies amplitude (Story 13.4)"},
      {"global_volume", C::kSupported, "dB, added to volume (Story 13.4)"},
      {"master_volume", C::kSupported, "dB, added to volume (Story 13.4)"},
      {"group_volume", C::kSupported, "dB, added to volume (Story 13.4)"},
      {"amp_keytrack", C::kSupported, "dB per key -96..12 (Story 13.4)"},
      {"amp_keycenter", C::kSupported, "note number or name (Story 13.4)"},
      {"ampeg_vel2attack", C::kSupported, "s at velocity 127 (Story 13.4)"},
      {"ampeg_vel2decay", C::kSupported, "s at velocity 127 (Story 13.4)"},
      {"ampeg_vel2sustain", C::kSupported, "% at velocity 127 (Story 13.4)"},
      {"ampeg_vel2release", C::kSupported, "s at velocity 127 (Story 13.4)"},
      {"sw_lokey", C::kSupported, "keyswitch range low (Story 5.1)"},
      {"sw_hikey", C::kSupported, "keyswitch range high (Story 5.1)"},
      {"sw_last", C::kSupported, "(Story 5.1)"},
      {"sw_down", C::kSupported, "(Story 5.1)"},
      {"sw_up", C::kSupported, "(Story 5.1)"},
      {"sw_previous", C::kSupported, "(Story 5.1)"},
      {"sw_default", C::kSupported, "(Story 5.1)"},
      {"xfin_locc<N>", C::kSupported, "N = controller 0..127, up to 4 per region (Story 5.1)"},
      {"xfin_hicc<N>", C::kSupported, "(Story 5.1)"},
      {"xfout_locc<N>", C::kSupported, "(Story 5.1)"},
      {"xfout_hicc<N>", C::kSupported, "(Story 5.1)"},
      {"xf_cccurve", C::kSupported, "gain, power (Story 5.1)"},
      {"fil_type", C::kSupported, "lpf_1p, lpf_2p, hpf_1p, hpf_2p, bpf_2p (Story 5.1)"},
      {"cutoff", C::kSupported, "Hz; the filter exists only when cutoff is given (Story 5.1)"},
      {"resonance", C::kSupported, "dB (Story 5.1)"},
      // Story 13.8: per-voice filter tuning and a second filter (see the importer comment in sfz_import.hpp).
      {"filtype", C::kSupported, "alias of fil_type (Story 13.8)"},
      {"fil1_type", C::kSupported, "alias of fil_type (Story 13.8)"},
      {"fil2_type", C::kSupported, "type of the second filter, which exists only when cutoff2 is given (Story 13.8)"},
      {"cutoff1", C::kSupported, "alias of cutoff (Story 13.8)"},
      {"cutoff2", C::kSupported, "Hz, the second filter, in series after the first; needs a first filter (Story 13.8)"},
      {"resonance1", C::kSupported, "alias of resonance (Story 13.8)"},
      {"resonance2", C::kSupported, "dB, the second filter (Story 13.8)"},
      {"fil_keytrack", C::kSupported, "cents per key 0..1200 from fil_keycenter (Story 13.8)"},
      {"fil1_keytrack", C::kSupported, "alias of fil_keytrack (Story 13.8)"},
      {"fil2_keytrack", C::kSupported, "fil_keytrack of the second filter (Story 13.8)"},
      {"fil_keycenter", C::kSupported, "note number or name, default 60 (Story 13.8)"},
      {"fil1_keycenter", C::kSupported, "alias of fil_keycenter (Story 13.8)"},
      {"fil2_keycenter", C::kSupported, "fil_keycenter of the second filter (Story 13.8)"},
      {"fil_veltrack", C::kSupported, "cents at velocity 127, -12000..12000 (Story 13.8)"},
      {"fil1_veltrack", C::kSupported, "alias of fil_veltrack (Story 13.8)"},
      {"fil2_veltrack", C::kSupported, "fil_veltrack of the second filter (Story 13.8)"},
      {"cutoff_oncc<N>", C::kSupported, "cents added at voice start, -12000..12000; N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141 (Story 13.8)"},
      {"cutoff_cc<N>", C::kSupported, "alias of cutoff_oncc<N> (Story 13.8)"},
      {"cutoff_curvecc<N>", C::kSupported, "curve index (Story 13.8)"},
      {"cutoff1_oncc<N>", C::kSupported, "alias of cutoff_oncc<N> (Story 13.8)"},
      {"cutoff1_cc<N>", C::kSupported, "alias of cutoff_oncc<N> (Story 13.8)"},
      {"cutoff1_curvecc<N>", C::kSupported, "curve index (Story 13.8)"},
      {"cutoff2_oncc<N>", C::kSupported, "cutoff_oncc<N> of the second filter (Story 13.8)"},
      {"cutoff2_cc<N>", C::kSupported, "alias of cutoff2_oncc<N> (Story 13.8)"},
      {"cutoff2_curvecc<N>", C::kSupported, "curve index (Story 13.8)"},
      {"resonance_oncc<N>", C::kSupported, "dB added at voice start, 0..96; N as above (Story 13.8)"},
      {"resonance_cc<N>", C::kSupported, "alias of resonance_oncc<N> (Story 13.8)"},
      {"resonance_curvecc<N>", C::kSupported, "curve index (Story 13.8)"},
      {"resonance1_oncc<N>", C::kSupported, "alias of resonance_oncc<N> (Story 13.8)"},
      {"resonance1_cc<N>", C::kSupported, "alias of resonance_oncc<N> (Story 13.8)"},
      {"resonance1_curvecc<N>", C::kSupported, "curve index (Story 13.8)"},
      {"resonance2_oncc<N>", C::kSupported, "resonance_oncc<N> of the second filter (Story 13.8)"},
      {"resonance2_cc<N>", C::kSupported, "alias of resonance2_oncc<N> (Story 13.8)"},
      {"resonance2_curvecc<N>", C::kSupported, "curve index (Story 13.8)"},
      {"label_cc<N>", C::kAllowListed, "controller name"},
      {"label_key<N>", C::kAllowListed, "key name"},
      {"global_label", C::kAllowListed, ""},
      {"master_label", C::kAllowListed, ""},
      {"group_label", C::kAllowListed, ""},
      {"region_label", C::kAllowListed, ""},
      {"sw_label", C::kAllowListed, "keyswitch name"},
      {"image", C::kAllowListed, "instrument picture"},
      {"hint_*", C::kAllowListed, "editor hints"},
      // Story 13.5: CC gates, CC defaults and CC modulation read once when a voice starts (see the importer comment in sfz_import.hpp).
      {"locc<N>", C::kSupported, "N = controller 0..127, CC units; the region plays while the part's CC value is in [locc, hicc] (Story 13.5)"},
      {"hicc<N>", C::kSupported, "hicc below 127 covers values up to hicc + 1, exclusive (sfizz fill-gap) (Story 13.5)"},
      {"lohdcc<N>", C::kSupported, "normalized 0..1 (Story 13.5)"},
      {"hihdcc<N>", C::kSupported, "normalized 0..1 (Story 13.5)"},
      {"lorealcc<N>", C::kSupported, "alias of lohdcc<N> (Story 13.5)"},
      {"hirealcc<N>", C::kSupported, "alias of hihdcc<N> (Story 13.5)"},
      {"set_cc<N>", C::kSupported, "<control> only: the part's initial value of controller N (0..127) (Story 13.5)"},
      {"set_hdcc<N>", C::kSupported, "<control> only, normalized 0..1 (Story 13.5)"},
      {"set_realcc<N>", C::kSupported, "alias of set_hdcc<N> (Story 13.5)"},
      {"amplitude_oncc<N>", C::kSupported, "percent 0..10000, gain x (depth/100 x curve); N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141 (Story 13.5)"},
      {"amplitude_cc<N>", C::kSupported, "alias of amplitude_oncc<N> (Story 13.5)"},
      {"amplitude_curvecc<N>", C::kSupported, "curve index: a <curve> block or a predefined curve 0..6 (Story 13.5)"},
      {"volume_oncc<N>", C::kSupported, "dB; N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141 (Story 13.5)"},
      {"volume_cc<N>", C::kSupported, "alias of volume_oncc<N> (Story 13.5)"},
      {"volume_curvecc<N>", C::kSupported, "curve index: a <curve> block or a predefined curve 0..6 (Story 13.5)"},
      {"gain_oncc<N>", C::kSupported, "dB, alias of volume; N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141 (Story 13.5)"},
      {"gain_cc<N>", C::kSupported, "alias of gain_oncc<N> (Story 13.5)"},
      {"gain_curvecc<N>", C::kSupported, "curve index: a <curve> block or a predefined curve 0..6 (Story 13.5)"},
      {"pan_oncc<N>", C::kSupported, "percent; N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141 (Story 13.5)"},
      {"pan_cc<N>", C::kSupported, "alias of pan_oncc<N> (Story 13.5)"},
      {"pan_curvecc<N>", C::kSupported, "curve index: a <curve> block or a predefined curve 0..6 (Story 13.5)"},
      {"tune_oncc<N>", C::kSupported, "cents; N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141 (Story 13.5)"},
      {"tune_cc<N>", C::kSupported, "alias of tune_oncc<N> (Story 13.5)"},
      {"tune_curvecc<N>", C::kSupported, "curve index: a <curve> block or a predefined curve 0..6 (Story 13.5)"},
      {"pitch_oncc<N>", C::kSupported, "cents, alias of tune; N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141 (Story 13.5)"},
      {"pitch_cc<N>", C::kSupported, "alias of pitch_oncc<N> (Story 13.5)"},
      {"pitch_curvecc<N>", C::kSupported, "curve index: a <curve> block or a predefined curve 0..6 (Story 13.5)"},
      {"offset_oncc<N>", C::kSupported, "frames: depth x CC value, no curve; N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141 (Story 13.5)"},
      {"offset_cc<N>", C::kSupported, "alias of offset_oncc<N> (Story 13.5)"},
      {"offset_curvecc<N>", C::kSupported, "accepted and ignored, as sfizz: offset is not curve-modulated (ignored_opcode warning) (Story 13.5)"},
      {"ampeg_delay_oncc<N>", C::kSupported, "s, added at voice start; N as above (Story 13.5)"},
      {"ampeg_delay_cc<N>", C::kSupported, "alias of ampeg_delay_oncc<N> (Story 13.5)"},
      {"ampeg_delaycc<N>", C::kSupported, "SFZ 1 alias of ampeg_delay_oncc<N> (Story 13.5)"},
      {"ampeg_delay_curvecc<N>", C::kSupported, "curve index (Story 13.5)"},
      {"ampeg_start_oncc<N>", C::kSupported, "%, added at voice start; N as above (Story 13.5)"},
      {"ampeg_start_cc<N>", C::kSupported, "alias of ampeg_start_oncc<N> (Story 13.5)"},
      {"ampeg_startcc<N>", C::kSupported, "SFZ 1 alias of ampeg_start_oncc<N> (Story 13.5)"},
      {"ampeg_start_curvecc<N>", C::kSupported, "curve index (Story 13.5)"},
      {"ampeg_attack_oncc<N>", C::kSupported, "s, added at voice start; N as above (Story 13.5)"},
      {"ampeg_attack_cc<N>", C::kSupported, "alias of ampeg_attack_oncc<N> (Story 13.5)"},
      {"ampeg_attackcc<N>", C::kSupported, "SFZ 1 alias of ampeg_attack_oncc<N> (Story 13.5)"},
      {"ampeg_attack_curvecc<N>", C::kSupported, "curve index (Story 13.5)"},
      {"ampeg_hold_oncc<N>", C::kSupported, "s, added at voice start; N as above (Story 13.5)"},
      {"ampeg_hold_cc<N>", C::kSupported, "alias of ampeg_hold_oncc<N> (Story 13.5)"},
      {"ampeg_holdcc<N>", C::kSupported, "SFZ 1 alias of ampeg_hold_oncc<N> (Story 13.5)"},
      {"ampeg_hold_curvecc<N>", C::kSupported, "curve index (Story 13.5)"},
      {"ampeg_decay_oncc<N>", C::kSupported, "s, added at voice start; N as above (Story 13.5)"},
      {"ampeg_decay_cc<N>", C::kSupported, "alias of ampeg_decay_oncc<N> (Story 13.5)"},
      {"ampeg_decaycc<N>", C::kSupported, "SFZ 1 alias of ampeg_decay_oncc<N> (Story 13.5)"},
      {"ampeg_decay_curvecc<N>", C::kSupported, "curve index (Story 13.5)"},
      {"ampeg_sustain_oncc<N>", C::kSupported, "%, added at voice start; N as above (Story 13.5)"},
      {"ampeg_sustain_cc<N>", C::kSupported, "alias of ampeg_sustain_oncc<N> (Story 13.5)"},
      {"ampeg_sustaincc<N>", C::kSupported, "SFZ 1 alias of ampeg_sustain_oncc<N> (Story 13.5)"},
      {"ampeg_sustain_curvecc<N>", C::kSupported, "curve index (Story 13.5)"},
      {"ampeg_release_oncc<N>", C::kSupported, "s, added at voice start; N as above (Story 13.5)"},
      {"ampeg_release_cc<N>", C::kSupported, "alias of ampeg_release_oncc<N> (Story 13.5)"},
      {"ampeg_releasecc<N>", C::kSupported, "SFZ 1 alias of ampeg_release_oncc<N> (Story 13.5)"},
      {"ampeg_release_curvecc<N>", C::kSupported, "curve index (Story 13.5)"},
      // Story 13.5: ampeg_dynamic makes sfizz re-read ampeg_* (including its _oncc values) every chunk. This importer reads the CC once at voice start, so the
      // flag is no longer inert: a region with ampeg_dynamic=1 and any ampeg_*_oncc<N> is rejected (unsupported_value). Without an _oncc it changes nothing
      // (ampeg_vel2* reads the trigger velocity, constant for the life of the voice, Story 13.4), so VSCO2's ampeg_dynamic=1 imports as before.
      // Story 13.6: choke groups and polyphony limits, evaluated when a voice starts (see the importer comment in sfz_import.hpp).
      {"group", C::kSupported, "integer, the region's choke / polyphony group (default 0); a <group> header is unaffected (Story 13.6)"},
      {"polyphony_group", C::kSupported, "alias of group (Story 13.6)"},
      {"off_by", C::kSupported, "integer: the voices of every group with this number are cut when this region starts a voice (Story 13.6)"},
      {"offby", C::kSupported, "alias of off_by (Story 13.6)"},
      {"off_mode", C::kSupported, "fast (6 ms, the default), normal (the voice's own ampeg_release), time (off_time) (Story 13.6)"},
      {"offmode", C::kSupported, "alias of off_mode (Story 13.6)"},
      {"off_time", C::kSupported, "s 0..100, implies off_mode=time (sfizz); times below 6 ms behave as 6 ms (Story 13.6)"},
      {"polyphony", C::kSupported, "1..255 voices; in <region>, or in a header together with a non-zero group set at that header or a broader one (the group's limit); otherwise a region-set limit, rejected (Story 13.6)"},
      {"note_polyphony", C::kSupported, "1..255 voices of the group for one note (Story 13.6)"},
      {"note_selfmask", C::kSupported, "on or mask (default: a quieter retrigger does not cut a louder voice) or off (Story 13.6)"},
      {"ampeg_dynamic", C::kSupported, "flag; rejected together with ampeg_*_oncc<N> (continuous envelope modulation is not implemented), otherwise no effect (Story 13.5)"},
      // Story 13.7: pitch bend and controller-triggered voices (see the importer comment in sfz_import.hpp).
      {"bend_up", C::kSupported, "cents -12000..12000 at full wheel travel up (default 200); setting any bend opcode makes the region follow the pitch wheel (Story 13.7)"},
      {"bendup", C::kSupported, "alias of bend_up (Story 13.7)"},
      {"bend_down", C::kSupported, "cents -12000..12000 at full wheel travel down (default -200; a positive value bends up at the bottom of the wheel) (Story 13.7)"},
      {"benddown", C::kSupported, "alias of bend_down (Story 13.7)"},
      {"bend_step", C::kSupported, "cents 1..12000: the bend is quantised to multiples of this (default 1 = none) (Story 13.7)"},
      {"bend_smooth", C::kSupported, "ms 0..65535: length of the linear ramp to a new wheel position (default 0) (Story 13.7)"},
      {"on_locc<N>", C::kSupported, "N = controller 0..127, CC units: the region starts a voice when the controller moves into [on_locc, on_hicc] (Story 13.7)"},
      {"on_hicc<N>", C::kSupported, "on_hicc below 127 covers values up to on_hicc + 1, exclusive (sfizz fill-gap) (Story 13.7)"},
      {"start_locc<N>", C::kSupported, "alias of on_locc<N> (Story 13.7)"},
      {"start_hicc<N>", C::kSupported, "alias of on_hicc<N> (Story 13.7)"},
      // Story 13.9: region EQ and the dedicated LFOs / EGs (see _bmad-output/planning-artifacts/sfz-modulation-design.md).
      {"eq1_freq", C::kSupported, "EQ band 1, Hz (Story 13.9)"},
      {"eq1_bw", C::kSupported, "EQ band 1, octaves 0.001..4 (Story 13.9)"},
      {"eq1_gain", C::kSupported, "EQ band 1, dB -96..96 (Story 13.9)"},
      {"eq1_type", C::kSupported, "peak, lshelf, hshelf (Story 13.9)"},
      {"eq1_vel2freq", C::kSupported, "Hz added at velocity 127 (Story 13.9)"},
      {"eq1_veltofreq", C::kSupported, "alias of eq1_vel2freq (Story 13.9)"},
      {"eq1_vel2gain", C::kSupported, "dB added at velocity 127 (Story 13.9)"},
      {"eq1_veltogain", C::kSupported, "alias of eq1_vel2gain (Story 13.9)"},
      {"eq1_freq_oncc<N>", C::kSupported, "Hz added at voice start; N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141, at most 4 per target (Story 13.9)"},
      {"eq1_freq_cc<N>", C::kSupported, "alias of eq1_freq_oncc<N> (Story 13.9)"},
      {"eq1_freqcc<N>", C::kSupported, "SFZ 1 alias of eq1_freq_oncc<N> (Story 13.9)"},
      {"eq1_freq_curvecc<N>", C::kSupported, "curve index (Story 13.9)"},
      {"eq1_bw_oncc<N>", C::kSupported, "octaves added at voice start; N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141, at most 4 per target (Story 13.9)"},
      {"eq1_bw_cc<N>", C::kSupported, "alias of eq1_bw_oncc<N> (Story 13.9)"},
      {"eq1_bwcc<N>", C::kSupported, "SFZ 1 alias of eq1_bw_oncc<N> (Story 13.9)"},
      {"eq1_bw_curvecc<N>", C::kSupported, "curve index (Story 13.9)"},
      {"eq1_gain_oncc<N>", C::kSupported, "dB added at voice start; N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141, at most 4 per target (Story 13.9)"},
      {"eq1_gain_cc<N>", C::kSupported, "alias of eq1_gain_oncc<N> (Story 13.9)"},
      {"eq1_gaincc<N>", C::kSupported, "SFZ 1 alias of eq1_gain_oncc<N> (Story 13.9)"},
      {"eq1_gain_curvecc<N>", C::kSupported, "curve index (Story 13.9)"},
      {"eq2_freq", C::kSupported, "EQ band 2, Hz (Story 13.9)"},
      {"eq2_bw", C::kSupported, "EQ band 2, octaves 0.001..4 (Story 13.9)"},
      {"eq2_gain", C::kSupported, "EQ band 2, dB -96..96 (Story 13.9)"},
      {"eq2_type", C::kSupported, "peak, lshelf, hshelf (Story 13.9)"},
      {"eq2_vel2freq", C::kSupported, "Hz added at velocity 127 (Story 13.9)"},
      {"eq2_veltofreq", C::kSupported, "alias of eq2_vel2freq (Story 13.9)"},
      {"eq2_vel2gain", C::kSupported, "dB added at velocity 127 (Story 13.9)"},
      {"eq2_veltogain", C::kSupported, "alias of eq2_vel2gain (Story 13.9)"},
      {"eq2_freq_oncc<N>", C::kSupported, "Hz added at voice start; N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141, at most 4 per target (Story 13.9)"},
      {"eq2_freq_cc<N>", C::kSupported, "alias of eq2_freq_oncc<N> (Story 13.9)"},
      {"eq2_freqcc<N>", C::kSupported, "SFZ 1 alias of eq2_freq_oncc<N> (Story 13.9)"},
      {"eq2_freq_curvecc<N>", C::kSupported, "curve index (Story 13.9)"},
      {"eq2_bw_oncc<N>", C::kSupported, "octaves added at voice start; N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141, at most 4 per target (Story 13.9)"},
      {"eq2_bw_cc<N>", C::kSupported, "alias of eq2_bw_oncc<N> (Story 13.9)"},
      {"eq2_bwcc<N>", C::kSupported, "SFZ 1 alias of eq2_bw_oncc<N> (Story 13.9)"},
      {"eq2_bw_curvecc<N>", C::kSupported, "curve index (Story 13.9)"},
      {"eq2_gain_oncc<N>", C::kSupported, "dB added at voice start; N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141, at most 4 per target (Story 13.9)"},
      {"eq2_gain_cc<N>", C::kSupported, "alias of eq2_gain_oncc<N> (Story 13.9)"},
      {"eq2_gaincc<N>", C::kSupported, "SFZ 1 alias of eq2_gain_oncc<N> (Story 13.9)"},
      {"eq2_gain_curvecc<N>", C::kSupported, "curve index (Story 13.9)"},
      {"eq3_freq", C::kSupported, "EQ band 3, Hz (Story 13.9)"},
      {"eq3_bw", C::kSupported, "EQ band 3, octaves 0.001..4 (Story 13.9)"},
      {"eq3_gain", C::kSupported, "EQ band 3, dB -96..96 (Story 13.9)"},
      {"eq3_type", C::kSupported, "peak, lshelf, hshelf (Story 13.9)"},
      {"eq3_vel2freq", C::kSupported, "Hz added at velocity 127 (Story 13.9)"},
      {"eq3_veltofreq", C::kSupported, "alias of eq3_vel2freq (Story 13.9)"},
      {"eq3_vel2gain", C::kSupported, "dB added at velocity 127 (Story 13.9)"},
      {"eq3_veltogain", C::kSupported, "alias of eq3_vel2gain (Story 13.9)"},
      {"eq3_freq_oncc<N>", C::kSupported, "Hz added at voice start; N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141, at most 4 per target (Story 13.9)"},
      {"eq3_freq_cc<N>", C::kSupported, "alias of eq3_freq_oncc<N> (Story 13.9)"},
      {"eq3_freqcc<N>", C::kSupported, "SFZ 1 alias of eq3_freq_oncc<N> (Story 13.9)"},
      {"eq3_freq_curvecc<N>", C::kSupported, "curve index (Story 13.9)"},
      {"eq3_bw_oncc<N>", C::kSupported, "octaves added at voice start; N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141, at most 4 per target (Story 13.9)"},
      {"eq3_bw_cc<N>", C::kSupported, "alias of eq3_bw_oncc<N> (Story 13.9)"},
      {"eq3_bwcc<N>", C::kSupported, "SFZ 1 alias of eq3_bw_oncc<N> (Story 13.9)"},
      {"eq3_bw_curvecc<N>", C::kSupported, "curve index (Story 13.9)"},
      {"eq3_gain_oncc<N>", C::kSupported, "dB added at voice start; N = controller 0..127 or pseudo-CC 131, 135, 136, 137, 140, 141, at most 4 per target (Story 13.9)"},
      {"eq3_gain_cc<N>", C::kSupported, "alias of eq3_gain_oncc<N> (Story 13.9)"},
      {"eq3_gaincc<N>", C::kSupported, "SFZ 1 alias of eq3_gain_oncc<N> (Story 13.9)"},
      {"eq3_gain_curvecc<N>", C::kSupported, "curve index (Story 13.9)"},
      {"amplfo_depth", C::kSupported, "amplitude LFO, dB; the LFO exists only with a depth (Story 13.9)"},
      {"amplfo_freq", C::kSupported, "Hz 0..100 (Story 13.9)"},
      {"amplfo_delay", C::kSupported, "s 0..30 (Story 13.9)"},
      {"amplfo_fade", C::kSupported, "s 0..30 (Story 13.9)"},
      {"amplfo_wave", C::kSupported, "0 triangle, 1 sine (default), 2 pulse75, 3 square, 4 pulse25, 5 pulse12.5, 6 ramp, 7 saw (Story 13.9)"},
      {"pitchlfo_depth", C::kSupported, "pitch LFO, cents; the LFO exists only with a depth (Story 13.9)"},
      {"pitchlfo_freq", C::kSupported, "Hz 0..100 (Story 13.9)"},
      {"pitchlfo_delay", C::kSupported, "s 0..30 (Story 13.9)"},
      {"pitchlfo_fade", C::kSupported, "s 0..30 (Story 13.9)"},
      {"pitchlfo_wave", C::kSupported, "0 triangle, 1 sine (default), 2 pulse75, 3 square, 4 pulse25, 5 pulse12.5, 6 ramp, 7 saw (Story 13.9)"},
      {"fillfo_depth", C::kSupported, "filter LFO, cents on the first filter's cutoff; the LFO exists only with a depth (Story 13.9)"},
      {"fillfo_freq", C::kSupported, "Hz 0..100 (Story 13.9)"},
      {"fillfo_delay", C::kSupported, "s 0..30 (Story 13.9)"},
      {"fillfo_fade", C::kSupported, "s 0..30 (Story 13.9)"},
      {"fillfo_wave", C::kSupported, "0 triangle, 1 sine (default), 2 pulse75, 3 square, 4 pulse25, 5 pulse12.5, 6 ramp, 7 saw (Story 13.9)"},
      {"pitcheg_delay", C::kSupported, "s (Story 13.9)"},
      {"pitcheg_start", C::kSupported, "% (Story 13.9)"},
      {"pitcheg_attack", C::kSupported, "s (Story 13.9)"},
      {"pitcheg_hold", C::kSupported, "s (Story 13.9)"},
      {"pitcheg_decay", C::kSupported, "s (Story 13.9)"},
      {"pitcheg_sustain", C::kSupported, "% (Story 13.9)"},
      {"pitcheg_release", C::kSupported, "s (Story 13.9)"},
      {"pitcheg_depth", C::kSupported, "pitch EG, cents; the EG exists only with a depth or vel2depth (Story 13.9)"},
      {"pitcheg_vel2depth", C::kSupported, "cents added at velocity 127 (Story 13.9)"},
      {"pitcheg_veltodepth", C::kSupported, "alias of pitcheg_vel2depth (Story 13.9)"},
      {"fileg_delay", C::kSupported, "s (Story 13.9)"},
      {"fileg_start", C::kSupported, "% (Story 13.9)"},
      {"fileg_attack", C::kSupported, "s (Story 13.9)"},
      {"fileg_hold", C::kSupported, "s (Story 13.9)"},
      {"fileg_decay", C::kSupported, "s (Story 13.9)"},
      {"fileg_sustain", C::kSupported, "% (Story 13.9)"},
      {"fileg_release", C::kSupported, "s (Story 13.9)"},
      {"fileg_depth", C::kSupported, "filter EG, cents on the first filter's cutoff; the EG exists only with a depth or vel2depth (Story 13.9)"},
      {"fileg_vel2depth", C::kSupported, "cents added at velocity 127 (Story 13.9)"},
      {"fileg_veltodepth", C::kSupported, "alias of fileg_vel2depth (Story 13.9)"},
  };
  return t;
}

bool opcode_matches(const std::string& pattern, const std::string& name) {
  auto starts = [&](const std::string& prefix) {
    return name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0;
  };
  if (pattern.size() > 3 && pattern.compare(pattern.size() - 3, 3, "<N>") == 0) {
    const std::string prefix = pattern.substr(0, pattern.size() - 3);
    if (!starts(prefix)) return false;
    const std::size_t digits = name.size() - prefix.size();
    if (digits < 1 || digits > 3) return false;
    for (std::size_t i = prefix.size(); i < name.size(); ++i)
      if (name[i] < '0' || name[i] > '9') return false;
    return true;
  }
  if (!pattern.empty() && pattern.back() == '*') return starts(pattern.substr(0, pattern.size() - 1));
  return name == pattern;
}

OpcodeClass classify_opcode(const std::string& name) {
  for (const auto& e : sfz_opcode_table())
    if (opcode_matches(e.pattern, name)) return e.cls;
  return OpcodeClass::kUnsupported;
}

std::string SfzIssue::to_string() const {
  std::string s = file;
  if (line) s += ":" + std::to_string(line);
  return s + ": " + message;
}

namespace {

struct Op {
  std::string name, value, file;
  std::size_t line = 0;
};
using Level = std::vector<Op>;

// State gathered while applying a region's opcodes that does not fit in Region itself.
struct XfPart {
  std::uint8_t lo = 0, hi = 0;
};
struct Acc {
  SfzLoopSpec ls;
  bool cutoff_set = false, type_set = false;
  FilterType type = FilterType::kLpf2p;
  std::map<int, XfPart> in, out;  // by controller number, ascending
  bool never_note = false;        // hikey=-1 (the last hikey wins, so a later valid hikey clears it)
  const Op* never_op = nullptr;   // the hikey=-1 opcode (for the ignored_values line of a region that is dropped)
  const Op* type_op = nullptr;  // fil_type / resonance as given (for ignored_opcode warnings)
  const Op* res_op = nullptr;
  // Story 13.4. amp_velcurve_N points (last wins, ascending by N); group/master/global volume and amplitude, folded into the region at the end.
  std::map<int, float> velcurve;
  bool vol_set = false;
  double vol_extra[3] = {0, 0, 0};  // group_volume, master_volume, global_volume
  bool amp_set = false;
  double amp_mult[3] = {100, 100, 100};  // group_amplitude, master_amplitude, global_amplitude (percent)
  // Story 13.5. CC gates (by controller; a missing end is the sfizz default 0 / 127), CC modulation entries (by target and controller), ampeg_dynamic.
  struct GateAcc {
    bool lo_set = false, hi_set = false;
    float lo = 0, hi = 127;
  };
  struct ModAcc {
    bool depth_set = false;  // a *_curvecc alone is no connection (sfizz's would have depth 0 and, on amplitude, silence the region: not reproduced)
    float depth = 0;
    int curve = 0;
  };
  std::map<int, GateAcc> gates;
  std::map<int, GateAcc> trigs;  // Story 13.7: on_locc / on_hicc by controller
  std::map<std::pair<int, int>, ModAcc> mods;
  bool dynamic = false;
  // Story 13.6. group / polyphony and the header level (0 global, 1 master, 2 group, 3 region) they were last given at; off_time defaults to sfizz's 6 ms.
  std::int64_t group = 0, poly = 0;  // poly: 0 = not given or the default (256)
  int group_level = -1, poly_level = -1;
  const Op* poly_op = nullptr;
  const Op* hdr_poly_op = nullptr;  // the first polyphony given in a <global>/<master>/<group> header (a region-set limit in sfizz)
  float off_time = 0.006f;
  // Story 13.8. Per filter (0 = the first, tuned here; 1 = the second, complete here): key track, velocity track, CC entries for the cutoff and the resonance.
  struct FilterAcc {
    float keytrack = 0, veltrack = 0;
    int keycenter = 60;
    std::map<int, ModAcc> cut, res;  // by controller, ascending
    std::vector<const Op*> ops;      // the Story 13.8 opcodes as given, for the "no effect without cutoff" warnings
    // the second filter only
    bool cutoff_set = false, type_set = false;
    FilterType type = FilterType::kLpf2p;
    float cutoff = 0, resonance = 0;
    const Op* cutoff_op = nullptr;
  };
  FilterAcc fil[2];
  // Story 13.9. EQ bands (1..3), the amplitude / pitch / filter LFOs and the pitch / filter EGs as given; folded into Region::mod at flush_region.
  struct EqAcc {
    bool used = false;
    EqType type = EqType::kPeak;
    float freq = 0, bw = 1, gain = 0, vel2freq = 0, vel2gain = 0;
    std::map<int, ModAcc> mods[3];  // freq, bw, gain; by controller
  };
  EqAcc eq[kMaxEqBands];
  struct LfoAcc {
    bool used = false, depth_given = false;  // depth_given: an explicit depth (even 0) is a deliberate disable, not a forgotten depth
    LfoSpec s;
    const Op* op = nullptr;
  };
  LfoAcc lfo[3];  // amplfo, pitchlfo, fillfo
  struct EgAcc {
    bool used = false, depth_given = false;
    EgSpec s;
    const Op* op = nullptr;
  };
  EgAcc eg[2];  // pitcheg, fileg
};

// Story 13.5: a recognised CC opcode split into its parts. gate: locc/hicc/lohdcc/hihdcc (lo = true for the low end, hd = normalized value); def: set_cc/set_hdcc;
// mod: <target>_oncc (curve = false) or <target>_curvecc (curve = true).
struct CcOp {
  enum class Kind { kNone, kGate, kTrigger, kDefault, kMod } kind = Kind::kNone;
  bool lo = false, hd = false, curve = false, canonical = true;
  int target = 0, cc = 0;
};

// Story 13.8: target ids of the filter CC entries, above the CcTarget range.
constexpr int kFilCutoff0 = 100, kFilCutoff1 = 101, kFilRes0 = 102, kFilRes1 = 103;
// Story 13.9: EQ entry targets, kEqBase + band * 3 + (0 freq, 1 bw, 2 gain).
constexpr int kEqBase = 110;

// Story 13.9: "eq2_vel2gain" -> band 1, kind 'T' type, 'F' freq, 'B' bw, 'G' gain, 'f' vel2freq, 'g' vel2gain.
bool eq_opcode(const std::string& k, int& band, char& kind) {
  if (k.size() < 6 || k.compare(0, 2, "eq") != 0 || k[2] < '1' || k[2] > '3' || k[3] != '_') return false;
  band = k[2] - '1';
  const std::string rest = k.substr(4);
  if (rest == "freq") kind = 'F';
  else if (rest == "bw") kind = 'B';
  else if (rest == "gain") kind = 'G';
  else if (rest == "type") kind = 'T';
  else if (rest == "vel2freq" || rest == "veltofreq") kind = 'f';
  else if (rest == "vel2gain" || rest == "veltogain") kind = 'g';
  else return false;
  return true;
}

// Story 13.9: "amplfo_depth" -> lfo 0 (amplfo, pitchlfo, fillfo), field 'D' depth, 'F' freq, 'L' delay, 'E' fade, 'W' wave. "pitcheg_vel2depth" -> eg 0 (pitcheg, fileg),
// field 'd' delay, 's' start, 'a' attack, 'h' hold, 'c' decay, 'u' sustain, 'r' release, 'D' depth, 'V' vel2depth.
bool lfo_opcode(const std::string& k, int& idx, char& field) {
  static const char* const names[] = {"amplfo_", "pitchlfo_", "fillfo_"};
  for (int i = 0; i < 3; ++i) {
    const std::string p = names[i];
    if (k.compare(0, p.size(), p) != 0) continue;
    const std::string rest = k.substr(p.size());
    idx = i;
    if (rest == "depth") field = 'D';
    else if (rest == "freq") field = 'F';
    else if (rest == "delay") field = 'L';
    else if (rest == "fade") field = 'E';
    else if (rest == "wave") field = 'W';
    else return false;
    return true;
  }
  return false;
}
bool eg_opcode(const std::string& k, int& idx, char& field) {
  static const char* const names[] = {"pitcheg_", "fileg_"};
  for (int i = 0; i < 2; ++i) {
    const std::string p = names[i];
    if (k.compare(0, p.size(), p) != 0) continue;
    const std::string rest = k.substr(p.size());
    idx = i;
    if (rest == "delay") field = 'd';
    else if (rest == "start") field = 's';
    else if (rest == "attack") field = 'a';
    else if (rest == "hold") field = 'h';
    else if (rest == "decay") field = 'c';
    else if (rest == "sustain") field = 'u';
    else if (rest == "release") field = 'r';
    else if (rest == "depth") field = 'D';
    else if (rest == "vel2depth" || rest == "veltodepth") field = 'V';
    else return false;
    return true;
  }
  return false;
}

struct FilterOp {
  int idx = 0;
  char kind = 0;
};

// Story 13.8: "cutoff2" -> filter 1 (the second), kind 'C'. Kinds: T type, C cutoff, R resonance, K keytrack, Z keycenter, V veltrack. The first filter is index 0.
bool filter_opcode(const std::string& k, int& idx, char& kind) {
  if (k == "filtype") { idx = 0; kind = 'T'; return true; }
  for (const char* base : {"cutoff", "resonance"}) {
    const std::string b = base;
    if (k == b || k == b + "1") { idx = 0; kind = b[0] == 'c' ? 'C' : 'R'; return true; }
    if (k == b + "2") { idx = 1; kind = b[0] == 'c' ? 'C' : 'R'; return true; }
  }
  for (const char* pre : {"fil_", "fil1_", "fil2_"}) {
    const std::string p = pre;
    if (k.compare(0, p.size(), p) != 0) continue;
    const std::string rest = k.substr(p.size());
    idx = p == "fil2_" ? 1 : 0;
    if (rest == "type") { kind = 'T'; return true; }
    if (rest == "keytrack") { kind = 'K'; return true; }
    if (rest == "keycenter") { kind = 'Z'; return true; }
    if (rest == "veltrack") { kind = 'V'; return true; }
  }
  return false;
}

CcOp split_cc_opcode(const std::string& k) {
  CcOp o;
  std::size_t d = k.size();
  while (d > 0 && k[d - 1] >= '0' && k[d - 1] <= '9') --d;
  if (d == k.size() || d == 0 || k.size() - d > 3) return o;
  o.cc = std::atoi(k.c_str() + d);
  o.canonical = !(k.size() - d > 1 && k[d] == '0');
  const std::string p = k.substr(0, d);
  struct Gate { const char* name; bool lo, hd; };
  static const Gate gates[] = {{"locc", true, false}, {"hicc", false, false}, {"lohdcc", true, true},
                               {"hihdcc", false, true}, {"lorealcc", true, true}, {"hirealcc", false, true}};
  for (const Gate& g : gates)
    if (p == g.name) { o.kind = CcOp::Kind::kGate; o.lo = g.lo; o.hd = g.hd; return o; }
  static const Gate triggers[] = {{"on_locc", true, false}, {"on_hicc", false, false}, {"start_locc", true, false}, {"start_hicc", false, false}};
  for (const Gate& g : triggers)  // Story 13.7: a hdcc form is not accepted (see the opcode table)
    if (p == g.name) { o.kind = CcOp::Kind::kTrigger; o.lo = g.lo; o.hd = g.hd; return o; }
  if (p == "set_cc" || p == "set_hdcc" || p == "set_realcc") { o.kind = CcOp::Kind::kDefault; o.hd = p != "set_cc"; return o; }
  auto ends = [&](const char* suf) {
    const std::size_t n = std::strlen(suf);
    return p.size() > n && p.compare(p.size() - n, n, suf) == 0;
  };
  std::string stem;
  if (ends("_curvecc")) { o.curve = true; stem = p.substr(0, p.size() - 8); }
  else if (ends("_oncc")) stem = p.substr(0, p.size() - 5);
  else if (ends("_cc")) stem = p.substr(0, p.size() - 3);
  else if ((p.compare(0, 6, "ampeg_") == 0 || p.compare(0, 2, "eq") == 0) && ends("cc")) stem = p.substr(0, p.size() - 2);  // ampeg_releasecc64, eq1_freqcc80
  else return o;
  struct Target { const char* name; int id; };
  static const Target targets[] = {
      {"amplitude", int(CcTarget::kAmplitude)}, {"volume", int(CcTarget::kVolume)}, {"gain", int(CcTarget::kVolume)},
      {"pan", int(CcTarget::kPan)}, {"tune", int(CcTarget::kTune)}, {"pitch", int(CcTarget::kTune)}, {"offset", int(CcTarget::kOffset)},
      {"ampeg_delay", int(CcTarget::kEgDelay)}, {"ampeg_start", int(CcTarget::kEgStart)}, {"ampeg_attack", int(CcTarget::kEgAttack)},
      {"ampeg_hold", int(CcTarget::kEgHold)}, {"ampeg_decay", int(CcTarget::kEgDecay)}, {"ampeg_sustain", int(CcTarget::kEgSustain)},
      {"ampeg_release", int(CcTarget::kEgRelease)},
      // Story 13.8: filter targets (ids above the CcTarget range; they live in the filter table, not the cc table)
      {"cutoff", kFilCutoff0}, {"cutoff1", kFilCutoff0}, {"cutoff2", kFilCutoff1},
      {"resonance", kFilRes0}, {"resonance1", kFilRes0}, {"resonance2", kFilRes1}};
  for (const Target& t : targets)
    if (stem == t.name) { o.kind = CcOp::Kind::kMod; o.target = t.id; return o; }
  {  // Story 13.9: eqN_freq / eqN_bw / eqN_gain entries
    int band = 0;
    char kind = 0;
    if (eq_opcode(stem, band, kind) && (kind == 'F' || kind == 'B' || kind == 'G')) {
      o.kind = CcOp::Kind::kMod;
      o.target = kEqBase + band * 3 + (kind == 'F' ? 0 : kind == 'B' ? 1 : 2);
      return o;
    }
  }
  return o;
}

bool is_eg_target(int t) { return t >= int(CcTarget::kEgDelay) && t <= int(CcTarget::kEgRelease); }

// "xfin_locc64" -> in, lo, cc 64. Names are already classified as xf opcodes.
bool xf_opcode(const std::string& name, bool& in, bool& lo, int& cc) {
  static const char* const names[] = {"xfin_locc", "xfin_hicc", "xfout_locc", "xfout_hicc"};
  for (int i = 0; i < 4; ++i) {
    const std::string p = names[i];
    if (name.size() > p.size() && name.compare(0, p.size(), p) == 0) {
      cc = std::atoi(name.c_str() + p.size());
      in = i < 2;
      lo = i % 2 == 0;
      return true;
    }
  }
  return false;
}

std::string trim(std::string s) {
  const char* ws = " \t\r\n";
  const auto b = s.find_first_not_of(ws);
  if (b == std::string::npos) return {};
  return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

bool parse_double(const std::string& v, double& out) {
  const std::string t = trim(v);
  if (t.empty()) return false;
  char* end = nullptr;
  errno = 0;
  out = std::strtod(t.c_str(), &end);
  return errno == 0 && *end == 0 && std::isfinite(out);
}

bool parse_int(const std::string& v, std::int64_t& out) {
  const std::string t = trim(v);
  if (t.empty()) return false;
  char* end = nullptr;
  errno = 0;
  out = std::strtoll(t.c_str(), &end, 10);
  return errno == 0 && *end == 0;
}

// MIDI note number or note name (c4 = 60; sharps '#', flats 'b'; octave may be negative).
bool parse_note(const std::string& v, std::int64_t& out) {
  const std::string t = trim(v);
  if (t.empty()) return false;
  if (parse_int(t, out)) return out >= 0 && out <= 127;
  static const int base[7] = {9, 11, 0, 2, 4, 5, 7};  // a b c d e f g
  const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(t[0])));
  if (l < 'a' || l > 'g') return false;
  int semi = base[l - 'a'];
  std::size_t i = 1;
  if (i < t.size() && t[i] == '#') { ++semi; ++i; }
  else if (i < t.size() && (t[i] == 'b' || t[i] == 'B')) { --semi; ++i; }
  std::int64_t octave = 0;
  if (i >= t.size() || !parse_int(t.substr(i), octave)) return false;
  out = 12 * (octave + 1) + semi;
  return out >= 0 && out <= 127;
}

std::string norm_slashes(std::string s) {
  for (char& c : s)
    if (c == '\\') c = '/';
  return s;
}

class Importer final : public sfz::ParserListener {
 public:
  explicit Importer(const fs::path& main) : main_(main), dir_(main.parent_path().lexically_normal()) {}

  SfzParseResult result;

  void onParseBegin() override {}
  void onParseEnd() override {
    flush_region();
    for (const auto& [cc, d] : cc_defaults_) result.cc_defaults.push_back(d);
  }

  void onParseHeader(const sfz::SourceRange& range, const std::string& header) override {
    flush_region();
    const Loc loc = at(range);
    if (header == "global") {
      global_.clear(); master_.clear(); group_.clear(); region_.clear();
      cur_ = Cur::kGlobal;
    } else if (header == "master") {
      master_.clear(); group_.clear(); region_.clear();
      cur_ = Cur::kMaster;
    } else if (header == "group") {
      group_.clear(); region_.clear();
      cur_ = Cur::kGroup;
    } else if (header == "region") {
      region_.clear();
      cur_ = Cur::kRegion;
      region_loc_ = loc;
      ++result.source_region_count;
    } else if (header == "control") {
      cur_ = Cur::kControl;
    } else if (header == "curve") {
      cur_ = Cur::kCurve;
      SfzCurve c;
      c.file = loc.file;
      c.line = loc.line;
      if (result.curves.size() < 256) result.curves.push_back(std::move(c));
      else issue(SfzIssueKind::kUnsupportedValue, loc, "more than 256 <curve> blocks", "<curve>");
    } else {
      issue(SfzIssueKind::kUnsupportedHeader, loc, "unsupported header <" + header + ">", "<" + header + ">");
      cur_ = Cur::kIgnored;
    }
  }

  void onParseOpcode(const sfz::SourceRange& range, const sfz::SourceRange&,
                     const std::string& name, const std::string& value) override {
    const Loc loc = at(range);
    Op op{name, value, loc.file, loc.line};
    switch (cur_) {
      case Cur::kNone:
        issue(SfzIssueKind::kParse, loc, "opcode '" + name + "' outside any header");
        return;
      case Cur::kIgnored:
        return;
      case Cur::kCurve:
        curve_opcode(op);
        return;
      default:
        break;
    }
    if (classify_opcode(name) == OpcodeClass::kAllowListed) {
      // Informational only: accepted, counted, never stored.
      result.allowlisted.push_back({name, loc.file, loc.line});
      return;
    }
    switch (cur_) {
      case Cur::kControl:
        if (name == "default_path") {
          const std::string dp = norm_slashes(value);
          if (check_path(dp, op)) default_path_ = dp;
        } else if (const CcOp co = split_cc_opcode(name); co.kind == CcOp::Kind::kDefault) {
          control_default(op, co);
        } else {
          issue(SfzIssueKind::kUnsupportedOpcode, loc, "unsupported opcode '" + name + "' in <control>", name);
        }
        return;
      default:
        break;
    }
    if (classify_opcode(name) != OpcodeClass::kSupported || name == "default_path") {
      issue(SfzIssueKind::kUnsupportedOpcode, loc, "unsupported opcode '" + name + "'", name);
      return;
    }
    if (name == "sample") {
      // `sample` is resolved against default_path as it was when the opcode was read.
      const std::string raw = norm_slashes(value);
      if (!default_path_.empty() && fs::path(raw).is_absolute()) {
        issue(SfzIssueKind::kInvalidValue, loc, "absolute sample '" + raw + "' combined with default_path '" + default_path_ + "'");
        return;
      }
      op.value = default_path_ + raw;
      if (!check_path(op.value, op)) return;
    } else {
      Region scratch;  // validate once, here, so each bad value is reported exactly once
      Acc acc;
      if (!apply(scratch, acc, op, true)) return;
    }
    Level& lv = cur_ == Cur::kGlobal ? global_ : cur_ == Cur::kMaster ? master_
              : cur_ == Cur::kGroup ? group_ : region_;
    lv.push_back(std::move(op));
  }

  void onParseError(const sfz::SourceRange& range, const std::string& message) override {
    issue(SfzIssueKind::kParse, at(range), message);
  }
  void onParseWarning(const sfz::SourceRange& range, const std::string& message) override {
    issue(SfzIssueKind::kParse, at(range), "warning: " + message);
  }

 private:
  enum class Cur { kNone, kGlobal, kMaster, kGroup, kRegion, kControl, kCurve, kIgnored };
  struct Loc {
    std::string file;
    std::size_t line = 0;
  };

  Loc at(const sfz::SourceRange& r) const {
    Loc l;
    if (r.start.filePath) l.file = r.start.filePath->string();
    else l.file = main_.string();
    l.line = r.start ? r.start.lineNumber + 1 : 0;  // the parser counts lines from 0
    return l;
  }

  // Every issue goes through here: identical (file, line, message) triples are reported once.
  bool add(SfzIssueKind k, const std::string& file, std::size_t line, std::string msg,
           std::string opcode = {}) {
    if (!seen_.insert(file + ":" + std::to_string(line) + ":" + msg).second) return false;
    result.issues.push_back({k, file, line, std::move(msg), file, std::move(opcode)});
    return true;
  }
  void issue(SfzIssueKind k, const Loc& l, std::string msg, std::string opcode = {}) {
    add(k, l.file, l.line, std::move(msg), std::move(opcode));
  }

  // A path (sample or default_path) must stay inside the SFZ's directory.
  bool check_path(const std::string& p, const Op& op) {
    if (p.empty()) return true;
    const fs::path full = (fs::path(p).is_absolute() ? fs::path(p) : dir_ / p).lexically_normal();
    const fs::path rel = full.lexically_relative(dir_);
    if (rel.empty() || *rel.begin() == "..") {
      add(SfzIssueKind::kInvalidValue, op.file, op.line,
          "path '" + p + "' leaves the SFZ directory (no '..' escapes or absolute paths outside it)");
      return false;
    }
    return true;
  }

  // <curve>: curve_index and vNNN points are stored inert (bounded: 256 curves, 256 points each); anything else is unsupported.
  void curve_opcode(const Op& op) {
    report_ = true;  // called once per opcode
    SfzCurve& c = result.curves.back();
    const std::string& k = op.name;
    std::int64_t n = 0;
    double d = 0;
    if (k == "curve_index") {
      if (parse_int(op.value, n) && n >= 0 && n <= 255) c.index = int(n);
      else {
        c.index = 0;  // sfizz reads curve_index with a default of 0, so an unusable value spells index 0 (not "implicit")
        ignore(op, "expected curve index 0..255; index 0 is used");
      }
    } else if (k.size() >= 2 && k.size() <= 4 && k[0] == 'v' &&
               k.find_first_not_of("0123456789", 1) == std::string::npos) {
      const int pt = std::atoi(k.c_str() + 1);
      if (pt > 255) ignore(op, "curve point index above 255");
      else if (!parse_double(op.value, d) || !std::isfinite(d)) ignore(op, "expected a finite number");
      else if (c.points.size() >= 256) ignore(op, "more than 256 points in one curve");
      else c.points.emplace_back(pt, d);
    } else {
      add(SfzIssueKind::kUnsupportedOpcode, op.file, op.line, "unsupported opcode '" + k + "' in <curve>", k);
    }
  }

  // Records a value replaced by the opcode default (once per opcode occurrence: only on the reporting pass).
  void ignore(const Op& op, const char* why, bool default_used = true) {
    if (!report_) return;
    result.ignored_values.push_back({op.name, op.value, op.file, op.line,
                                     "ignored value '" + op.value + "' for opcode '" + op.name + "' (" + why +
                                         ")" + (default_used ? "; the opcode default is used" : "")});
  }

  void bad(const Op& op, const char* why) {
    if (!report_) return;
    add(SfzIssueKind::kInvalidValue, op.file, op.line,
        "invalid value '" + op.value + "' for opcode '" + op.name + "' (" + why + ")");
  }
  void unsupported_value(const Op& op) {
    if (!report_) return;
    add(SfzIssueKind::kUnsupportedValue, op.file, op.line,
        "unsupported value '" + op.value + "' for opcode '" + op.name + "'", op.name);
  }

  bool as_int(const Op& op, std::int64_t lo, std::int64_t hi, std::int64_t& out) {
    if (!parse_int(op.value, out) || out < lo || out > hi) {
      bad(op, "expected integer in range");
      return false;
    }
    return true;
  }
  bool as_float(const Op& op, double lo, double hi, double& out) {
    if (!parse_double(op.value, out) || out < lo || out > hi) {
      bad(op, "expected number in range");
      return false;
    }
    return true;
  }
  bool as_note(const Op& op, std::int64_t& out) {
    if (!parse_note(op.value, out)) {
      bad(op, "expected MIDI note number or name");
      return false;
    }
    return true;
  }

  // lokey/hikey/key (sfizz): an unparseable or out-of-range note is not an error; the caller keeps the default.
  bool as_note_or_ignore(const Op& op, std::int64_t& out) {
    if (parse_note(op.value, out)) return true;
    ignore(op, "not a MIDI note 0..127");
    return false;
  }

  // A number the value must parse as; outside [lo, hi] it is clamped (sfizz's permissive bounds) and recorded in ignored_values.
  bool as_clamped(const Op& op, double lo, double hi, double& out) {
    if (!parse_double(op.value, out)) {
      bad(op, "expected a number");
      return false;
    }
    if (out < lo || out > hi) {
      out = out < lo ? lo : hi;
      ignore(op, "outside the range sfizz accepts, clamped", false);
    }
    if (out == 0) out = 0;  // "-0" is stored as +0: one encoding per value (the bundle compares bytes, the model compares numbers)
    return true;
  }

  // A finite number (any magnitude); the caller applies the range rule of its opcode.
  bool as_float_any(const Op& op, double& out) {
    if (!parse_double(op.value, out) || !std::isfinite(out)) {
      bad(op, "expected a number");
      return false;
    }
    return true;
  }

  void unsupported_cc(const Op& op, const std::string& why) {
    if (!report_) return;
    add(SfzIssueKind::kUnsupportedValue, op.file, op.line, "unsupported controller for opcode '" + op.name + "' (" + why + ")", op.name);
  }

  // Story 13.5: locc/hicc/lohdcc/hihdcc and the `_oncc` / `_curvecc` modulation forms. The values are validated here (once, when read) and folded into the
  // region at flush_region.
  bool apply_cc(Acc& acc, const Op& op, const CcOp& o) {
    if (!o.canonical) {
      if (report_)
        add(SfzIssueKind::kInvalidValue, op.file, op.line,
            "opcode '" + op.name + "' has a non-canonical controller number (leading zeros would spell one controller two ways)");
      return false;
    }
    double d = 0;
    if (o.kind == CcOp::Kind::kDefault) {  // reaches apply only from a region level
      if (report_) add(SfzIssueKind::kUnsupportedOpcode, op.file, op.line, "opcode '" + op.name + "' is only valid in <control>", op.name);
      return false;
    }
    if (o.kind == CcOp::Kind::kGate) {
      if (o.cc > 127) {
        unsupported_cc(op, "only controllers 0..127 can gate a region; sfizz never refreshes a gate on a pseudo-controller after load");
        return false;
      }
      if (o.hd ? !as_clamped(op, 0, 1, d) : !as_clamped(op, 0, 127, d)) return false;
      if (o.hd) d *= 127.0;
      Acc::GateAcc& g = acc.gates[o.cc];
      if (o.lo) {
        g.lo_set = true;
        g.lo = float(d);
      } else {
        g.hi_set = true;
        // sfizz hicc (not hdcc) fills the gap up to the next whole CC step: hicc=N accepts every value below N + 1.
        g.hi = (!o.hd && d <= 126.0) ? std::nextafter(float(d) + 1.0f, 0.0f) : float(d);
      }
      return true;
    }
    if (o.kind == CcOp::Kind::kTrigger) {  // Story 13.7: same value rules as a gate (CC units, hicc fill-gap)
      if (o.cc > 127) {
        unsupported_cc(op, "only controllers 0..127 can start a voice");
        return false;
      }
      if (!as_clamped(op, 0, 127, d)) return false;
      Acc::GateAcc& g = acc.trigs[o.cc];
      if (o.lo) {
        g.lo_set = true;
        g.lo = float(d);
      } else {
        g.hi_set = true;
        g.hi = d <= 126.0 ? std::nextafter(float(d) + 1.0f, 0.0f) : float(d);
      }
      return true;
    }
    if (!is_cc_mod_source(unsigned(o.cc))) {
      unsupported_cc(op, "controllers 0..127 and the pseudo-controllers 131 (velocity), 135, 136 (random), 137 (alternate), 140, 141 (key delta) are supported");
      return false;
    }
    if (o.target >= kEqBase) {  // Story 13.9: freq / bw / gain entries of an EQ band
      const int band = (o.target - kEqBase) / 3, kind = (o.target - kEqBase) % 3;
      Acc::EqAcc& ea = acc.eq[band];
      ea.used = true;
      Acc::ModAcc& e = ea.mods[kind][o.cc];
      if (o.curve) {
        std::int64_t n = 0;
        if (!parse_int(op.value, n) || n < 0 || n > 255) {
          ignore(op, "expected a curve index 0..255; curve 0 is used");
          n = 0;
        }
        e.curve = int(n);
        return true;
      }
      const double lim = kind == 0 ? 20000.0 : kind == 1 ? 4.0 : 96.0;
      if (!as_clamped(op, -lim, lim, d)) return false;
      if (d == 0) d = 0;
      e.depth = float(d);
      e.depth_set = true;
      return true;
    }
    if (o.target >= kFilCutoff0) {  // Story 13.8: cutoff / resonance entries of the first or second filter
      Acc::FilterAcc& fa = acc.fil[(o.target == kFilCutoff1 || o.target == kFilRes1) ? 1 : 0];
      const bool is_cut = o.target == kFilCutoff0 || o.target == kFilCutoff1;
      Acc::ModAcc& e = (is_cut ? fa.cut : fa.res)[o.cc];
      fa.ops.push_back(&op);
      if (o.curve) {
        std::int64_t n = 0;
        if (!parse_int(op.value, n) || n < 0 || n > 255) {
          ignore(op, "expected a curve index 0..255; curve 0 is used");
          n = 0;
        }
        e.curve = int(n);
        return true;
      }
      if (is_cut ? !as_clamped(op, -12000, 12000, d) : !as_clamped(op, 0, 96, d)) return false;
      if (d == 0) d = 0;
      e.depth = float(d);
      e.depth_set = true;
      return true;
    }
    if (o.curve) {
      if (o.target == int(CcTarget::kOffset)) {  // sfizz has no offset_curvecc: the opcode is ignored
        if (report_)
          result.warnings.push_back({op.name, op.file, op.line, "opcode '" + op.name + "' has no effect: sfizz does not apply a curve to offset_oncc<N>"});
        return true;
      }
      std::int64_t n = 0;
      if (!parse_int(op.value, n) || n < 0 || n > 255) {
        ignore(op, "expected a curve index 0..255; curve 0 is used");
        n = 0;
      }
      acc.mods[{o.target, o.cc}].curve = int(n);
      return true;
    }
    double lo = -100, hi = 100;
    switch (CcTarget(o.target)) {
      case CcTarget::kAmplitude: lo = 0; hi = 10000; break;
      case CcTarget::kVolume: lo = -144; hi = 48; break;
      case CcTarget::kPan: lo = -200; hi = 200; break;
      case CcTarget::kTune: lo = -12000; hi = 12000; break;
      case CcTarget::kOffset: lo = 0; hi = 2147483647.0; break;
      default: break;
    }
    if (!as_clamped(op, lo, hi, d)) return false;
    if (o.target == int(CcTarget::kOffset)) d = std::trunc(d);  // sfizz reads the offset depth as an integer
    if (d == 0) d = 0;
    acc.mods[{o.target, o.cc}].depth = float(d);
    acc.mods[{o.target, o.cc}].depth_set = true;
    return true;
  }

  // <control> set_ccN / set_hdccN / set_realccN: the part's initial controller value.
  void control_default(const Op& op, const CcOp& o) {
    report_ = true;
    if (!o.canonical) {
      add(SfzIssueKind::kInvalidValue, op.file, op.line, "opcode '" + op.name + "' has a non-canonical controller number");
      return;
    }
    if (o.cc > 127) {
      unsupported_cc(op, "only controllers 0..127 have a value to seed");
      return;
    }
    double d = 0;
    if (o.hd ? !as_clamped(op, 0, 1, d) : !as_clamped(op, 0, 127, d)) return;
    if (o.hd) d *= 127.0;
    if (d == 0) d = 0;
    cc_defaults_[o.cc] = {o.cc, float(d), op.file, op.line};
  }

  // Story 13.9: region EQ bands, dedicated LFOs and EGs (the opcode is one of eq_opcode / lfo_opcode / eg_opcode).
  bool apply_mod(Acc& acc, const Op& op) {
    const std::string& k = op.name;
    std::int64_t n = 0;
    double d = 0;
    int eqb = 0, idx = 0;
    char ek = 0, fld = 0;
    if (eq_opcode(k, eqb, ek)) {  // Story 13.9
      Acc::EqAcc& ea = acc.eq[eqb];
      ea.used = true;
      if (ek == 'T') {
        const std::string v = trim(op.value);
        if (v == "peak") ea.type = EqType::kPeak;
        else if (v == "lshelf") ea.type = EqType::kLowShelf;
        else if (v == "hshelf") ea.type = EqType::kHighShelf;
        else { unsupported_value(op); return false; }
      } else {
        const double lo = ek == 'B' ? 0.01 : ek == 'f' ? -30000 : ek == 'F' ? 0 : -96, hi = ek == 'B' ? 4 : ek == 'f' ? 30000 : ek == 'F' ? 20000 : ek == 'G' ? 60 : 96;  // bw and gain: what the DSP honours
        if (!as_clamped(op, lo, hi, d)) return false;
        if (d == 0) d = 0;
        (ek == 'F' ? ea.freq : ek == 'B' ? ea.bw : ek == 'G' ? ea.gain : ek == 'f' ? ea.vel2freq : ea.vel2gain) = float(d);
      }
      return true;
    }
    if (lfo_opcode(k, idx, fld)) {  // Story 13.9
      Acc::LfoAcc& la = acc.lfo[idx];
      la.used = true;
      la.op = &op;
      if (fld == 'D') la.depth_given = true;
      if (fld == 'W') {
        if (!parse_int(trim(op.value), n) || n < 0 || n > int(kMaxLfoWave)) { unsupported_value(op); return false; }  // 12 (random S&H) has no deterministic oracle
        la.s.wave = static_cast<LfoWave>(n);
      } else {
        const double dmax = idx == 0 ? 10.0 : 1200.0;
        const double lo = fld == 'D' ? -dmax : 0.0, hi = fld == 'D' ? dmax : fld == 'F' ? 100.0 : 30.0;
        if (!as_clamped(op, lo, hi, d)) return false;
        if (d == 0) d = 0;
        (fld == 'D' ? la.s.depth : fld == 'F' ? la.s.freq : fld == 'L' ? la.s.delay : la.s.fade) = float(d);
      }
      return true;
    }
    if (eg_opcode(k, idx, fld)) {  // Story 13.9
      Acc::EgAcc& ga = acc.eg[idx];
      ga.used = true;
      ga.op = &op;
      if (fld == 'D' || fld == 'V') ga.depth_given = true;
      const bool depthy = fld == 'D' || fld == 'V';
      if (!as_clamped(op, depthy ? -12000 : 0, depthy ? 12000 : 100, d)) return false;
      if (d == 0) d = 0;
      float& f = fld == 'd' ? ga.s.delay : fld == 's' ? ga.s.start : fld == 'a' ? ga.s.attack : fld == 'h' ? ga.s.hold : fld == 'c' ? ga.s.decay
                 : fld == 'u' ? ga.s.sustain : fld == 'r' ? ga.s.release : fld == 'D' ? ga.s.depth : ga.s.vel2depth;
      f = float(d);
      return true;
    }
    return true;
  }

  // Applies one opcode to `r`. Returns false for an invalid value (reported when `report`).
  bool apply(Region& r, Acc& acc, const Op& op, bool report) {
    SfzLoopSpec& ls = acc.ls;
    report_ = report;
    std::int64_t n = 0;
    double d = 0;
    const std::string& k = op.name;
    const std::int64_t kMaxFrames = std::int64_t(1) << 40;
    if (k == "sample") return true;
    if (const CcOp co = split_cc_opcode(k); co.kind != CcOp::Kind::kNone) return apply_cc(acc, op, co);
    {  // Story 13.9
      int mi = 0;
      char mc = 0;
      if (eq_opcode(k, mi, mc) || lfo_opcode(k, mi, mc) || eg_opcode(k, mi, mc)) return apply_mod(acc, op);
    }
    if (k == "ampeg_dynamic") {  // sfizz BoolSpec: 0 or 1; anything that is not a non-zero integer is off
      const std::string v = trim(op.value);
      if (v == "true" || v == "on") acc.dynamic = true;
      else if (parse_int(v, n)) acc.dynamic = n != 0;
      else {
        acc.dynamic = false;
        ignore(op, "expected 0, 1, true or on; the flag is off");
      }
      return true;
    }
    if (k == "lokey") {
      r.key_lo = as_note_or_ignore(op, n) ? std::uint8_t(n) : Region{}.key_lo;
    } else if (k == "hikey") {
      std::int64_t raw = 0;
      if (parse_int(op.value, raw) && raw == -1) {  // sfizz: no note triggers this region
        acc.never_note = true;
        acc.never_op = &op;  // reported when the region is flushed, and only if no on_locc / on_hicc trigger keeps it
        return true;
      }
      acc.never_note = false;
      r.key_hi = as_note_or_ignore(op, n) ? std::uint8_t(n) : Region{}.key_hi;
    } else if (k == "key") {
      if (as_note_or_ignore(op, n)) { r.key_lo = r.key_hi = r.key_center = std::uint8_t(n); acc.never_note = false; }
      else { const Region d0; r.key_lo = d0.key_lo; r.key_hi = d0.key_hi; r.key_center = d0.key_center; }
    }
    else if (k == "pitch_keycenter") { if (!as_note(op, n)) return false; r.key_center = std::uint8_t(n); }
    else if (k == "lovel") { if (!as_int(op, 0, 127, n)) return false; r.vel_lo = std::uint8_t(n); }
    else if (k == "hivel") { if (!as_int(op, 0, 127, n)) return false; r.vel_hi = std::uint8_t(n); }
    else if (k == "transpose") { if (!as_int(op, -127, 127, n)) return false; r.transpose = std::int32_t(n); }
    else if (k == "tune") { if (!as_float(op, -1200, 1200, d)) return false; r.tune_cents = float(d); }
    else if (k == "volume") { if (!as_float(op, -144, 48, d)) return false; r.volume_db = float(d); }
    else if (k == "pan") { if (!as_float(op, -100, 100, d)) return false; r.pan = float(d); }
    else if (k == "offset") { if (!as_int(op, 0, kMaxFrames, n)) return false; r.offset = std::uint64_t(n); }
    else if (k == "seq_length") { if (!as_int(op, 1, 255, n)) return false; r.seq_length = std::uint32_t(n); }
    else if (k == "seq_position") { if (!as_int(op, 1, 255, n)) return false; r.seq_position = std::uint32_t(n); }
    else if (k == "lorand") { if (!as_float(op, 0, 1, d)) return false; r.lorand = float(d); }
    else if (k == "hirand") { if (!as_float(op, 0, 1, d)) return false; r.hirand = float(d); }
    else if (k == "end") {  // sfizz: a sample end of 0 (or below) turns the region off; beyond the file it is clamped by the engine
      if (!parse_int(op.value, n) || n > kMaxFrames) { bad(op, "expected integer in range"); return false; }
      r.has_end = true;
      r.end = n > 0 ? std::uint64_t(n) : 0;
    }
    else if (k == "loop_start") { if (!as_int(op, 0, kMaxFrames, n)) return false; r.loop_start = std::uint64_t(n); ls.start_set = true; }
    else if (k == "loop_end") { if (!as_int(op, 0, kMaxFrames, n)) return false; r.loop_end = std::uint64_t(n); ls.end_set = true; }
    else if (k == "loop_mode") {
      const std::string v = trim(op.value);
      if (v == "no_loop") r.loop_mode = LoopMode::kNoLoop;
      else if (v == "one_shot") r.loop_mode = LoopMode::kOneShot;
      else if (v == "loop_continuous") r.loop_mode = LoopMode::kLoopContinuous;
      else if (v == "loop_sustain") r.loop_mode = LoopMode::kLoopSustain;
      else { unsupported_value(op); return false; }
    } else if (k == "trigger") {
      const std::string v = trim(op.value);
      if (v == "attack") r.trigger = TriggerMode::kAttack;
      else if (v == "release") r.trigger = TriggerMode::kRelease;
      else if (v == "first") r.trigger = TriggerMode::kFirst;
      else if (v == "legato") r.trigger = TriggerMode::kLegato;
      else if (v == "release_key") r.trigger = TriggerMode::kReleaseKey;
      else { unsupported_value(op); return false; }
    }
    else if (k == "group" || k == "polyphony_group") {  // sfizz Int64Spec {-int32_max, uint32_max}; this importer keeps the int32 range
      if (!parse_int(op.value, n)) { bad(op, "expected an integer"); return false; }
      if (n < -2147483647 || n > 2147483647) { unsupported_value(op); return false; }
      acc.group = n;
    }
    else if (k == "off_by" || k == "offby") {
      if (!parse_int(op.value, n)) { bad(op, "expected an integer"); return false; }
      if (n < -2147483647 || n > 2147483647) { unsupported_value(op); return false; }
      r.vg.has_off_by = true;
      r.vg.off_by = std::int32_t(n);
    }
    else if (k == "off_mode" || k == "offmode") {
      const std::string v = trim(op.value);
      if (v == "fast") r.vg.off_mode = OffMode::kFast;
      else if (v == "normal") r.vg.off_mode = OffMode::kNormal;
      else if (v == "time") r.vg.off_mode = OffMode::kTime;
      else { unsupported_value(op); return false; }
    }
    else if (k == "off_time") {  // sfizz: off_time also selects off_mode=time
      if (!as_clamped(op, 0, 100, d)) return false;
      acc.off_time = float(d);
      r.vg.off_mode = OffMode::kTime;
    }
    else if (k == "polyphony") {
      if (!parse_int(op.value, n)) { bad(op, "expected an integer"); return false; }
      if (n == 0) { unsupported_value(op); return false; }
      if (n < 0 || n > 256) { ignore(op, "outside 1..256 (sfizz enforces the bounds); no limit", true); acc.poly = 0; return true; }
      acc.poly = n >= 256 ? 0 : n;  // 256 is sfizz's default: no limit
      acc.poly_op = &op;
    }
    else if (k == "note_polyphony") {
      if (!parse_int(op.value, n)) { bad(op, "expected an integer"); return false; }
      if (n < 1 || n > 256) { ignore(op, "outside 1..256 (sfizz enforces the bounds); no limit", true); r.vg.note_polyphony = 0; return true; }
      r.vg.note_polyphony = n >= 256 ? 0 : std::uint16_t(n);
    }
    else if (k == "note_selfmask") {
      const std::string v = trim(op.value);
      if (v == "on" || v == "mask") r.vg.note_selfmask = true;
      else if (v == "off") r.vg.note_selfmask = false;
      else { unsupported_value(op); return false; }
    }
    else if (k == "bend_up" || k == "bendup" || k == "bend_down" || k == "benddown") {  // Story 13.7
      if (!as_float_any(op, d)) return false;
      if (d < -double(kMaxBendCents) || d > double(kMaxBendCents)) { unsupported_value(op); return false; }  // sfizz keeps values beyond +-12000 (permissive bounds)
      if (d == 0) d = 0;
      r.bend.enabled = true;
      (k == "bend_up" || k == "bendup" ? r.bend.up : r.bend.down) = float(d);
    }
    else if (k == "bend_step") {
      if (!as_float_any(op, d)) return false;
      if (d < 1.0 || d > double(kMaxBendStep)) { ignore(op, "outside 1..12000, clamped", false); d = d < 1.0 ? 1.0 : double(kMaxBendStep); }
      r.bend.enabled = true;
      r.bend.step = float(d);
    }
    else if (k == "bend_smooth") {
      if (!as_float_any(op, d)) return false;
      if (d < 0.0 || d > 65535.0) { ignore(op, "outside 0..65535 ms, clamped", false); d = d < 0.0 ? 0.0 : 65535.0; }
      r.bend.enabled = true;
      r.bend.smooth_ms = std::uint16_t(std::trunc(d));
    }
    else if (k == "amp_veltrack") { if (!as_clamped(op, -100, 100, d)) return false; r.amp.veltrack = float(d); }
    else if (k == "amplitude") { if (!as_clamped(op, 0, 10000, d)) return false; r.amp.amplitude = float(d); }
    else if (k == "global_amplitude" || k == "master_amplitude" || k == "group_amplitude") {
      if (!as_clamped(op, 0, 10000, d)) return false;
      acc.amp_mult[k[0] == 'g' && k[1] == 'r' ? 0 : k[0] == 'm' ? 1 : 2] = d;
      acc.amp_set = true;
    }
    else if (k == "global_volume" || k == "master_volume" || k == "group_volume") {
      if (!as_clamped(op, -144, 48, d)) return false;
      acc.vol_extra[k[0] == 'g' && k[1] == 'r' ? 0 : k[0] == 'm' ? 1 : 2] = d;
      acc.vol_set = true;
    }
    else if (k == "amp_keytrack") { if (!as_clamped(op, -96, 12, d)) return false; r.amp.keytrack = float(d); }
    else if (k == "amp_keycenter") {  // an unusable note keeps the value inherited from a higher level (or the default)
      if (parse_note(op.value, n)) r.amp.keycenter = std::uint8_t(n);
      else ignore(op, "not a MIDI note 0..127; the inherited value is kept", false);
    }
    else if (k == "ampeg_vel2attack") { if (!as_clamped(op, -100, 100, d)) return false; r.amp.vel2attack = float(d); }
    else if (k == "ampeg_vel2decay") { if (!as_clamped(op, -100, 100, d)) return false; r.amp.vel2decay = float(d); }
    else if (k == "ampeg_vel2sustain") { if (!as_clamped(op, -100, 100, d)) return false; r.amp.vel2sustain = float(d); }
    else if (k == "ampeg_vel2release") { if (!as_clamped(op, -100, 100, d)) return false; r.amp.vel2release = float(d); }
    else if (k.compare(0, 13, "amp_velcurve_") == 0) {
      if (k.size() > 14 && k[13] == '0') {  // like xfin_locc<N>: amp_velcurve_064 would spell velocity 64 twice
        if (report_)
          add(SfzIssueKind::kInvalidValue, op.file, op.line,
              "opcode '" + k + "' has a non-canonical velocity number (write 'amp_velcurve_" + std::to_string(std::atoi(k.c_str() + 13)) + "')");
        return false;
      }
      const int vel = std::atoi(k.c_str() + 13);
      if (vel > 127) { ignore(op, "velocity above 127; sfizz drops the opcode", false); return true; }
      if (!as_clamped(op, 0, 1, d)) return false;
      acc.velcurve[vel] = float(d);
    }
    else if (k == "sw_lokey") { if (!as_note(op, n)) return false; r.sw.lokey = std::int16_t(n); }
    else if (k == "sw_hikey") { if (!as_note(op, n)) return false; r.sw.hikey = std::int16_t(n); }
    else if (k == "sw_last") { if (!as_note(op, n)) return false; r.sw.last = std::int16_t(n); }
    else if (k == "sw_down") { if (!as_note(op, n)) return false; r.sw.down = std::int16_t(n); }
    else if (k == "sw_up") { if (!as_note(op, n)) return false; r.sw.up = std::int16_t(n); }
    else if (k == "sw_previous") { if (!as_note(op, n)) return false; r.sw.previous = std::int16_t(n); }
    else if (k == "sw_default") { if (!as_note(op, n)) return false; r.sw.default_key = std::int16_t(n); }
    else if (k == "xf_cccurve") {
      const std::string v = trim(op.value);
      if (v == "gain") r.xf.curve = XfCurve::kGain;
      else if (v == "power") r.xf.curve = XfCurve::kPower;
      else { unsupported_value(op); return false; }
    }
    else if (FilterOp fo; filter_opcode(k, fo.idx, fo.kind)) {  // Story 13.8 (fil_type, cutoff, resonance, fil_keytrack, ...)
      const int fi = fo.idx;
      const char fk = fo.kind;
      Acc::FilterAcc& fa = acc.fil[fi];
      fa.ops.push_back(&op);
      if (fk == 'T') {
        const std::string v = trim(op.value);
        FilterType t;
        if (v == "lpf_1p") t = FilterType::kLpf1p;
        else if (v == "lpf_2p") t = FilterType::kLpf2p;
        else if (v == "hpf_1p") t = FilterType::kHpf1p;
        else if (v == "hpf_2p") t = FilterType::kHpf2p;
        else if (v == "bpf_2p") t = FilterType::kBpf2p;
        else { unsupported_value(op); return false; }
        if (fi == 0) { acc.type = t; acc.type_set = true; acc.type_op = &op; }
        else { fa.type = t; fa.type_set = true; }
      } else if (fk == 'C') {
        if (!as_float(op, 0, 48000, d)) return false;
        if (d == 0) d = 0;  // "-0" parses as negative zero; the bundle stores one encoding per value
        if (fi == 0) { r.filter.cutoff_hz = float(d); acc.cutoff_set = true; }
        else { fa.cutoff = float(d); fa.cutoff_set = true; fa.cutoff_op = &op; }
      } else if (fk == 'R') {
        if (!as_float(op, 0, 40, d)) return false;
        if (d == 0) d = 0;  // "-0" -> +0, as above
        if (fi == 0) { r.filter.resonance_db = float(d); acc.res_op = &op; }
        else fa.resonance = float(d);
      } else if (fk == 'K') {
        if (!as_clamped(op, 0, 1200, d)) return false;  // as_clamped also turns "-0" into +0
        fa.keytrack = float(d);
      } else if (fk == 'Z') {
        if (!as_note(op, n)) return false;
        fa.keycenter = int(n);
      } else {  // 'V'
        if (!as_clamped(op, -12000, 12000, d)) return false;
        fa.veltrack = float(d);
      }
    }
    else if (bool in = false, lo = false; xf_opcode(k, in, lo, cc_)) {
      {
        const std::size_t base = std::string(in ? "xfin_" : "xfout_").size() + 4;  // prefix + "locc"/"hicc"
        if (k.size() > base + 1 && k[base] == '0') {
          if (report_)
            add(SfzIssueKind::kInvalidValue, op.file, op.line,
                "opcode '" + k + "' has a non-canonical controller number (write '" + k.substr(0, base) +
                    std::to_string(cc_) + "'; leading zeros would spell one controller two ways)");
          return false;
        }
      }
      if (cc_ > 127) { unsupported_value(op); return false; }
      if (!as_int(op, 0, 127, n)) return false;
      XfPart& part = (in ? acc.in : acc.out)[cc_];
      (lo ? part.lo : part.hi) = std::uint8_t(n);
    }
    else if (k == "ampeg_delay") { if (!as_float(op, 0, 100, d)) return false; r.ampeg.delay = float(d); }
    else if (k == "ampeg_start") { if (!as_float(op, 0, 100, d)) return false; r.ampeg.start = float(d); }
    else if (k == "ampeg_attack") { if (!as_float(op, 0, 100, d)) return false; r.ampeg.attack = float(d); }
    else if (k == "ampeg_hold") { if (!as_float(op, 0, 100, d)) return false; r.ampeg.hold = float(d); }
    else if (k == "ampeg_decay") { if (!as_float(op, 0, 100, d)) return false; r.ampeg.decay = float(d); }
    else if (k == "ampeg_sustain") { if (!as_float(op, 0, 100, d)) return false; r.ampeg.sustain = float(d); }
    else if (k == "ampeg_release") { if (!as_float(op, 0, 100, d)) return false; r.ampeg.release = float(d); }
    return true;
  }

  // True when a band has a gain entry that is a connection (a depth other than 0): only then can a band at gain 0 change the sound.
  static bool has_gain_entry(const Acc::EqAcc& ea) {
    for (const auto& kv : ea.mods[2])
      if (kv.second.depth_set && kv.second.depth != 0.0f) return true;
    return false;
  }

  void flush_region() {
    if (cur_ != Cur::kRegion) return;
    cur_ = Cur::kNone;  // a region is flushed exactly once
    Region r;
    Acc acc;
    const Op* sample_op = nullptr;
    int level = 0;
    for (const Level* lv : {&global_, &master_, &group_, &region_}) {
      for (const Op& op : *lv) {
        if (op.name == "sample") sample_op = &op;
        apply(r, acc, op, false);  // values were validated when read
        if (op.name == "group" || op.name == "polyphony_group") acc.group_level = level;
        else if (op.name == "polyphony") {
          acc.poly_level = level;
          if (level <= 2 && !acc.hdr_poly_op) acc.hdr_poly_op = &op;
        }
      }
      ++level;
    }
    const SfzLoopSpec ls = acc.ls;
    // Story 13.4: volumes add and amplitudes multiply, as sfizz does; the amp map keeps the velocity curve.
    if (acc.vol_set) {
      const double v = double(r.volume_db) + acc.vol_extra[0] + acc.vol_extra[1] + acc.vol_extra[2];
      r.volume_db = float(std::clamp(v, -144.0, 48.0));
    }
    if (acc.amp_set) {
      const double a = double(r.amp.amplitude) * (acc.amp_mult[0] / 100.0) * (acc.amp_mult[1] / 100.0) * (acc.amp_mult[2] / 100.0);
      r.amp.amplitude = float(std::min(a, 10000.0));
    }
    if (acc.velcurve.size() > kMaxVelPoints) {
      issue(SfzIssueKind::kUnsupportedValue, region_loc_,
            "more than " + std::to_string(kMaxVelPoints) + " amp_velcurve_<N> points in one region", "amp_velcurve_<N>");
      return;
    }
    for (const auto& [vel, gain] : acc.velcurve) {
      r.amp.curve[r.amp.curve_count].vel = std::uint8_t(vel);
      r.amp.curve[r.amp.curve_count].gain = gain;
      ++r.amp.curve_count;
    }
    {  // Story 13.5: CC gates and CC modulation entries
      CcMap m;
      bool eg_cc = false;
      for (const auto& [cc, g] : acc.gates) {
        const float lo = g.lo_set ? g.lo : 0.0f, hi = g.hi_set ? g.hi : 127.0f;
        if (lo <= 0.0f && hi >= 127.0f) continue;  // covers every value: no gate
        if (m.gate_count >= kMaxCcGates) {
          issue(SfzIssueKind::kUnsupportedValue, region_loc_, "more than " + std::to_string(kMaxCcGates) + " CC gates (locc/hicc) in one region", "locc<N>");
          return;
        }
        if (lo > hi) {
          issue(SfzIssueKind::kInvalidValue, region_loc_, "inverted CC gate (locc" + std::to_string(cc) + " > hicc" + std::to_string(cc) + ")");
          return;
        }
        m.gates[m.gate_count++] = {std::uint8_t(cc), lo, hi};
      }
      for (const auto& [key, e] : acc.mods) {
        if (!e.depth_set) continue;
        // sfizz: a connection with depth 0 is neutral for an additive target, but multiplies the gain by 0 for amplitude, so amplitude keeps it.
        if (e.depth == 0.0f && key.first != int(CcTarget::kAmplitude)) continue;
        if (m.mod_count >= kMaxCcMods) {
          issue(SfzIssueKind::kUnsupportedValue, region_loc_, "more than " + std::to_string(kMaxCcMods) + " CC modulation entries (*_oncc) in one region", "<opcode>_oncc<N>");
          return;
        }
        m.mods[m.mod_count++] = {CcTarget(key.first), std::uint8_t(key.second), std::uint8_t(e.curve), e.depth};
        eg_cc = eg_cc || is_eg_target(key.first);
      }
      if (acc.dynamic && eg_cc) {
        issue(SfzIssueKind::kUnsupportedValue, region_loc_,
              "ampeg_dynamic=1 with ampeg_*_oncc<N>: the envelope would follow the controller during the note; the importer samples it once when the note starts",
              "ampeg_dynamic");
        return;
      }
      r.cc = m;
    }
    {  // Story 13.7: controller triggers and the pitch-bend range
      CcTriggers t;
      for (const auto& [cc, g] : acc.trigs) {
        const float lo = g.lo_set ? g.lo : 0.0f, hi = g.hi_set ? g.hi : 127.0f;  // an entry exists as soon as either opcode is given (sfizz ccTriggers)
        if (t.count >= kMaxCcTriggers) {
          issue(SfzIssueKind::kUnsupportedValue, region_loc_, "more than " + std::to_string(kMaxCcTriggers) + " controller triggers (on_locc/on_hicc) in one region", "on_locc<N>");
          return;
        }
        if (lo > hi) {
          issue(SfzIssueKind::kInvalidValue, region_loc_, "inverted controller trigger (on_locc" + std::to_string(cc) + " > on_hicc" + std::to_string(cc) + ")");
          return;
        }
        t.t[t.count++] = {std::uint8_t(cc), lo, hi};
      }
      if (t.count > 0 && r.trigger != TriggerMode::kAttack) {
        issue(SfzIssueKind::kUnsupportedValue, region_loc_, "on_locc/on_hicc together with trigger=release/first/legato/release_key: a controller voice always starts as an attack", "trigger");
        return;
      }
      t.no_note = acc.never_note && t.count > 0;
      r.trig = t;
      if (acc.never_note && t.count == 0 && acc.never_op &&
          warned_.insert(acc.never_op->file + ":" + std::to_string(acc.never_op->line) + ":hikey-1").second)
        result.ignored_values.push_back({acc.never_op->name, acc.never_op->value, acc.never_op->file, acc.never_op->line,
                                         "ignored value '" + acc.never_op->value + "' for opcode '" + acc.never_op->name +
                                             "' (hikey=-1 and no on_locc/on_hicc trigger: nothing can start the region, it is not imported)"});
    }
    {  // Story 13.6: choke groups and polyphony limits
      VoiceGroup& g = r.vg;
      g.group = std::int32_t(acc.group);
      if (g.off_mode == OffMode::kTime) g.off_time = acc.off_time;
      if (acc.hdr_poly_op && acc.hdr_poly_op != acc.poly_op) {
        // A deeper polyphony replaces the header's value for this region, but sfizz still limits the header's whole region set with the header's own value.
        issue(SfzIssueKind::kUnsupportedValue, Loc{acc.hdr_poly_op->file, acc.hdr_poly_op->line},
              "polyphony in a <global>/<master>/<group> header that a later polyphony overrides: the header's region-set limit would be lost", "polyphony");
        return;
      }
      if (acc.poly != 0) {
        // sfizz: a polyphony in a <global>, <master> or <group> header is also a limit on that header's region set (all its regions together; RegionSet), which
        // this importer has no representation for. A region keeps the whole story when it has a non-zero group number that is set at the same header or a broader
        // one (so every region under the header is in that group, or the file is rejected through the region that leaves it): sfizz then also limits the group
        // (Synth::setGroupPolyphony, from the flattened region) with the same value, the set is part of that group, and the group limit stands for both. They differ
        // only when the group has regions under another header with a different final limit. Without such a group (none, or one a deeper header or the region
        // changes) the set limit would be lost, so the file is rejected. A polyphony in a <region> is not a set limit.
        const bool set_limit = acc.poly_level <= 2 && (acc.group == 0 || acc.group_level > acc.poly_level);
        if (set_limit) {
          issue(SfzIssueKind::kUnsupportedValue, Loc{acc.poly_op->file, acc.poly_op->line},
                "polyphony in a <global>/<master>/<group> header limits the voices of all the regions under that header (a region set); it is imported only together with "
                "a non-zero group=N set at that header or a broader one (then it is the group's limit), or in a <region>",
                "polyphony");
          return;
        }
        g.polyphony = std::uint16_t(acc.poly);
        if (acc.group != 0) g.group_polyphony = std::uint16_t(acc.poly);  // resolved per group at the end of the file
      }
      if (g.off_mode != OffMode::kTime) g.off_time = 0;
      if (!g.has_off_by) g.off_by = 0;
    }
    r.has_loop_points = ls.start_set || ls.end_set;
    // Story 5.1: filter, keyswitch and crossfade state.
    if (acc.cutoff_set) r.filter.type = acc.type_set ? acc.type : FilterType::kLpf2p;
    else {
      r.filter = Filter{};  // fil_type/resonance without cutoff: no filter (documented), but never silent
      for (const Op* o : {acc.type_op, acc.res_op})
        if (o && warned_.insert(o->file + ":" + std::to_string(o->line) + ":" + o->name).second)
          result.warnings.push_back({o->name, o->file, o->line,
                                     "opcode '" + o->name + "' has no effect without 'cutoff'; the region gets no filter"});
    }
    {  // Story 13.8: filter tuning (key track, velocity track, CC entries) and the second filter
      FilterMap fm;
      auto warn_none = [&](const Acc::FilterAcc& a, bool skip_type_res, const char* needs) {
        for (const Op* o : a.ops) {
          FilterOp fo;
          if (skip_type_res && filter_opcode(o->name, fo.idx, fo.kind) && (fo.kind == 'T' || fo.kind == 'R')) continue;  // the Story 5.1 loop above reports these
          if (warned_.insert(o->file + ":" + std::to_string(o->line) + ":" + o->name).second)
            result.warnings.push_back({o->name, o->file, o->line, "opcode '" + o->name + "' has no effect without '" + needs + "'; the region gets no such filter"});
        }
      };
      auto build = [&](const Acc::FilterAcc& a, FilterTune& t) {
        t.keytrack = a.keytrack;
        t.keycenter = a.keytrack != 0.0f ? std::uint8_t(a.keycenter) : std::uint8_t(60);  // the centre only matters with a key track; one encoding for "no tuning"
        t.veltrack = a.veltrack;
        for (int w = 0; w < 2; ++w) {
          const auto& m = w == 0 ? a.cut : a.res;
          FilterMod* out = w == 0 ? t.cutoff : t.res;
          std::uint8_t& cnt = w == 0 ? t.cutoff_count : t.res_count;
          for (const auto& [cc, e] : m) {
            if (!e.depth_set || e.depth == 0.0f) continue;  // a curve alone is no connection; a zero depth adds nothing to an additive target
            if (cnt >= kMaxFilterMods) {
              issue(SfzIssueKind::kUnsupportedValue, region_loc_,
                    std::string("more than ") + std::to_string(kMaxFilterMods) + (w == 0 ? " cutoff_oncc<N>" : " resonance_oncc<N>") + " entries on one filter",
                    w == 0 ? "cutoff_oncc<N>" : "resonance_oncc<N>");
              return false;
            }
            out[cnt++] = {std::uint8_t(cc), std::uint8_t(e.curve), e.depth};
          }
        }
        return true;
      };
      if (acc.cutoff_set) {
        if (!build(acc.fil[0], fm.f1)) return;
      } else {
        warn_none(acc.fil[0], true, "cutoff");
      }
      const Acc::FilterAcc& a2 = acc.fil[1];
      if (a2.cutoff_set) {
        if (!acc.cutoff_set) {
          issue(SfzIssueKind::kUnsupportedValue, Loc{a2.cutoff_op->file, a2.cutoff_op->line},
                "cutoff2 on a region without 'cutoff': sfizz would run the missing first filter at cutoff 0, which this engine does not model", a2.cutoff_op->name);
          return;
        }
        fm.f2.type = a2.type_set ? a2.type : FilterType::kLpf2p;
        fm.f2.cutoff_hz = a2.cutoff;
        fm.f2.resonance_db = a2.resonance;
        if (!build(a2, fm.f2.tune)) return;
      } else {
        warn_none(a2, false, "cutoff2");
      }
      r.fmap = fm;
    }
    {  // Story 13.9: the dedicated LFOs and EGs, and the EQ bands
      ModMap mm;
      auto warn_mod = [&](const Op* o, const std::string& why) {
        if (o && warned_.insert(o->file + ":" + std::to_string(o->line) + ":" + o->name + ":mod").second)
          result.warnings.push_back({o->name, o->file, o->line, "opcode '" + o->name + "' has no effect " + why});
      };
      LfoSpec* lfo_out[3] = {&mm.amp, &mm.pitch, &mm.fil};
      for (int i = 0; i < 3; ++i) {
        const Acc::LfoAcc& la = acc.lfo[i];
        if (!la.used) continue;
        if (la.s.depth == 0.0f) { if (!la.depth_given) warn_mod(la.op, "without a depth (sfizz connects the LFO with depth 0); the region gets no such LFO"); continue; }
        if (i == 2 && !acc.cutoff_set) { warn_mod(la.op, "without 'cutoff'; the region gets no filter LFO"); continue; }
        *lfo_out[i] = la.s;
        lfo_out[i]->on = true;
      }
      EgSpec* eg_out[2] = {&mm.pitch_eg, &mm.fil_eg};
      for (int i = 0; i < 2; ++i) {
        const Acc::EgAcc& ga = acc.eg[i];
        if (!ga.used) continue;
        if (ga.s.depth == 0.0f && ga.s.vel2depth == 0.0f) { if (!ga.depth_given) warn_mod(ga.op, "without a depth (sfizz connects the EG with depth 0); the region gets no such EG"); continue; }
        if (i == 1 && !acc.cutoff_set) { warn_mod(ga.op, "without 'cutoff'; the region gets no filter EG"); continue; }
        *eg_out[i] = ga.s;
        eg_out[i]->on = true;
      }
      unsigned bands = 0;
      for (unsigned b = 0; b < kMaxEqBands; ++b)
        if (acc.eq[b].used) bands = b + 1;
      float freq[kMaxEqBands] = {};
      for (unsigned b = 0; b < bands; ++b) freq[b] = acc.eq[b].freq;
      // sfizz Synth.cpp: the 50 / 500 / 5000 Hz defaults apply one after the other, each only if the bands before it are at the default as well.
      if (bands > 0 && freq[0] == 0.0f) {
        freq[0] = 50.0f;
        if (bands > 1 && freq[1] == 0.0f) {
          freq[1] = 500.0f;
          if (bands > 2 && freq[2] == 0.0f) freq[2] = 5000.0f;
        }
      }
      mm.eq_count = std::uint8_t(bands);
      for (unsigned b = 0; b < bands; ++b) {
        if (acc.eq[b].gain == 0.0f && acc.eq[b].vel2gain == 0.0f && !has_gain_entry(acc.eq[b])) continue;  // transparent: stays the default band
        const Acc::EqAcc& ea = acc.eq[b];
        EqBand& q = mm.eq[b];
        q.type = ea.type;
        q.freq = freq[b];
        q.bw = ea.bw;
        q.gain = ea.gain;
        q.vel2freq = ea.vel2freq;
        q.vel2gain = ea.vel2gain;
        for (int w = 0; w < 3; ++w) {
          FilterMod* out = w == 0 ? q.freq_mods : w == 1 ? q.bw_mods : q.gain_mods;
          std::uint8_t& cnt = w == 0 ? q.freq_count : w == 1 ? q.bw_count : q.gain_count;
          for (const auto& [cc, e] : ea.mods[w]) {
            if (!e.depth_set || e.depth == 0.0f) continue;
            if (cnt >= kMaxFilterMods) {
              issue(SfzIssueKind::kUnsupportedValue, region_loc_,
                    std::string("more than ") + std::to_string(kMaxFilterMods) + " eqN_" + (w == 0 ? "freq" : w == 1 ? "bw" : "gain") + "_oncc<N> entries on one band",
                    std::string("eqN_") + (w == 0 ? "freq" : w == 1 ? "bw" : "gain") + "_oncc<N>");
              return;
            }
            out[cnt++] = {std::uint8_t(cc), std::uint8_t(e.curve), e.depth};
          }
        }
      }
      while (mm.eq_count > 0 && mm.eq[mm.eq_count - 1] == EqBand{}) --mm.eq_count;  // no trailing transparent band: it cannot change the sound
      r.mod = mm;
    }
    if (r.sw.lokey >= 0 && r.sw.hikey >= 0 && r.sw.lokey > r.sw.hikey) {
      issue(SfzIssueKind::kInvalidValue, region_loc_, "sw_lokey > sw_hikey");
      return;
    }
    for (int dir = 0; dir < 2; ++dir) {
      const auto& m = dir == 0 ? acc.in : acc.out;
      const char* nm = dir == 0 ? "xfin" : "xfout";
      if (m.size() > kMaxCcCrossfades) {
        issue(SfzIssueKind::kUnsupportedValue, region_loc_,
              std::string("more than ") + std::to_string(kMaxCcCrossfades) + " " + nm +
                  " controllers in one region",
              std::string(nm) + "_locc<N>");
        return;
      }
      CcRange* out = dir == 0 ? r.xf.in : r.xf.out;
      std::uint8_t& count = dir == 0 ? r.xf.in_count : r.xf.out_count;
      count = 0;
      for (const auto& [cc, part] : m) {
        if (part.lo > part.hi) {
          issue(SfzIssueKind::kInvalidValue, region_loc_,
                std::string(nm) + "_locc" + std::to_string(cc) + " > " + nm + "_hicc" + std::to_string(cc) +
                    " (give both opcodes)");
          return;
        }
        out[count++] = {std::uint8_t(cc), part.lo, part.hi};
      }
    }
    if (r.key_lo > r.key_hi || r.vel_lo > r.vel_hi || r.lorand > r.hirand ||
        r.seq_position > r.seq_length) {
      issue(SfzIssueKind::kInvalidValue, region_loc_, "region has an inverted or inconsistent range (key, velocity, rand or seq)");
      return;
    }
    if (ls.start_set && ls.end_set && r.loop_start > r.loop_end) {
      issue(SfzIssueKind::kInvalidValue, region_loc_, "loop_start > loop_end");
      return;
    }
    if (!sample_op) {
      issue(SfzIssueKind::kMissingSampleOpcode, region_loc_, "region without a sample opcode");
      return;
    }
    const std::string& sample_value = sample_op->value;
    if (sample_value.empty() || sample_value[0] == '*') {
      add(SfzIssueKind::kUnsupportedValue, sample_op->file, sample_op->line,
          "unsupported sample '" + sample_value + "' (generators and empty samples are not supported)");
      return;
    }
    const fs::path p = (fs::path(sample_value).is_absolute() ? fs::path(sample_value) : dir_ / sample_value).lexically_normal();
    std::size_t idx = samples_.size();
    auto it = samples_.find(p.string());
    if (it != samples_.end()) {
      idx = it->second;
    } else {
      std::error_code ec;
      if (!fs::is_regular_file(p, ec)) {
        if (add(SfzIssueKind::kMissingSample, sample_op->file, sample_op->line, "sample file not found: " + p.string()))
          result.issues.back().path = p.string();  // only the issue just added
        return;
      }
      SfzSample s;
      s.name = p.lexically_relative(dir_).generic_string();
      if (s.name.size() > layout::kMaxStringLen) {
        issue(SfzIssueKind::kInvalidValue, region_loc_, "sample path too long");
        return;
      }
      s.path = p.string();
      s.ref_file = sample_op->file;
      s.ref_line = sample_op->line;
      samples_[p.string()] = idx;
      result.samples.push_back(std::move(s));
    }
    r.sample = static_cast<std::uint32_t>(idx);
    result.regions.push_back(r);
    result.never_note_triggered.push_back(acc.never_note && r.trig.count == 0);  // Story 13.7: a controller trigger keeps the region
    result.loops.push_back(ls);
    result.region_sources.push_back({region_loc_.file, region_loc_.line});
  }

  fs::path main_, dir_;
  Cur cur_ = Cur::kNone;
  Level global_, master_, group_, region_;
  Loc region_loc_;
  std::string default_path_;
  std::map<std::string, std::size_t> samples_;
  std::set<std::string> seen_, warned_;
  std::map<int, SfzCcDefault> cc_defaults_;  // Story 13.5, last value of each controller wins
  bool report_ = false;
  int cc_ = 0;
};

// sfizz CurveSet (Curve.cpp): seven predefined slots; a <curve> without curve_index is appended (and is dropped once any curve had an explicit index);
// an explicit curve_index replaces or extends. The result is the explicit definitions, by final index (0..255); later blocks win.
std::vector<CurveDef> resolve_curves(const std::vector<SfzCurve>& curves) {
  std::map<int, CurveDef> out;
  bool explicit_mode = false;
  std::size_t size = 7;
  for (const SfzCurve& c : curves) {
    int idx = 0;
    if (c.index < 0) {
      if (explicit_mode) continue;
      idx = static_cast<int>(size++);
    } else {
      idx = c.index;
      explicit_mode = true;
      if (static_cast<std::size_t>(idx) >= size) size = static_cast<std::size_t>(idx) + 1;
    }
    if (idx > 255) continue;
    std::map<int, float> pts;  // vNNN: only 0..127 exist, the last of a number wins
    for (const auto& [pt, v] : c.points) {
      if (pt < 0 || pt > 127) continue;
      float f = static_cast<float>(v);
      if (f == 0.0f) f = 0.0f;  // "-0" is stored as +0: one encoding per value
      pts[pt] = f;
    }
    CurveDef def;
    def.index = static_cast<std::uint8_t>(idx);
    for (const auto& [x, y] : pts) def.points.push_back({static_cast<std::uint8_t>(x), y});
    out[idx] = std::move(def);
  }
  std::vector<CurveDef> v;
  for (auto& [i, d] : out) v.push_back(std::move(d));
  return v;
}

// Story 13.5: the instrument-level setup the bundle stores: the set_cc values for controllers some region (gate, modulation or crossfade) reads, and the
// <curve> tables the regions' modulation entries name. Everything else is dropped so a library with <curve> blocks or set_cc nothing uses keeps its bundle.
void build_cc_setup(SfzParseResult& r, bool report) {
  std::set<int> used_cc, used_curve;
  for (const Region& g : r.regions) {
    for (unsigned i = 0; i < g.cc.gate_count; ++i) used_cc.insert(g.cc.gates[i].cc);
    for (unsigned i = 0; i < g.trig.count; ++i) used_cc.insert(g.trig.t[i].cc);  // Story 13.7
    for (unsigned i = 0; i < g.cc.mod_count; ++i) {
      if (g.cc.mods[i].cc <= 127) used_cc.insert(g.cc.mods[i].cc);
      used_curve.insert(g.cc.mods[i].curve);
    }
    for (const FilterTune* t : {&g.fmap.f1, &g.fmap.f2.tune})  // Story 13.8
      for (int w = 0; w < 2; ++w) {
        const FilterMod* e = w == 0 ? t->cutoff : t->res;
        for (unsigned i = 0; i < (w == 0 ? t->cutoff_count : t->res_count); ++i) {
          if (e[i].cc <= 127) used_cc.insert(e[i].cc);
          used_curve.insert(e[i].curve);
        }
      }
    for (unsigned b = 0; b < g.mod.eq_count; ++b) {  // Story 13.9
      const EqBand& q = g.mod.eq[b];
      for (int w = 0; w < 3; ++w) {
        const FilterMod* e = w == 0 ? q.freq_mods : w == 1 ? q.bw_mods : q.gain_mods;
        for (unsigned i = 0; i < (w == 0 ? q.freq_count : w == 1 ? q.bw_count : q.gain_count); ++i) {
          if (e[i].cc <= 127) used_cc.insert(e[i].cc);
          used_curve.insert(e[i].curve);
        }
      }
    }
    for (unsigned i = 0; i < g.xf.in_count; ++i) used_cc.insert(g.xf.in[i].cc);
    for (unsigned i = 0; i < g.xf.out_count; ++i) used_cc.insert(g.xf.out[i].cc);
  }
  InstrumentCcSetup out;
  for (const SfzCcDefault& d : r.cc_defaults) {
    if (used_cc.count(d.cc)) {
      out.defaults.push_back({static_cast<std::uint8_t>(d.cc), d.value});  // kept even when equal to the engine's value: binding this bank then restores it
    } else if (report) {
      r.ignored_values.push_back({"set_cc" + std::to_string(d.cc), std::to_string(d.value), d.file, d.line,
                                  "set_cc" + std::to_string(d.cc) + " seeds a controller no region reads (no gate, trigger, modulation or crossfade); it is not stored"});
    }
  }
  for (CurveDef& c : resolve_curves(r.curves))
    if (used_curve.count(c.index)) out.curves.push_back(std::move(c));
  r.cc_setup = std::move(out);
}

// Story 13.6: sfizz sets a group's polyphony each time a region or a <group> header gives a group number and a polyphony, and the last one wins for the whole
// group (VoiceManager::setGroupPolyphony). Every region of a group carries the final limit, so the engine needs no group table.
void resolve_group_polyphony(SfzParseResult& r) {
  std::map<std::int32_t, std::uint16_t> limit;
  for (const Region& g : r.regions)
    if (g.vg.group != 0 && g.vg.group_polyphony != 0) limit[g.vg.group] = g.vg.group_polyphony;
  for (Region& g : r.regions) {
    const auto it = limit.find(g.vg.group);
    g.vg.group_polyphony = (g.vg.group != 0 && it != limit.end()) ? it->second : std::uint16_t(0);
  }
}

}  // namespace

std::size_t drop_never_note_triggered(SfzParseResult& r) {
  std::size_t out = 0, dropped = 0;
  for (std::size_t i = 0; i < r.regions.size(); ++i) {
    if (i < r.never_note_triggered.size() && r.never_note_triggered[i]) { ++dropped; continue; }
    if (out != i) {
      r.regions[out] = r.regions[i];
      r.loops[out] = r.loops[i];
      r.region_sources[out] = r.region_sources[i];
    }
    ++out;
  }
  r.regions.resize(out);
  r.loops.resize(out);
  r.region_sources.resize(out);
  r.never_note_triggered.assign(out, false);
  if (dropped) build_cc_setup(r, false);
  return dropped;
}

bool resolve_loop_points(Region& r, const SfzLoopSpec& spec, std::uint64_t frames, std::string* why) {
  const bool loops = r.loop_mode == LoopMode::kLoopContinuous || r.loop_mode == LoopMode::kLoopSustain;
  if (!spec.start_set && !spec.end_set && !loops) {
    r.has_loop_points = false;
    r.loop_start = r.loop_end = 0;
    return true;
  }
  if (frames == 0) {
    if (why) *why = "the sample is empty, it cannot loop";
    return false;
  }
  if (!spec.start_set) r.loop_start = 0;
  if (!spec.end_set) r.loop_end = frames - 1;
  r.has_loop_points = true;
  if (r.loop_start > r.loop_end) {
    if (why) *why = spec.end_set ? "loop_start > loop_end" : "loop_start is beyond the last frame of the sample";
    return false;
  }
  return true;
}

SfzParseResult parse_sfz_file(const std::string& sfz_path) {
  SfzParseResult none;
  std::error_code ec;
  fs::path abs = fs::absolute(sfz_path, ec);
  if (ec || !fs::is_regular_file(abs, ec)) {
    none.issues.push_back({SfzIssueKind::kParse, sfz_path, 0, "cannot open SFZ file", sfz_path, {}});
    return none;
  }
  abs = abs.lexically_normal();
  Importer imp(abs);
  try {
    sfz::Parser parser;
    parser.setListener(&imp);
    parser.setMaximumIncludeDepth(16);
    parser.parseFile(abs);
    for (const auto& f : parser.getIncludedFiles()) imp.result.included_files.push_back(f);
  } catch (const std::exception& e) {
    imp.result.issues.push_back({SfzIssueKind::kParse, abs.string(), 0, std::string("parser failure: ") + e.what(), abs.string(), {}});
  }
  for (auto& i : imp.result.issues)
    if (i.kind == SfzIssueKind::kParse && i.message.find("Exceeded maximum include depth") != std::string::npos)
      i.message += " (recursive #include?)";
  std::sort(imp.result.included_files.begin(), imp.result.included_files.end());
  build_cc_setup(imp.result, true);
  resolve_group_polyphony(imp.result);
  return std::move(imp.result);
}

}  // namespace velora::format
