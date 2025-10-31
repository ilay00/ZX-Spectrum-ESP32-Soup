#ifndef ASM_H
#define ASM_H

#include <vector>
#include <string>
#include <Arduino.h>

// Регистры Z80 (статические)
class Z80Regs {
public:
  static uint8_t A, B, C, D, E, H, L;
  static uint16_t PC;
  static void reset();
};

// Assembler класс
class Assembler {
private:
  static std::vector<uint8_t> bytecode;
public:
  static void clear();
  static bool assemble_line(String line);
  static void execute();
  static size_t get_bytecode_size();
};

#endif

