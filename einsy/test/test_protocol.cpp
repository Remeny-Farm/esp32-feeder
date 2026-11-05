// SPDX-License-Identifier: MIT
// Host test for the Einsy line protocol parser. Run from einsy/test:
//   c++ -std=c++11 -Wall -Werror -I../einsy-feeder test_protocol.cpp \
//       ../einsy-feeder/protocol.cpp -o /tmp/test_protocol && /tmp/test_protocol
#include <stdio.h>
#include <string.h>

#include "feeder_limits.h"
#include "protocol.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    checks++;                                                          \
    if (!(cond)) {                                                     \
      failures++;                                                      \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
    }                                                                  \
  } while (0)

// Build "<body>*<HH>" with the correct checksum.
static void make_line(char* out, size_t n, const char* body) {
  snprintf(out, n, "%s*%02X", body, (unsigned)proto::crc8(body, strlen(body)));
}

static proto::Err parse_body(const char* body, proto::Command* c) {
  char line[128];
  make_line(line, sizeof(line), body);
  return proto::parse_line(line, c);
}

int main() {
  proto::Command c;
  char line[128];

  // CRC-8 (poly 0x07) check value from the CRC catalogue.
  CHECK(proto::crc8("123456789", 9) == 0xF4);

  // Fixed vectors from an independent CRC-8 implementation (Python).
  CHECK(proto::parse_line("12 DOSE A 800 400*48", &c) == proto::E_OK);
  CHECK(proto::parse_line("1 HB*C6", &c) == proto::E_OK);
  CHECK(proto::parse_line("1 HB*C7", &c) == proto::E_BAD_CRC);

  // Valid DOSE line.
  CHECK(parse_body("12 DOSE A 800 400", &c) == proto::E_OK);
  CHECK(c.cmd == proto::CMD_DOSE && c.seq == 12 && c.axis == 'A');
  CHECK(c.steps == 800 && c.speed == 400);

  // Valid SPIN, HB, STOP, CFG, CLEAR.
  CHECK(parse_body("3 SPIN 2000 1500", &c) == proto::E_OK);
  CHECK(c.cmd == proto::CMD_SPIN && c.speed == 2000 && c.ms == 1500);
  CHECK(parse_body("4 HB", &c) == proto::E_OK && c.cmd == proto::CMD_HB && c.seq == 4);
  CHECK(parse_body("5 STOP", &c) == proto::E_OK && c.cmd == proto::CMD_STOP);
  CHECK(parse_body("6 CFG", &c) == proto::E_OK && c.cmd == proto::CMD_CFG);
  CHECK(parse_body("7 CLEAR", &c) == proto::E_OK && c.cmd == proto::CMD_CLEAR);

  // A trailing CR is accepted, lower-case hex too.
  make_line(line, sizeof(line), "9 HB");
  strcat(line, "\r");
  CHECK(proto::parse_line(line, &c) == proto::E_OK);
  snprintf(line, sizeof(line), "9 HB*%02x", (unsigned)proto::crc8("9 HB", 4));
  CHECK(proto::parse_line(line, &c) == proto::E_OK);

  // Bad checksum: rejected, no command, seq not trusted.
  CHECK(proto::parse_line("12 DOSE A 800 400*00", &c) == proto::E_BAD_CRC);
  CHECK(c.cmd == proto::CMD_NONE && c.seq == 0);
  make_line(line, sizeof(line), "12 DOSE A 800 400");
  line[3] = 'X';  // flip one byte of the body, keep the old checksum
  CHECK(proto::parse_line(line, &c) == proto::E_BAD_CRC);
  CHECK(c.cmd == proto::CMD_NONE);

  // No checksum at all, garbage, empty line, one-byte line.
  CHECK(proto::parse_line("12 DOSE A 800 400", &c) == proto::E_BAD_CRC);
  CHECK(proto::parse_line("", &c) == proto::E_BAD_CRC);
  CHECK(proto::parse_line("*", &c) == proto::E_BAD_CRC);
  CHECK(proto::parse_line("\x01\x02\x03\x04\x05", &c) == proto::E_BAD_CRC);
  CHECK(proto::parse_line("12 DOSE A 800 400*ZZ", &c) == proto::E_BAD_CRC);

  // Over-limit steps, speed, run time: rejected, never clamped. The seq is kept.
  char body[64];
  snprintf(body, sizeof(body), "20 DOSE A %lu 400", MAX_DOSE_STEPS + 1);
  CHECK(parse_body(body, &c) == proto::E_OVER_LIMIT);
  CHECK(c.seq == 20 && c.cmd == proto::CMD_DOSE);
  snprintf(body, sizeof(body), "21 DOSE B 100 %lu", MAX_DOSE_SPEED + 1);
  CHECK(parse_body(body, &c) == proto::E_OVER_LIMIT);
  snprintf(body, sizeof(body), "22 DOSE C %lu %lu", MAX_DOSE_STEPS, MAX_DOSE_SPEED);
  CHECK(parse_body(body, &c) == proto::E_OK);  // exactly at the limits is allowed
  CHECK(parse_body("23 DOSE A 16000 100", &c) == proto::E_OVER_LIMIT);  // 160 s run time
  snprintf(body, sizeof(body), "24 SPIN 1000 %lu", MAX_SPIN_MS + 1);
  CHECK(parse_body(body, &c) == proto::E_OVER_LIMIT);
  snprintf(body, sizeof(body), "25 SPIN %lu 1000", MAX_SPIN_SPEED + 1);
  CHECK(parse_body(body, &c) == proto::E_OVER_LIMIT);
  CHECK(parse_body("26 DOSE A 999999999 400", &c) == proto::E_OVER_LIMIT);

  // Bad arguments.
  CHECK(parse_body("30 DOSE D 100 400", &c) == proto::E_BAD_ARG);   // unknown axis
  CHECK(parse_body("31 DOSE AB 100 400", &c) == proto::E_BAD_ARG);
  CHECK(parse_body("32 DOSE A 0 400", &c) == proto::E_BAD_ARG);     // zero steps
  CHECK(parse_body("33 DOSE A 100 0", &c) == proto::E_BAD_ARG);     // zero speed
  CHECK(parse_body("34 DOSE A -5 400", &c) == proto::E_BAD_ARG);
  CHECK(parse_body("35 DOSE A 1x0 400", &c) == proto::E_BAD_ARG);
  CHECK(parse_body("36 SPIN 0 100", &c) == proto::E_BAD_ARG);
  CHECK(parse_body("37 DOSE A 100 50", &c) == proto::E_BAD_ARG);    // below min speed

  // Bad format: wrong argument count, double space, no seq, seq 0, seq too big.
  CHECK(parse_body("40 DOSE A 100", &c) == proto::E_BAD_FORMAT);
  CHECK(parse_body("41 DOSE A 100 400 7", &c) == proto::E_BAD_FORMAT);
  CHECK(parse_body("42 DOSE  A 100 400", &c) == proto::E_BAD_FORMAT);
  CHECK(parse_body("43 HB extra", &c) == proto::E_BAD_FORMAT);
  CHECK(parse_body("HB", &c) == proto::E_BAD_FORMAT);
  CHECK(parse_body("0 HB", &c) == proto::E_BAD_FORMAT);
  CHECK(parse_body("65536 HB", &c) == proto::E_BAD_FORMAT);
  CHECK(parse_body("65535 HB", &c) == proto::E_OK && c.seq == 65535);

  // Unknown command: valid line, so the seq is known and cmd stays NONE.
  CHECK(parse_body("50 REBOOT", &c) == proto::E_UNKNOWN_CMD);
  CHECK(c.seq == 50 && c.cmd == proto::CMD_NONE);
  CHECK(parse_body("51 dose A 100 400", &c) == proto::E_UNKNOWN_CMD);  // case matters
  CHECK(parse_body("52 HEAT 200", &c) == proto::E_UNKNOWN_CMD);

  // Line too long.
  char longbody[200];
  memset(longbody, 'A', sizeof(longbody));
  longbody[150] = '\0';
  CHECK(parse_body(longbody, &c) == proto::E_BAD_CRC);

  // format_line round trip, and a buffer that is too small.
  char buf[64];
  size_t n = proto::format_line(buf, sizeof(buf), "OK 12");
  CHECK(n == strlen(buf) && n == 9);
  CHECK(strcmp(buf + 5, "") != 0);
  char expect[32];
  snprintf(expect, sizeof(expect), "OK 12*%02X\n", (unsigned)proto::crc8("OK 12", 5));
  CHECK(strcmp(buf, expect) == 0);
  CHECK(proto::format_line(buf, 8, "OK 12") == 0);

  // Error names are stable: the C3 parses them.
  CHECK(strcmp(proto::err_name(proto::E_OVER_LIMIT), "over_limit") == 0);
  CHECK(strcmp(proto::err_name(proto::E_UNKNOWN_CMD), "unknown_cmd") == 0);

  printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
