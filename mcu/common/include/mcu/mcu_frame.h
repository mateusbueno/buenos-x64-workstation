/*
 * MCU frame protocol (HARDWARE-INTERFACES.md section 5), shared by the keybed MCU, the panel MCU and the SoC.
 *
 * Portable C11: no OS, no heap, no stdio, no floating point. All buffers belong to the caller (the decoder
 * owns one fixed internal buffer inside its own struct). All multi-byte fields are little-endian.
 *
 * Wire layout (12 + length bytes, length 0..MCU_MAX_PAYLOAD):
 *   [0] sync 0xA5  [1] version  [2] type  [3] length  [4..5] seq  [6..9] tick  [10..] payload  [..+1] CRC16
 * The CRC is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no xorout; check("123456789") = 0x29B1)
 * over every byte from version to the end of the payload, stored little-endian.
 *
 * Sequence: every CRC-valid frame consumes a seq (rejected ones too); duplicates / out-of-order are counted as reorders,
 * not losses; a delivered HELLO resets the expectation. See MCU-ID-MAP.md section 5.
 *
 * Version byte: high nibble major, low nibble minor. A receiver of major N accepts majors N and N-1 (any minor);
 * N-1 must be at least 1 (major 0 is never valid). See mcu_version_accepted().
 *
 * Payload layouts (see also the published MCU-ID-MAP.md):
 *   HELLO            5  fw_version u16, protocol_version u8, board_id u8, keybed_contacts u8
 *   KEY_CONTACT      3  key u8, contact u8, state u8 (1 closed, 0 open)
 *   AFTERTOUCH       3  target u8 (0xFF = channel pressure, else key index), value u16
 *   CONTROL          3  control_id u8, value u16 (unsigned absolute, or int16 two's complement step delta for encoders)
 *   BUTTON           2  button_id u8, state u8 (1 pressed, 0 released)
 *   LED_SET          3  led_id u8, brightness u8, arg u8 (colour index, or fill position for rings)
 *   TIME_SYNC        9  kind u8 (0 request, 1 reply), token u32 (echoed), tick u32 (reply: MCU tick; request: 0)
 *   ENTER_BOOTLOADER 4  magic u32 = MCU_BOOTLOADER_MAGIC
 */
#ifndef MCU_FRAME_H
#define MCU_FRAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MCU_SYNC 0xA5u
#define MCU_PROTOCOL_VERSION 0x10u /* 1.0: major 1, minor 0. Bump here (single place). */
#define MCU_HEADER_SIZE 10u        /* sync, version, type, length, seq(2), tick(4) */
#define MCU_CRC_SIZE 2u
#define MCU_MAX_PAYLOAD 64u
#define MCU_MAX_FRAME (MCU_HEADER_SIZE + MCU_MAX_PAYLOAD + MCU_CRC_SIZE)
#define MCU_BOOTLOADER_MAGIC 0x544F4F42u /* bytes 42 4F 4F 54 */

#define MCU_VERSION_MAJOR(v) ((uint8_t)((v) >> 4))
#define MCU_VERSION_MINOR(v) ((uint8_t)((v) & 0x0Fu))

typedef enum mcu_type {
  MCU_TYPE_HELLO = 1,
  MCU_TYPE_KEY_CONTACT = 2,
  MCU_TYPE_AFTERTOUCH = 3,
  MCU_TYPE_CONTROL = 4,
  MCU_TYPE_BUTTON = 5,
  MCU_TYPE_LED_SET = 6,
  MCU_TYPE_TIME_SYNC = 7,
  MCU_TYPE_ENTER_BOOTLOADER = 8
} mcu_type;

enum {
  MCU_LEN_HELLO = 5,
  MCU_LEN_KEY_CONTACT = 3,
  MCU_LEN_AFTERTOUCH = 3,
  MCU_LEN_CONTROL = 3,
  MCU_LEN_BUTTON = 2,
  MCU_LEN_LED_SET = 3,
  MCU_LEN_TIME_SYNC = 9,
  MCU_LEN_ENTER_BOOTLOADER = 4
};

enum { MCU_BOARD_KEYBED = 1, MCU_BOARD_PANEL = 2 };
enum { MCU_KEYBED_AFTERTOUCH_FLAG = 0x80u, MCU_KEYBED_CONTACTS_MASK = 0x7Fu };
enum { MCU_AFTERTOUCH_CHANNEL = 0xFF };
enum { MCU_TIME_SYNC_REQUEST = 0, MCU_TIME_SYNC_REPLY = 1 };

