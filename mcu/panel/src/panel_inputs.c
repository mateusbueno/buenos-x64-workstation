/*
 * Panel MCU firmware — inputs implementation (Story 3.8).
 * Portable C11: no OS, no heap, no stdio, no floating point.
 */
#include "mcu/panel_inputs.h"

#include <string.h>

static const uint8_t kEncoderIds[MCU_PANEL_ENCODER_COUNT] = {
    MCU_CTRL_ENCODER_1,     MCU_CTRL_ENCODER_2, MCU_CTRL_ENCODER_3,
    MCU_CTRL_ENCODER_4,     MCU_CTRL_ENCODER_5, MCU_CTRL_ENCODER_6,
    MCU_CTRL_ENCODER_7,     MCU_CTRL_ENCODER_8, MCU_CTRL_VALUE_ENCODER};

static const uint8_t kAnalogIds[MCU_PANEL_ANALOG_COUNT] = {
    MCU_CTRL_FADER_1,          MCU_CTRL_FADER_2,       MCU_CTRL_FADER_3,
    MCU_CTRL_FADER_4,          MCU_CTRL_FADER_5,       MCU_CTRL_FADER_6,
    MCU_CTRL_FADER_7,          MCU_CTRL_FADER_8,       MCU_CTRL_FADER_9,
    MCU_CTRL_WHEEL_PITCH,      MCU_CTRL_WHEEL_MOD,     MCU_CTRL_PEDAL_SUSTAIN,
    MCU_CTRL_PEDAL_EXPRESSION};

static const uint8_t kButtonIds[MCU_PANEL_BUTTON_COUNT] = {
    MCU_BTN_PART_SCENE_1,          MCU_BTN_PART_SCENE_2,
    MCU_BTN_PART_SCENE_3,          MCU_BTN_PART_SCENE_4,
    MCU_BTN_PART_SCENE_5,          MCU_BTN_PART_SCENE_6,
    MCU_BTN_PART_SCENE_7,          MCU_BTN_PART_SCENE_8,
    MCU_BTN_LIVE_SET_PREV,         MCU_BTN_LIVE_SET_NEXT,
    MCU_BTN_SCENE_PREV,            MCU_BTN_SCENE_NEXT,
    MCU_BTN_EXIT,                  MCU_BTN_MENU,
    MCU_BTN_WRITE,                 MCU_BTN_VALUE_PUSH,
    MCU_BTN_ROTARY_STOP,           MCU_BTN_ROTARY_SF,
    MCU_BTN_TOGGLE_DRAWBARS_MIXER, MCU_BTN_TOGGLE_PARTS_SCENES,
    MCU_BTN_TOGGLE_PART_ALL_ASSIGN};

static void emit_frame(mcu_panel_inputs* p, mcu_frame* f, uint32_t tick_us) {
  f->version = MCU_PROTOCOL_VERSION;
  f->seq = p->next_seq++;
  f->tick = tick_us;
  if (p->tx) {
    p->tx(p->tx_user, f);
  }
}

static uint16_t normalize_to_12bit(uint16_t raw, uint8_t adc_bits) {
  uint32_t max_raw = (1u << adc_bits) - 1u;
  uint32_t clamped = raw > max_raw ? max_raw : (uint32_t)raw;
  if (adc_bits == 12u) {
    return (uint16_t)clamped;
  }
  if (adc_bits < 12u) {
    uint8_t shift = (uint8_t)(12u - adc_bits);
    /* Scale full-scale to 4095 so max raw reaches 4095 */
    uint32_t shifted = (clamped << shift) | (clamped >> (adc_bits - shift));
    return (uint16_t)(shifted > MCU_PANEL_ADC_MAX_VALUE ? MCU_PANEL_ADC_MAX_VALUE : shifted);
  }
  return (uint16_t)(clamped >> (adc_bits - 12u));
}

