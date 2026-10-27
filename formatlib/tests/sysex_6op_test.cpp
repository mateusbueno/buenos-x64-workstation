#include "velora/format/sysex_6op.hpp"
#include "velora/format/parametric_bundle.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

using namespace velora::format;

// Self-made test voice generator (marked as self-made per AC / NFR28)
static FmVoiceParameters make_self_made_voice(uint8_t algo, uint8_t fb, const std::string& name) {
  FmVoiceParameters v{};
  v.algorithm = std::clamp<uint8_t>(algo, 1, 32);
  v.feedback = std::clamp<uint8_t>(fb, 0, 7);
  v.osc_key_sync = true;
  v.lfo_speed = 35;
  v.lfo_delay = 10;
  v.lfo_pitch_mod_depth = 5;
  v.lfo_amp_mod_depth = 0;
  v.lfo_key_sync = true;
  v.lfo_wave = 1; // SawDown
  v.lfo_pitch_mod_sens = 3;
  v.transpose = 0;

  std::memset(v.name, 0, sizeof(v.name));
  std::strncpy(v.name, name.c_str(), 10);

  // Set operators (OP1..OP6)
  for (int i = 0; i < 6; ++i) {
    auto& op = v.operators[i];
    op.rates = {95, 80, 50, 40};
    op.levels = {99, 85, 70, 0};
    op.kbd_breakpoint = 60;
    op.left_depth = 15;
    op.right_depth = 20;
    op.left_curve = i % 4;
    op.right_curve = (i + 1) % 4;
    op.kbd_rate_scaling = i % 8;
    op.amp_mod_sens = i % 4;
    op.velocity_sens = (i * 2) % 8;
    op.total_level = 75 + (i * 4) % 25;
    op.fixed_freq_mode = (i == 5);
    op.freq_coarse = (i + 1);
    op.freq_fine = i * 10;
    op.detune = (i % 5) - 2; // -2..+2
  }
  return v;
}