/* Typed results of encoding / payload packing. Negative values are errors. */
typedef enum mcu_status {
  MCU_OK = 0,
  MCU_ERR_BUFFER_TOO_SMALL = -1,
  MCU_ERR_PAYLOAD_TOO_LONG = -2,
  MCU_ERR_UNKNOWN_TYPE = -3,
  MCU_ERR_BAD_LENGTH = -4,  /* payload length does not match the type */
  MCU_ERR_BAD_VALUE = -5,   /* a payload field is out of its documented range */
  MCU_ERR_NULL = -6
} mcu_status;

typedef struct mcu_frame {
  uint8_t version;
  uint8_t type;
  uint16_t seq;
  uint32_t tick;
  uint8_t length;
  uint8_t payload[MCU_MAX_PAYLOAD];
} mcu_frame;

/* ---- CRC-16/CCITT-FALSE ---- */
uint16_t mcu_crc16_update(uint16_t crc, uint8_t byte);
uint16_t mcu_crc16(const uint8_t* data, size_t len); /* init 0xFFFF */

/* ---- version policy ---- */
/* Nonzero when a receiver whose current version is `current` accepts `version`. */
int mcu_version_accepted(uint8_t version, uint8_t current);

/* ---- length per type ---- */
/* Fixed payload length of a known type, or -1 for an unknown type. */
int mcu_payload_length(uint8_t type);

/* ---- encoder ---- */
/* Sets version = MCU_PROTOCOL_VERSION, length 0, zero payload. */
void mcu_frame_init(mcu_frame* f, uint8_t type, uint16_t seq, uint32_t tick);
/* Encodes into the caller's buffer and returns the frame length (> 0) or a negative mcu_status.
 * Checks that the type is known and the length matches it. */
int mcu_encode(const mcu_frame* f, uint8_t* buf, size_t cap);
/* Same without the type / length-vs-type check (only length <= MCU_MAX_PAYLOAD): for tests and tools. */
int mcu_encode_unchecked(const mcu_frame* f, uint8_t* buf, size_t cap);

/* ---- typed payloads ----
 * pack_* fill f->type, f->length and f->payload only: they never touch version, seq or tick, so call mcu_frame_init()
 * first (it sets the version) and then set seq/tick in any order. pack_* and unpack_* validate documented ranges and
 * return MCU_ERR_BAD_VALUE for: board_id not in {1,2}; keybed_contacts not in {0,2,3}; closed/pressed/reply above 1;
 * a control ID not in the ID map, or a value outside that control's range (encoders: int16 delta within -127..127;
 * faders, wheels, pedals: unsigned within their resolution); a button or LED ID not in the ID map. */
typedef struct mcu_hello {
  uint16_t fw_version;
  uint8_t protocol_version;
  uint8_t board_id;
  uint8_t keybed_contacts;
} mcu_hello;
typedef struct mcu_key_contact {
  uint8_t key;
  uint8_t contact;
  uint8_t closed; /* 0 or 1 */
} mcu_key_contact;
typedef struct mcu_aftertouch {
  uint8_t target; /* MCU_AFTERTOUCH_CHANNEL or key index */
  uint16_t value;
} mcu_aftertouch;
typedef struct mcu_control {
  uint8_t id;
  uint16_t value; /* absolute value, or int16 delta in two's complement for encoders */
} mcu_control;
typedef struct mcu_button {
  uint8_t id;
  uint8_t pressed; /* 0 or 1 */
} mcu_button;
typedef struct mcu_led_set {
  uint8_t id;
  uint8_t brightness;
  uint8_t arg;
} mcu_led_set;
typedef struct mcu_time_sync {
  uint8_t reply; /* 0 request, 1 reply */
  uint32_t token;
  uint32_t tick;
} mcu_time_sync;

int mcu_pack_hello(mcu_frame* f, const mcu_hello* m);
int mcu_pack_key_contact(mcu_frame* f, const mcu_key_contact* m);
int mcu_pack_aftertouch(mcu_frame* f, const mcu_aftertouch* m);
int mcu_pack_control(mcu_frame* f, const mcu_control* m);
int mcu_pack_button(mcu_frame* f, const mcu_button* m);
int mcu_pack_led_set(mcu_frame* f, const mcu_led_set* m);
int mcu_pack_time_sync(mcu_frame* f, const mcu_time_sync* m);
int mcu_pack_enter_bootloader(mcu_frame* f);

int mcu_unpack_hello(const mcu_frame* f, mcu_hello* m);
int mcu_unpack_key_contact(const mcu_frame* f, mcu_key_contact* m);
int mcu_unpack_aftertouch(const mcu_frame* f, mcu_aftertouch* m);
int mcu_unpack_control(const mcu_frame* f, mcu_control* m);
int mcu_unpack_button(const mcu_frame* f, mcu_button* m);
int mcu_unpack_led_set(const mcu_frame* f, mcu_led_set* m);
int mcu_unpack_time_sync(const mcu_frame* f, mcu_time_sync* m);
int mcu_unpack_enter_bootloader(const mcu_frame* f);

