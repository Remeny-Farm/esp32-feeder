// SPDX-License-Identifier: MIT
#include "protocol.h"

#include <stdio.h>
#include <string.h>

#include "feeder_limits.h"

namespace proto {

uint8_t crc8(const char* data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint8_t)data[i];
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

static int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// Strict unsigned decimal: 1 to 9 digits, nothing else. Returns false on error.
static bool parse_u32(const char* s, uint32_t* out) {
  size_t n = strlen(s);
  if (n == 0 || n > 9) return false;
  uint32_t v = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (uint32_t)(s[i] - '0');
  }
  *out = v;
  return true;
}

// Split body in place on single spaces. Empty tokens (double space, leading or
// trailing space) make the line malformed. Returns the token count, or -1.
static int split(char* body, char** tok, int max_tok) {
  int n = 0;
  char* p = body;
  for (;;) {
    if (*p == '\0' || *p == ' ') return -1;  // empty token
    if (n >= max_tok) return -1;
    tok[n++] = p;
    while (*p != '\0' && *p != ' ') {
      if (*p < 0x21 || *p > 0x7E) return -1;  // control or non-ASCII byte
      p++;
    }
    if (*p == '\0') return n;
    *p++ = '\0';
  }
}

Err parse_line(const char* line, Command* out) {
  memset(out, 0, sizeof(*out));
  out->cmd = CMD_NONE;

  size_t len = strlen(line);
  if (len > 0 && line[len - 1] == '\r') len--;
  if (len < 4 || len > MAX_LINE_LEN) return E_BAD_CRC;
  if (line[len - 3] != '*') return E_BAD_CRC;
  int h = hex_val(line[len - 2]);
  int l = hex_val(line[len - 1]);
  if (h < 0 || l < 0) return E_BAD_CRC;
  size_t body_len = len - 3;
  if (crc8(line, body_len) != (uint8_t)((h << 4) | l)) return E_BAD_CRC;

  // The checksum is good from here on: the line really came from the sender.
  char body[MAX_LINE_LEN + 1];
  memcpy(body, line, body_len);
  body[body_len] = '\0';
  char* tok[6];
  int n = split(body, tok, 6);
  if (n < 2) return E_BAD_FORMAT;

  uint32_t seq;
  if (!parse_u32(tok[0], &seq) || seq < 1 || seq > 65535UL) return E_BAD_FORMAT;
  out->seq = (uint16_t)seq;

  const char* name = tok[1];
  int nargs = n - 2;
  char** arg = tok + 2;

  if (strcmp(name, "HB") == 0) {
    out->cmd = CMD_HB;
    return nargs == 0 ? E_OK : E_BAD_FORMAT;
  }
  if (strcmp(name, "STOP") == 0) {
    out->cmd = CMD_STOP;
    return nargs == 0 ? E_OK : E_BAD_FORMAT;
  }
  if (strcmp(name, "CFG") == 0) {
    out->cmd = CMD_CFG;
    return nargs == 0 ? E_OK : E_BAD_FORMAT;
  }
  if (strcmp(name, "CLEAR") == 0) {
    out->cmd = CMD_CLEAR;
    return nargs == 0 ? E_OK : E_BAD_FORMAT;
  }
  if (strcmp(name, "DOSE") == 0) {
    out->cmd = CMD_DOSE;
    if (nargs != 3) return E_BAD_FORMAT;
    if (arg[0][1] != '\0' || arg[0][0] < 'A' || arg[0][0] > 'C') return E_BAD_ARG;
    out->axis = arg[0][0];
    if (!parse_u32(arg[1], &out->steps) || !parse_u32(arg[2], &out->speed)) return E_BAD_ARG;
    if (out->steps == 0 || out->speed == 0) return E_BAD_ARG;
    if (out->steps > MAX_DOSE_STEPS || out->speed > MAX_DOSE_SPEED) return E_OVER_LIMIT;
    if (out->speed < MIN_DOSE_SPEED) return E_BAD_ARG;
    // steps <= MAX_DOSE_STEPS, so steps * 1000 fits in 32 bits (static_assert in limits.h).
    if (out->steps * 1000UL / out->speed > MAX_RUN_MS) return E_OVER_LIMIT;
    return E_OK;
  }
  if (strcmp(name, "SPIN") == 0) {
    out->cmd = CMD_SPIN;
    if (nargs != 2) return E_BAD_FORMAT;
    if (!parse_u32(arg[0], &out->speed) || !parse_u32(arg[1], &out->ms)) return E_BAD_ARG;
    if (out->speed == 0 || out->ms == 0) return E_BAD_ARG;
    if (out->speed > MAX_SPIN_SPEED || out->ms > MAX_SPIN_MS || out->ms > MAX_RUN_MS)
      return E_OVER_LIMIT;
    if (out->speed < MIN_SPIN_SPEED) return E_BAD_ARG;
    return E_OK;
  }
  return E_UNKNOWN_CMD;
}

const char* err_name(Err e) {
  switch (e) {
    case E_OK: return "ok";
    case E_BAD_CRC: return "bad_crc";
    case E_BAD_FORMAT: return "bad_format";
    case E_UNKNOWN_CMD: return "unknown_cmd";
    case E_BAD_ARG: return "bad_arg";
    case E_OVER_LIMIT: return "over_limit";
    case E_BUSY: return "busy";
    case E_FAULT: return "fault";
    case E_DUP: return "dup";
    case E_DRIVER: return "driver";
    case E_LID: return "lid_open";
    case E_NOT_IDLE: return "not_idle";
  }
  return "unknown";
}

size_t format_line(char* buf, size_t buf_len, const char* body) {
  size_t n = strlen(body);
  if (n + 5 > buf_len) return 0;  // '*', 2 hex digits, '\n', NUL
  memcpy(buf, body, n);
  snprintf(buf + n, buf_len - n, "*%02X\n", (unsigned)crc8(body, n));
  return n + 4;
}

}  // namespace proto