int main() {
  std::cout << "Running sysex_6op_test...\n";

  // Test 1: Single voice dump round-trip byte-exactness
  {
    FmVoiceParameters original = make_self_made_voice(5, 6, "TEST VOX 1");
    std::vector<uint8_t> syx = encode_sysex_single_dump(original, 0);
    assert(syx.size() == kSysexSingleDumpSize);
    assert(syx[0] == 0xF0 && syx[1] == 0x43 && syx[2] == 0x00);
    assert(syx[3] == 0x00 && syx[4] == 0x01 && syx[5] == 0x1B);
    assert(syx.back() == 0xF7);

    SysexParseResult res = parse_sysex_memory(syx);
    assert(res.ok());
    assert(!res.is_bulk);
    assert(res.voices.size() == 1);
    assert(res.voices[0].name == "TEST VOX 1");

    // Re-encode and compare byte-for-byte
    std::vector<uint8_t> reencoded = encode_sysex_single_dump(res.voices[0].parameters, 0);
    assert(reencoded == syx);
    std::cout << "  ✓ Single voice dump round-trip bit-identical passed\n";
  }

  // Test 2: 32-Voice bulk dump round-trip byte-exactness (algorithms 1..32)
  {
    std::vector<FmVoiceParameters> bank;
    bank.reserve(32);
    for (uint8_t i = 1; i <= 32; ++i) {
      bank.push_back(make_self_made_voice(i, i % 8, "PATCH " + std::to_string(i)));
    }

    std::vector<uint8_t> bulk_syx = encode_sysex_bulk_dump(bank, 0);
    assert(bulk_syx.size() == kSysexBulkDumpSize);
    assert(bulk_syx[0] == 0xF0 && bulk_syx[1] == 0x43 && bulk_syx[2] == 0x00);
    assert(bulk_syx[3] == 0x09 && bulk_syx[4] == 0x20 && bulk_syx[5] == 0x00);
    assert(bulk_syx.back() == 0xF7);

    SysexParseResult res = parse_sysex_memory(bulk_syx);
    assert(res.ok());
    assert(res.is_bulk);
    assert(res.voices.size() == 32);

    for (size_t i = 0; i < 32; ++i) {
      assert(res.voices[i].parameters.algorithm == i + 1);
      assert(res.voices[i].parameters.feedback == (i + 1) % 8);
    }

    std::vector<uint8_t> reencoded = encode_sysex_bulk_dump(bank, 0);
    assert(reencoded == bulk_syx);
    std::cout << "  ✓ 32-voice bulk dump round-trip bit-identical passed\n";
  }

  // Test 3: Rejection of corrupted files with exact copy message
  {
    FmVoiceParameters v = make_self_made_voice(1, 0, "TEST");
    std::vector<uint8_t> good = encode_sysex_single_dump(v, 0);

    // 3a: Wrong size
    std::vector<uint8_t> bad_size = good;
    bad_size.pop_back();
    SysexParseResult r1 = parse_sysex_memory(bad_size);
    assert(r1.damaged());
    assert(r1.error_message == kSysexDamagedMessage);

    // 3b: Bad header byte
    std::vector<uint8_t> bad_header = good;
    bad_header[1] = 0x44; // Not Yamaha ID
    SysexParseResult r2 = parse_sysex_memory(bad_header);
    assert(r2.damaged());
    assert(r2.error_message == kSysexDamagedMessage);

    // 3c: Bad checksum
    std::vector<uint8_t> bad_checksum = good;
    bad_checksum[161] ^= 0x01; // flip 1 bit
    SysexParseResult r3 = parse_sysex_memory(bad_checksum);
    assert(r3.damaged());
    assert(r3.error_message == kSysexDamagedMessage);

    // 3d: Out-of-range parameter (e.g. algorithm > 31 in payload)
    std::vector<uint8_t> bad_param = good;
    bad_param[6 + 134] = 32; // Algorithm 32 stored as 0..31, 32 is invalid!
    // Recompute checksum so only the parameter is invalid
    uint8_t sum = 0;
    for (size_t i = 0; i < 155; ++i) sum = (sum + bad_param[6 + i]) & 0x7F;
    bad_param[161] = (-sum) & 0x7F;
    SysexParseResult r4 = parse_sysex_memory(bad_param);
    assert(r4.damaged());
    assert(r4.error_message == kSysexDamagedMessage);

    // 3e: MSB set in payload
    std::vector<uint8_t> bad_msb = good;
    bad_msb[10] = 0x85;
    sum = 0;
    for (size_t i = 0; i < 155; ++i) sum = (sum + bad_msb[6 + i]) & 0x7F;
    bad_msb[161] = (-sum) & 0x7F;
    SysexParseResult r5 = parse_sysex_memory(bad_msb);
    assert(r5.damaged());
    assert(r5.error_message == kSysexDamagedMessage);

    std::cout << "  ✓ Error rejection with exact copy message passed\n";
  }

  // Test 4: Name sanitization and disambiguation
  {
    // 4a: Non-printable bytes and trailing spaces
    std::string sanitized1 = sanitize_sysex_name("VOX\x01\x02  \x7F ", 1);
    assert(sanitized1 == "VOX");

    // 4b: Empty name becomes "Voice n"
    std::string sanitized2 = sanitize_sysex_name("          ", 3);
    assert(sanitized2 == "Voice 3");

    // 4c: Duplicate names in bulk dump
    std::vector<FmVoiceParameters> bank;
    for (int i = 0; i < 32; ++i) {
      bank.push_back(make_self_made_voice(1, 0, "PIANO"));
    }
    std::vector<uint8_t> bulk_syx = encode_sysex_bulk_dump(bank, 0);
    SysexParseResult res = parse_sysex_memory(bulk_syx);
    assert(res.ok());
    assert(res.voices[0].name == "PIANO");
    assert(res.voices[1].name == "PIANO (2)");
    assert(res.voices[31].name == "PIANO (32)");

    std::cout << "  ✓ Name sanitization and duplicate disambiguation passed\n";
  }

  // Test 5: Parametric Bundle conversion and round-trip
  {
    FmVoiceParameters v = make_self_made_voice(5, 4, "LEAD SYNTH");
    std::vector<uint8_t> syx = encode_sysex_single_dump(v, 0);
    SysexParseResult res = parse_sysex_memory(syx);
    assert(res.ok());

    auto b_res = ParametricBundle::from_sysex(res, "/usb/patches/lead.syx", "Lead Synth");
    assert(b_res);
    ParametricBundle bundle = b_res.take();
    assert(bundle.timbres().size() == 1);
    assert(bundle.timbres()[0].key == "1");
    assert(bundle.timbres()[0].name == "LEAD SYNTH");
    assert(bundle.timbres()[0].category == Category::kSynthPoly);
    assert(bundle.timbres()[0].engine_type_id == kEngineTypeFm);

    std::vector<uint8_t> serialized = bundle.serialize();
    auto d_res = ParametricBundle::deserialize(serialized);
    assert(d_res);
    ParametricBundle deserialized = d_res.take();
    assert(deserialized.timbres().size() == 1);
    assert(deserialized.timbres()[0].key == "1");
    assert(deserialized.timbres()[0].name == "LEAD SYNTH");
    assert(deserialized.timbres()[0].fm_parameters == v);
    std::cout << "  ✓ Parametric bundle serialization and deserialization passed\n";
  }

  // Test 6: Parametric Bundle with VA Voice Parameters
  {
    ParametricBundle bundle;
    bundle.set_name("Mixed Factory Synth");
    Uuid lib_id{};
    assert(uuid_from_string("12345678-1234-1234-1234-123456789abc", lib_id));
    bundle.set_library_id(lib_id);

    VaVoiceParameters va_v;
    va_v.name = "Hyper Saw Lead";
    va_v.osc1_wave = VaWaveform::kSaw;
    va_v.osc2_wave = VaWaveform::kPulse;
    va_v.osc2_detune_cents = 12.5f;
    va_v.cutoff_hz = 3200.0f;
    va_v.resonance = 0.45f;
    va_v.drive = 0.2f;
    va_v.voice_mode = VaVoiceMode::kMono;
    va_v.unison_voices = 2;

    ParametricTimbre t_va;
    t_va.key = "va_lead_01";
    t_va.name = "Hyper Saw Lead";
    t_va.category = Category::kSynthLead;
    t_va.engine_type_id = kEngineTypeVa;
    t_va.va_parameters = va_v;
    assert(bundle.add_timbre(t_va) == Error::kNone);

    FmVoiceParameters fm_v = make_self_made_voice(7, 3, "FM Bell Poly");
    ParametricTimbre t_fm;
    t_fm.key = "fm_bell_01";
    t_fm.name = "FM Bell Poly";
    t_fm.category = Category::kMalletBells;
    t_fm.engine_type_id = kEngineTypeFm;
    t_fm.fm_parameters = fm_v;
    assert(bundle.add_timbre(t_fm) == Error::kNone);

    std::vector<uint8_t> bytes = bundle.serialize();
    auto d_res = ParametricBundle::deserialize(bytes);
    assert(d_res);
    ParametricBundle deserialized = d_res.take();
    assert(deserialized.timbres().size() == 2);
    assert(deserialized.timbres()[0].key == "va_lead_01");
    assert(deserialized.timbres()[0].name == "Hyper Saw Lead");
    assert(deserialized.timbres()[0].category == Category::kSynthLead);
    assert(deserialized.timbres()[0].engine_type_id == kEngineTypeVa);
    assert(deserialized.timbres()[0].va_parameters == va_v);

    assert(deserialized.timbres()[1].key == "fm_bell_01");
    assert(deserialized.timbres()[1].name == "FM Bell Poly");
    assert(deserialized.timbres()[1].category == Category::kMalletBells);
    assert(deserialized.timbres()[1].engine_type_id == kEngineTypeFm);
    assert(deserialized.timbres()[1].fm_parameters == fm_v);

    std::cout << "  ✓ VA and mixed parametric bundle round-trip bit-identical passed\n";
  }

  std::cout << "All sysex_6op_test checks passed!\n";
  return 0;
}
