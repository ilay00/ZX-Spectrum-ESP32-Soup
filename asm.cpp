#include "asm.h"

// Регистры
uint8_t Z80Regs::A = 0;
uint8_t Z80Regs::B = 0;
uint8_t Z80Regs::C = 0;
uint8_t Z80Regs::D = 0;
uint8_t Z80Regs::E = 0;
uint8_t Z80Regs::H = 0;
uint8_t Z80Regs::L = 0;
uint16_t Z80Regs::PC = 0;

// Reset регистров
void Z80Regs::reset() {
  A = B = C = D = E = H = L = 0;
  PC = 0;
}

// Assembler
std::vector<uint8_t> Assembler::bytecode;

void Assembler::clear() {
  bytecode.clear();
}

bool Assembler::assemble_line(String line) {
  if (line.startsWith("LD A,")) {
    String val_str = line.substring(5);
    uint8_t val = val_str.toInt();
    bytecode.push_back(0x3E); // LD A,n
    bytecode.push_back(val);
  } else if (line.startsWith("ADD A,")) {
    String reg = line.substring(6);
    if (reg == "B") bytecode.push_back(0x80); // ADD A,B
    else if (reg == "C") bytecode.push_back(0x81); // ADD A,C
    else return false;
  } else if (line == "RET") {
    bytecode.push_back(0xC9); // RET
  } else if (line.startsWith("CALL ")) {
    bytecode.push_back(0xCD); // CALL nn (placeholder)
    bytecode.push_back(0x00);
    bytecode.push_back(0x00);
  } else {
    return false;
  }
  return true;
}

void Assembler::execute() {
  Z80Regs::reset();
  while (Z80Regs::PC < bytecode.size()) {
    uint8_t op = bytecode[Z80Regs::PC++];
    switch (op) {
      case 0x3E: // LD A,n
        Z80Regs::A = bytecode[Z80Regs::PC++];
        break;
      case 0x80: // ADD A,B
        Z80Regs::A += Z80Regs::B;
        break;
      case 0x81: // ADD A,C
        Z80Regs::A += Z80Regs::C;
        break;
      case 0xC9: // RET
        return;
      case 0xCD: // CALL (skip)
        Z80Regs::PC += 2;
        break;
      default:
        Serial.println("Unknown opcode");
        return;
    }
  }
}

size_t Assembler::get_bytecode_size() {
  return bytecode.size();
}