/* ---- streaming decoder ---- */
typedef enum mcu_event {
  MCU_EV_NONE = 0,
  MCU_EV_FRAME,            /* a valid, accepted frame is in *out */
  MCU_EV_CRC_ERROR,        /* a candidate frame failed its CRC and was abandoned (nothing delivered) */
  MCU_EV_FRAMING_ERROR,    /* a candidate had an impossible length field and was abandoned */
  MCU_EV_VERSION_REJECTED, /* intact frame of an unsupported major version; header + payload in *out */
  MCU_EV_BAD_LENGTH,       /* intact frame whose length does not match its type; in *out */
  MCU_EV_UNKNOWN_TYPE      /* intact frame of an unknown type; in *out */
} mcu_event;

/* Counters other than bytes_in are 32-bit and wrap at 2^32 events (unreachable in practice: it is hours of
 * continuous error traffic); bytes_in is 64-bit. */
typedef struct mcu_decoder_counters {
  uint64_t bytes_in;
  uint32_t frames_ok;            /* delivered frames */
  uint32_t crc_errors;
  uint32_t framing_errors;       /* length field larger than MCU_MAX_PAYLOAD */
  uint32_t resyncs;              /* candidates abandoned (CRC or framing error) */
  uint32_t dropped_bytes;        /* bytes discarded as noise or from abandoned candidates */
  uint32_t version_mismatches;
  uint32_t bad_lengths;
  uint32_t unknown_types;
  uint32_t seq_gaps;             /* number of gap events among delivered frames */
  uint32_t seq_lost;             /* total frames lost, summed over gaps */
  uint32_t seq_reorders;         /* duplicate or out-of-order seq (not a loss) */
  uint32_t overflow_bytes;       /* bytes refused because the caller did not drain events (contract violation) */
} mcu_decoder_counters;

typedef struct mcu_decoder {
  uint8_t buf[2u * MCU_MAX_FRAME];
  size_t n;
  uint8_t current_version;
  uint8_t have_seq;
  uint16_t last_seq;
  uint16_t last_seq_lost; /* frames lost before the most recently delivered frame (0 if none) */
  mcu_decoder_counters counters;
} mcu_decoder;

/* A current_version whose major is 0 is invalid (major 0 is never valid) and is replaced by MCU_PROTOCOL_VERSION. */
void mcu_decoder_init(mcu_decoder* d, uint8_t current_version);
void mcu_decoder_reset_stream(mcu_decoder* d); /* drop buffered bytes and sequence state, keep the counters */

/* Feeds one byte and returns the first event it produced. Further events may be pending in the internal buffer:
 * after any event other than MCU_EV_NONE keep calling mcu_decoder_poll() until it returns MCU_EV_NONE, then feed the
 * next byte. *out is written only for events that carry a frame. */
mcu_event mcu_decoder_feed(mcu_decoder* d, uint8_t byte, mcu_frame* out);
mcu_event mcu_decoder_poll(mcu_decoder* d, mcu_frame* out);

/* For callers that know the link went idle (or time out a partial frame): if the decoder is stalled waiting for the
 * rest of a candidate frame (for example a false header with a bogus length), abandons that candidate (counted as a
 * resync, its sync byte dropped) and rescans the bytes it already holds, returning the first event this produces
 * (MCU_EV_NONE if nothing). Pending events are returned first, untouched. Call it repeatedly (with mcu_decoder_poll()
 * after any event) until MCU_EV_NONE and mcu_decoder_pending() == 0 to drain every stalled candidate. A real partial
 * frame in progress is abandoned too, so only call it after an idle gap. */
mcu_event mcu_decoder_flush_stale(mcu_decoder* d, mcu_frame* out);
/* Number of bytes currently buffered. */
size_t mcu_decoder_pending(const mcu_decoder* d);

/* Convenience that follows the contract above: feeds `len` bytes and calls `cb` for every event (including errors;
 * frames are passed for MCU_EV_FRAME and the intact-but-rejected events, NULL otherwise). */
typedef void (*mcu_event_cb)(void* user, mcu_event ev, const mcu_frame* frame);
void mcu_decoder_push(mcu_decoder* d, const uint8_t* data, size_t len, mcu_event_cb cb, void* user);

#ifdef __cplusplus
}
#endif

#endif /* MCU_FRAME_H */
