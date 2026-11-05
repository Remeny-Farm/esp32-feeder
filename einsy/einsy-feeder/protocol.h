// SPDX-License-Identifier: MIT
// Line protocol parser for the Einsy feeder firmware. See einsy/PROTOCOL.md.
//
// Pure C++11, no Arduino dependency: the host test in einsy/test compiles it.
//
// Line format:   <seq> <COMMAND> [args...]*<HH>\n
//   <seq>  decimal 1..65535
//   <HH>   two hex digits: CRC-8 (poly 0x07, init 0x00) over the bytes before '*'
#ifndef EINSY_PROTOCOL_H
#define EINSY_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

namespace proto {

enum Cmd : uint8_t {
  CMD_NONE = 0,  // not recognised
  CMD_DOSE,
  CMD_SPIN,
  CMD_STOP,
  CMD_HB,
  CMD_CFG,
  CMD_CLEAR,
};

// Error codes. The parser returns the first six. The firmware adds the rest.
enum Err : uint8_t {
  E_OK = 0,
  E_BAD_CRC,      // checksum missing or wrong: line is ignored, seq unknown
  E_BAD_FORMAT,   // wrong shape: no seq, wrong argument count, stray spaces
  E_UNKNOWN_CMD,  // valid line, command name unknown
  E_BAD_ARG,      // argument not a number, axis unknown, zero, below minimum
  E_OVER_LIMIT,   // argument above a compiled-in hard limit (rejected, not clamped)
  E_BUSY,         // that motor already moves
  E_FAULT,        // a fault is latched: send CLEAR first
  E_DUP,          // same seq as the last DOSE or SPIN: not executed again
  E_DRIVER,       // TMC2130 does not answer over SPI, or reports a fault
  E_LID,          // lid switch open
  E_NOT_IDLE,     // CLEAR while a motor moves
};

struct Command {
  Cmd cmd;
  uint16_t seq;    // 0 when unknown
  char axis;       // 'A', 'B' or 'C' (DOSE only)
  uint32_t steps;  // DOSE: microsteps
  uint32_t speed;  // DOSE and SPIN: steps per second
  uint32_t ms;     // SPIN: duration
};

// CRC-8, polynomial 0x07, initial value 0x00, no reflection, no final XOR.
// crc8("123456789") == 0xF4.
uint8_t crc8(const char* data, size_t len);

// Parse one line, without the newline. A trailing '\r' is accepted.
// Fills *out. On E_BAD_CRC, out->cmd is CMD_NONE and out->seq is 0.
// On every other error, out->seq is set when it could be read, and out->cmd is
// set when the command name is known.
Err parse_line(const char* line, Command* out);

// Error name for the ERR line, for example "over_limit".
const char* err_name(Err e);

// Write "<body>*<HH>\n" into buf. Returns the length, or 0 if buf is too small.
size_t format_line(char* buf, size_t buf_len, const char* body);

}  // namespace proto

#endif  // EINSY_PROTOCOL_H