int mcu_panel_inputs_init(mcu_panel_inputs* p,
                          const mcu_panel_inputs_config* cfg,
                          mcu_panel_frame_tx_fn tx,
                          void* tx_user) {
  unsigned i;
  if (!p || !cfg) return MCU_ERR_NULL;
  if (cfg->adc_bits < MCU_PANEL_ADC_MIN_BITS || cfg->adc_bits > 16u) {
    return MCU_ERR_BAD_VALUE;
  }
  if (cfg->steps_per_detent != 1u && cfg->steps_per_detent != 2u &&
      cfg->steps_per_detent != 4u) {
    return MCU_ERR_BAD_VALUE;
  }
  if (cfg->report_interval_us == 0u ||
      cfg->report_interval_us > MCU_PANEL_MAX_REPORT_INTERVAL_US) {
    return MCU_ERR_BAD_VALUE;
  }

  memset(p, 0, sizeof(*p));
  p->cfg = *cfg;
  p->tx = tx;
  p->tx_user = tx_user;

  for (i = 0u; i < MCU_PANEL_ENCODER_COUNT; ++i) {
    p->encoders[i].control_id = kEncoderIds[i];
  }
  for (i = 0u; i < MCU_PANEL_ANALOG_COUNT; ++i) {
    p->analogs[i].control_id = kAnalogIds[i];
  }
  p->pedal_switch.id = MCU_CTRL_PEDAL_SWITCH;
  for (i = 0u; i < MCU_PANEL_BUTTON_COUNT; ++i) {
    p->buttons[i].id = kButtonIds[i];
  }
  return MCU_OK;
}

int mcu_panel_inputs_boot(mcu_panel_inputs* p,
                          uint16_t sustain_raw,
                          uint16_t expression_raw,
                          uint8_t switch_raw,
                          uint32_t tick_us) {
  uint16_t sus12;
  uint16_t exp12;
  uint8_t flags = 0u;
  mcu_frame f;
  mcu_hello h;

  if (!p) return MCU_ERR_NULL;

  sus12 = normalize_to_12bit(sustain_raw, p->cfg.adc_bits);
  exp12 = normalize_to_12bit(expression_raw, p->cfg.adc_bits);

  if (sus12 > 2048u) flags |= (uint8_t)MCU_PANEL_PEDAL_SUSTAIN_INVERTED;
  if (exp12 > 2048u) flags |= (uint8_t)MCU_PANEL_PEDAL_EXPRESSION_INVERTED;
  if (switch_raw != 0u) flags |= (uint8_t)MCU_PANEL_PEDAL_SWITCH_INVERTED;

  p->pedal_polarity_flags = flags;
  p->next_seq = 0u;

  /* Seed the switch pedal resting state (after polarity normalization, resting = 0). */
  p->pedal_switch.stable_state = 0u;
  p->pedal_switch.candidate_state = 0u;
  p->pedal_switch.candidate_since_tick = tick_us;
  p->pedal_switch.initialized = 1u;

  mcu_frame_init(&f, MCU_TYPE_HELLO, 0u, tick_us);
  h.fw_version = p->cfg.fw_version;
  h.protocol_version = MCU_PROTOCOL_VERSION;
  h.board_id = MCU_BOARD_PANEL;
  h.keybed_contacts = flags;
  if (mcu_pack_hello(&f, &h) != MCU_OK) return MCU_ERR_BAD_VALUE;
  emit_frame(p, &f, tick_us);
  return MCU_OK;
}

/*
 * Standard quadrature transition table indexed by ((prev_ab << 2) | curr_ab):
 * Valid CW:  00 -> 01 -> 11 -> 10 -> 00 (+1 each)
 * Valid CCW: 00 -> 10 -> 11 -> 01 -> 00 (-1 each)
 * Invalid 2-bit jump (00<->11, 01<->10): 0 (glitch)
 */
static const int8_t kQuadTable[16] = {
    0,  +1, -1, 0,
    -1, 0,  0,  +1,
    +1, 0,  0,  -1,
    0,  -1, +1, 0};

int mcu_panel_inputs_sample_encoder(mcu_panel_inputs* p,
                                    uint8_t control_id,
                                    uint8_t ab_bits,
                                    uint32_t tick_us) {
  mcu_panel_encoder_state* enc = NULL;
  unsigned i;
  uint8_t ab = (uint8_t)(ab_bits & 0x03u);
  uint8_t idx;
  int8_t step;

  if (!p) return MCU_ERR_NULL;
  for (i = 0u; i < MCU_PANEL_ENCODER_COUNT; ++i) {
    if (p->encoders[i].control_id == control_id) {
      enc = &p->encoders[i];
      break;
    }
  }
  if (!enc) return MCU_ERR_BAD_VALUE;

  if (!enc->initialized) {
    enc->prev_ab = ab;
    enc->initialized = 1u;
    return MCU_OK;
  }

  if (ab == enc->prev_ab) return MCU_OK;

  idx = (uint8_t)((enc->prev_ab << 2) | ab);
  step = kQuadTable[idx];
  if (step == 0) {
    /* 2-bit illegal jump: ignore noise glitch and keep previous state */
    enc->glitches += 1u;
    return MCU_OK;
  }

  enc->prev_ab = ab;
  enc->sub_accum = (int16_t)(enc->sub_accum + step);

  if (enc->sub_accum >= (int16_t)p->cfg.steps_per_detent ||
      enc->sub_accum <= -(int16_t)p->cfg.steps_per_detent) {
    int16_t detents = (int16_t)(enc->sub_accum / (int16_t)p->cfg.steps_per_detent);
    enc->sub_accum = (int16_t)(enc->sub_accum % (int16_t)p->cfg.steps_per_detent);
    if (detents > 127) detents = 127;
    if (detents < -127) detents = -127;

    mcu_frame f;
    mcu_control c;
    mcu_frame_init(&f, MCU_TYPE_CONTROL, 0u, tick_us);
    c.id = control_id;
    c.value = (uint16_t)detents;
    if (mcu_pack_control(&f, &c) == MCU_OK) {
      emit_frame(p, &f, tick_us);
    }
  }

  return MCU_OK;
}

int mcu_panel_inputs_sample_analog(mcu_panel_inputs* p,
                                   uint8_t control_id,
                                   uint16_t raw_adc,
                                   uint32_t tick_us) {
  mcu_panel_analog_state* ch = NULL;
  unsigned i;
  uint16_t sample12;
  uint16_t filtered;
  uint16_t diff;

  if (!p) return MCU_ERR_NULL;
  for (i = 0u; i < MCU_PANEL_ANALOG_COUNT; ++i) {
    if (p->analogs[i].control_id == control_id) {
      ch = &p->analogs[i];
      break;
    }
  }
  if (!ch) return MCU_ERR_BAD_VALUE;

  sample12 = normalize_to_12bit(raw_adc, p->cfg.adc_bits);
  if (!ch->initialized) {
    ch->filtered_12bit = sample12;
    ch->last_reported_12bit = sample12;
    ch->last_report_tick = tick_us;
    ch->initialized = 1u;
    ch->ever_reported = 1u;
    mcu_frame f;
    mcu_control c;
    mcu_frame_init(&f, MCU_TYPE_CONTROL, 0u, tick_us);
    c.id = control_id;
    c.value = sample12;
    if (mcu_pack_control(&f, &c) == MCU_OK) {
      emit_frame(p, &f, tick_us);
    }
    return MCU_OK;
  }

  /* Integer IIR smoothing: 3/4 old + 1/4 new, with endpoint snap so 0 and 4095 are reached cleanly. */
  if (sample12 == 0u || sample12 == MCU_PANEL_ADC_MAX_VALUE) {
    filtered = sample12;
  } else {
    filtered = (uint16_t)(((uint32_t)ch->filtered_12bit * 3u + (uint32_t)sample12 + 2u) >> 2);
  }
  ch->filtered_12bit = filtered;

  /* Rate limit to report_interval_us (<= 2000 us, i.e. >= 500 Hz). */
  if ((uint32_t)(tick_us - ch->last_report_tick) < p->cfg.report_interval_us) {
    return MCU_OK;
  }

  diff = filtered >= ch->last_reported_12bit
             ? (uint16_t)(filtered - ch->last_reported_12bit)
             : (uint16_t)(ch->last_reported_12bit - filtered);

  /* Report when change >= deadband, or when reaching an exact endpoint (0 or 4095) from a different value. */
  if (diff >= p->cfg.deadband_12bit ||
      ((filtered == 0u || filtered == MCU_PANEL_ADC_MAX_VALUE) &&
       filtered != ch->last_reported_12bit)) {
    mcu_frame f;
    mcu_control c;
    ch->last_reported_12bit = filtered;
    ch->last_report_tick = tick_us;
    mcu_frame_init(&f, MCU_TYPE_CONTROL, 0u, tick_us);
    c.id = control_id;
    c.value = filtered;
    if (mcu_pack_control(&f, &c) == MCU_OK) {
      emit_frame(p, &f, tick_us);
    }
  }

  return MCU_OK;
}

static int debounce_step(mcu_panel_button_state* b,
                         uint8_t normalized_state,
                         uint32_t debounce_us,
                         uint32_t tick_us,
                         uint8_t* out_changed) {
  *out_changed = 0u;
  if (!b->initialized) {
    b->stable_state = normalized_state;
    b->candidate_state = normalized_state;
    b->candidate_since_tick = tick_us;
    b->initialized = 1u;
    return 0;
  }

  if (normalized_state != b->candidate_state) {
    b->candidate_state = normalized_state;
    b->candidate_since_tick = tick_us;
    return 0;
  }

  if (b->candidate_state != b->stable_state &&
      (uint32_t)(tick_us - b->candidate_since_tick) >= debounce_us) {
    b->stable_state = b->candidate_state;
    *out_changed = 1u;
  }
  return 0;
}

int mcu_panel_inputs_sample_pedal_switch(mcu_panel_inputs* p,
                                         uint8_t raw_level,
                                         uint32_t tick_us) {
  uint8_t raw_bit = (uint8_t)(raw_level ? 1u : 0u);
  uint8_t inverted = (uint8_t)((p->pedal_polarity_flags & MCU_PANEL_PEDAL_SWITCH_INVERTED) ? 1u : 0u);
  uint8_t closed = (uint8_t)(raw_bit ^ inverted);
  uint8_t changed = 0u;

  if (!p) return MCU_ERR_NULL;
  debounce_step(&p->pedal_switch, closed, p->cfg.debounce_us, tick_us, &changed);
  if (changed) {
    mcu_frame f;
    mcu_control c;
    mcu_frame_init(&f, MCU_TYPE_CONTROL, 0u, tick_us);
    c.id = MCU_CTRL_PEDAL_SWITCH;
    c.value = p->pedal_switch.stable_state;
    if (mcu_pack_control(&f, &c) == MCU_OK) {
      emit_frame(p, &f, tick_us);
    }
  }
  return MCU_OK;
}

int mcu_panel_inputs_sample_button(mcu_panel_inputs* p,
                                   uint8_t button_id,
                                   uint8_t raw_pressed,
                                   uint32_t tick_us) {
  mcu_panel_button_state* btn = NULL;
  unsigned i;
  uint8_t pressed = (uint8_t)(raw_pressed ? 1u : 0u);
  uint8_t changed = 0u;

  if (!p) return MCU_ERR_NULL;
  for (i = 0u; i < MCU_PANEL_BUTTON_COUNT; ++i) {
    if (p->buttons[i].id == button_id) {
      btn = &p->buttons[i];
      break;
    }
  }
  if (!btn) return MCU_ERR_BAD_VALUE;

  debounce_step(btn, pressed, p->cfg.debounce_us, tick_us, &changed);
  if (changed) {
    mcu_frame f;
    mcu_button b;
    mcu_frame_init(&f, MCU_TYPE_BUTTON, 0u, tick_us);
    b.id = button_id;
    b.pressed = btn->stable_state;
    if (mcu_pack_button(&f, &b) == MCU_OK) {
      emit_frame(p, &f, tick_us);
    }
  }
  return MCU_OK;
}

int mcu_panel_inputs_on_soc_frame(mcu_panel_inputs* p,
                                  const mcu_frame* frame,
                                  uint32_t tick_us) {
  if (!p || !frame) return MCU_ERR_NULL;
  if (frame->type == MCU_TYPE_TIME_SYNC) {
    mcu_time_sync req;
    if (mcu_unpack_time_sync(frame, &req) != MCU_OK) return MCU_ERR_BAD_VALUE;
    if (req.reply == MCU_TIME_SYNC_REQUEST) {
      mcu_frame reply_frame;
      mcu_time_sync rep;
      rep.reply = MCU_TIME_SYNC_REPLY;
      rep.token = req.token;
      rep.tick = tick_us;
      mcu_frame_init(&reply_frame, MCU_TYPE_TIME_SYNC, 0u, tick_us);
      if (mcu_pack_time_sync(&reply_frame, &rep) == MCU_OK) {
        emit_frame(p, &reply_frame, tick_us);
      }
    }
  }
  return MCU_OK;
}
