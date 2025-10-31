#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>
#include <SPIFFS.h>
#include <esp_heap_caps.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <ESPTelnet.h>
#include <vector>
#include <algorithm>
#include <map>
#include <string.h>

// Включаем BASIC (твои .h/.cpp — глобальные функции)
#include "BasicInterpreter.h"

// ASM: Z80 регистры (встроено)
struct Z80Regs {
  uint8_t A, B, C, D, E, H, L;
  uint16_t PC;
};

// Telnet
ESPTelnet telnet;

// BLE
BLEServer* pServer = nullptr;
BLECharacteristic* pTxCharacteristic;
bool deviceConnected = false;
bool first_connect = true;

// UI States
enum UIState { MENU, FILE_MANAGER, EDITOR, ENTER_FILENAME, SAVE_EXTENSION, RUN_BASIC, ASSEMBLER,PLAY_SOUP};
UIState ui_state = MENU;
UIState last_state = MENU;

// Меню
const char *main_menu_items[] = {
  "File Manager",
  "Edit File",
  "Run TenBasic",  // → File Manager, 'run <num>' для .bas
  "Assembler",
  "Send File via BLE",
  "System Info",
  "Play_Soup"
};
const int main_menu_count = sizeof(main_menu_items) / sizeof(main_menu_items[0]);
int menu_index = 0;

bool game_active = false;
int game_score = 0;


// Глобальные (ASM + редактор)
std::vector<String> file_list;
std::vector<String> editor_lines;
String current_file = "";
int editor_cursor_line = 0;
int editor_cursor_col = 0;
int editor_scroll_offset = 0;
Z80Regs regs = {0, 0, 0, 0, 0, 0, 0, 0};
std::vector<uint8_t> bytecode;

// Forward declarations
void handle_telnet_input(String input);
void print_assembler();
void enter_assembler();
void assemble_code();
void run_assembled_code();

// BLE callbacks (без изменений)
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) { 
    deviceConnected = true; 
    first_connect = true; 
  }
  void onDisconnect(BLEServer* pServer) { 
    deviceConnected = false; 
  }
};
class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    std::string rxValue = pCharacteristic->getValue();
    if (rxValue.length() > 0) {
      handle_telnet_input(String(rxValue.c_str()));
      Serial.println("BLE input: " + String(rxValue.c_str()));
    }
  }
};
// Telnet callbacks (без изменений)
void on_telnet_connect(String ip) {
  Serial.println("Telnet connected from " + ip);
  first_connect = true;
  telnet.print("Connected! Menu below:\n");
  print_main_menu_telnet();
}

void on_input_received(String input) {
  Serial.println("Received: '" + input + "'");
  handle_telnet_input(input);
}

// UI функции (без изменений, кроме print_file_manager_telnet — добавил "edit <num>")
void clear_screen_telnet() {
  telnet.print("\033[2J\033[H");
  telnet.flush();
  delay(50);
}

void print_header(const char* title) {
  telnet.printf("\n=== %s ===\n", title);
}

void draw_system_info_telnet() {
  size_t ram_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  telnet.printf("RAM: %uK free | BLE: %s | State: %d\n", (unsigned)(ram_free / 1024), 
                deviceConnected ? "Connected" : "Waiting", (int)ui_state);
}

void print_main_menu_telnet() {
  clear_screen_telnet();
  print_header("ZX SPECTRUM ESP32 - MAIN MENU");
  for (int i = 0; i < main_menu_count; i++) {
    if (i == menu_index) {
      telnet.printf("> %s\n", main_menu_items[i]);
    } else {
      telnet.printf("  %s\n", main_menu_items[i]);
    }
  }
  telnet.println("Commands: up/down/enter, '1' File Mgr, '2' Edit, 'menu' refresh, 'back' anywhere");
  draw_system_info_telnet();
  telnet.flush();
  last_state = MENU;
  Serial.println("Printed main menu, index: " + String(menu_index));
}

void scan_files() {
  file_list.clear();
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      file_list.push_back(String(file.name()));
    }
    file = root.openNextFile();
  }
  std::sort(file_list.begin(), file_list.end());
  Serial.println("Scanned files: " + String(file_list.size()));
}
void print_file_manager_telnet() {
  clear_screen_telnet();
  print_header("FILE MANAGER");
  scan_files();
  if (file_list.empty()) {
    telnet.println("No files. Try 'new test.bas'");
  } else {
    telnet.println("Files:");
    for (size_t i = 0; i < file_list.size(); i++) {
      telnet.printf("%d: %s\n", (int)i + 1, file_list[i].c_str());
    }
  }
  telnet.println("Commands: <num> edit, 'edit <num>', 'new <name>', 'run <num>' for BASIC (.bas only), 'back'");
  draw_system_info_telnet();
  telnet.flush();
  last_state = FILE_MANAGER;
  Serial.println("Printed File Manager, files: " + String(file_list.size()));
}

void load_file_to_editor(const String& filename) {
  editor_lines.clear();
  editor_cursor_line = 0;
  editor_cursor_col = 0;
  editor_scroll_offset = 0;
  current_file = filename;
  if (current_file.indexOf('.') == -1) current_file += ".bas";
  File file = SPIFFS.open("/" + current_file, "r");
  if (file) {
    while (file.available()) {
      String line_raw = file.readStringUntil('\n');
      String line = line_raw;
      line.trim();
      if (line.length() > 0) editor_lines.push_back(line);
    }
    file.close();
  }
  if (editor_lines.empty()) editor_lines.push_back("");
  Serial.println("Loaded: " + current_file + ", lines: " + String(editor_lines.size()));
}

void print_editor() {
  clear_screen_telnet();
  print_header(("EDITOR - " + current_file).c_str());
  int visible_lines = 20;
  int start_line = editor_scroll_offset;
  int end_line = std::min((int)editor_lines.size(), start_line + visible_lines);
  for (int i = start_line; i < end_line; i++) {
    if (i == editor_cursor_line) {
      telnet.printf("%3d > %s\n", i + 1, editor_lines[i].c_str());
    } else {
      telnet.printf("%3d   %s\n", i + 1, editor_lines[i].c_str());
    }
  }
  telnet.println("Commands: up/down cursor, 'add' new line, text=edit line, 'save', 'back'");
  draw_system_info_telnet();
  telnet.flush();
  last_state = EDITOR;
  Serial.println("Printed editor, cursor: " + String(editor_cursor_line));
}
void enter_editor(const String& filename) {
  ui_state = EDITOR;
  load_file_to_editor(filename);
  print_editor();
}

void save_editor_to_file() {
  ui_state = SAVE_EXTENSION;
  clear_screen_telnet();
  print_header("SAVE FILE");
  telnet.printf("Current: %s\n", current_file.c_str());
  telnet.println("Extension: 'bas', 'asm', 'c', 'py', 'skip' (no change), 'back'");
  telnet.flush();
  Serial.println("Save dialog opened for " + current_file);
}

void send_file_via_ble(const String& filename) {
  if (!deviceConnected) {
    telnet.println("BLE not connected!");
    return;
  }
  File file = SPIFFS.open("/" + filename, "r");
  if (!file) {
    telnet.println("File not found!");
    return;
  }
  String data = "";
  while (file.available()) data += (char)file.read();
  file.close();
  pTxCharacteristic->setValue(data.c_str());
  pTxCharacteristic->notify();
  telnet.println("File sent via BLE!");
  Serial.println("Sent: " + filename);
}

// ASM функции (фикс: auto-assemble в run)
void print_assembler() {
  clear_screen_telnet();
  print_header("ASSEMBLER - Z80 SIMULATOR");
  telnet.println("Current code:");
  int visible_lines = 20;
  int start_line = editor_scroll_offset;
  int end_line = std::min((int)editor_lines.size(), start_line + visible_lines);
  for (int i = start_line; i < end_line; i++) {
    if (i == editor_cursor_line) {
      telnet.printf("%3d > %s\n", i + 1, editor_lines[i].c_str());
    } else {
      telnet.printf("%3d   %s\n", i + 1, editor_lines[i].c_str());
    }
  }
  telnet.printf("Registers: A=%d B=%d C=%d D=%d E=%d H=%d L=%d PC=%d\n", 
                regs.A, regs.B, regs.C, regs.D, regs.E, regs.H, regs.L, regs.PC);
  telnet.printf("Bytecode size: %d bytes\n", (int)bytecode.size());
  telnet.println("Commands: up/down cursor, text=edit line, 'add' new line, 'assemble', 'run' (auto-assemble+exec), 'clear', 'save' (.asm), 'back'");
  draw_system_info_telnet();
  telnet.flush();
  last_state = ASSEMBLER;
  Serial.println("Printed assembler, cursor: " + String(editor_cursor_line));
}
void enter_assembler() {
  ui_state = ASSEMBLER;
  editor_lines.clear();
  editor_lines.push_back("LD A,10");
  editor_lines.push_back("ADD A,B");
  editor_lines.push_back("RET");
  editor_cursor_line = 0;
  editor_scroll_offset = 0;
  regs = {0, 0, 0, 0, 0, 0, 0, 0};
  bytecode.clear();
  current_file = "untitled.asm";
  print_assembler();
}

void assemble_code() {
  bytecode.clear();
  bool error = false;
  for (const String& line : editor_lines) {
    String cmd = line;
    cmd.trim();
    cmd.toUpperCase();
    if (cmd.startsWith("LD A,")) {
      String val_str = cmd.substring(5);
      val_str.trim();
      uint8_t val = val_str.toInt();
      bytecode.push_back(0x3E);
      bytecode.push_back(val);
    } else if (cmd.startsWith("ADD A,")) {
      String reg = cmd.substring(6);
      reg.trim();
      if (reg == "B") bytecode.push_back(0x80);
      else if (reg == "C") bytecode.push_back(0x81);
      else { error = true; break; }
    } else if (cmd == "RET") {
      bytecode.push_back(0xC9);
    } else if (cmd.startsWith("CALL ")) {
      bytecode.push_back(0xCD);
      bytecode.push_back(0x00);
      bytecode.push_back(0x00);
    } else if (cmd.length() > 0) {
      error = true;
      break;
    }
  }
  if (error) {
    telnet.println("Assembly error: unsupported (LD A,n; ADD A,B/C; RET; CALL)");
    Serial.println("ASM: Assembly error");
  } else {
    telnet.printf("Assembled %d bytes OK!\n", (int)bytecode.size());
    Serial.println("ASM: Assembled " + String(bytecode.size()) + " bytes");
  }
  delay(1000);
  print_assembler();
}

void run_assembled_code() {
  if (bytecode.empty()) {
    telnet.println("Assemble first!");
    Serial.println("ASM: No bytecode, assemble first");
    delay(1000);
    print_assembler();
    return;
  }
  regs = {0, 0, 0, 0, 0, 0, 0, 0};
  regs.PC = 0;
  Serial.println("ASM: Starting execution, PC=0");
  while (regs.PC < bytecode.size()) {
    uint8_t op = bytecode[regs.PC++];
    switch (op) {
      case 0x3E: if (regs.PC < bytecode.size()) regs.A = bytecode[regs.PC++]; Serial.println("ASM: LD A," + String(regs.A)); break;
      case 0x80: regs.A += regs.B; Serial.println("ASM: ADD A,B → A=" + String(regs.A)); break;
      case 0x81: regs.A += regs.C; Serial.println("ASM: ADD A,C → A=" + String(regs.A)); break;
      case 0xC9: Serial.println("ASM: RET"); goto end_exec;
      case 0xCD: if (regs.PC + 1 < bytecode.size()) regs.PC += 2; Serial.println("ASM: CALL (skipped)"); break;
      default: telnet.printf("Unknown opcode 0x%02X\n", op); Serial.println("ASM: Unknown opcode 0x" + String(op, HEX)); goto end_exec;
    }
  }
end_exec:
  telnet.printf("Execution done. A=%d B=%d C=%d PC=%d\n", regs.A, regs.B, regs.C, regs.PC);
  Serial.println("ASM: Done. A=" + String(regs.A) + " B=" + String(regs.B) + " C=" + String(regs.C) + " PC=" + String(regs.PC));
  delay(2000);  // Больше времени на просмотр
  print_assembler();
}
// Обработка ввода (фиксы: RUN_BASIC пауза, ASM auto-run, edit <num>)
void handle_telnet_input(String input) {
  String temp_input = input;
  temp_input.trim();
  input = temp_input;

  if (input.length() == 0) return;

  String state_name = "UNKNOWN";
  switch (ui_state) {
    case PLAY_SOUP: state_name = "PLAY_SOUP"; break;
    case MENU: state_name = "MENU"; break;
    case FILE_MANAGER: state_name = "FILE_MANAGER"; break;
    case EDITOR: state_name = "EDITOR"; break;
    case ENTER_FILENAME: state_name = "ENTER_FILENAME"; break;
    case SAVE_EXTENSION: state_name = "SAVE_EXTENSION"; break;
    case RUN_BASIC: state_name = "RUN_BASIC"; break;
    case ASSEMBLER: state_name = "ASSEMBLER"; break;

  }
  Serial.println("Processing: '" + input + "' in " + state_name + ", menu_index: " + String(menu_index));

  if (input == "menu" || input == "redraw") {
    ui_state = MENU;
    menu_index = 0;
    print_main_menu_telnet();
    return;
  } else if (input == "back") {
    switch (ui_state) {
      case ENTER_FILENAME:
      case SAVE_EXTENSION:
      case FILE_MANAGER:
      case EDITOR:
      case ASSEMBLER:
      case RUN_BASIC:  // Фикс: back из RUN_BASIC
        ui_state = MENU; 
        menu_index = 0; 
        print_main_menu_telnet(); 
        return;
      default: return;
    }
  }else if (ui_state == PLAY_SOUP) {
    handleGameInput(input);
}

  // RUN_BASIC: Только back/menu, игнор остального (пауза после BASIC)
  if (ui_state == RUN_BASIC) {
    if (input == "back" || input == "menu") {
      ui_state = FILE_MANAGER;
      print_file_manager_telnet();
    } else {
      telnet.println("BASIC done. Type 'back' or 'menu'.");
    }
    return;
  }

  if (ui_state == MENU) {
    if (input == "up") {
      menu_index = (menu_index - 1 + main_menu_count) % main_menu_count;
      print_main_menu_telnet();
    } else if (input == "down") {
      menu_index = (menu_index + 1) % main_menu_count;
      print_main_menu_telnet();
    } else if (input == "enter") {
      switch (menu_index) {
        case 0: ui_state = FILE_MANAGER; print_file_manager_telnet(); break;
        case 1: ui_state = ENTER_FILENAME; clear_screen_telnet(); print_header("ENTER FILENAME"); telnet.println("Type filename (e.g., 'test.bas') or 'back':"); draw_system_info_telnet(); telnet.flush(); break;
        case 2: ui_state = FILE_MANAGER; print_file_manager_telnet(); break;  // BASIC через File Manager
        case 3: enter_assembler(); break;  // ASM
        case 4: if (!file_list.empty()) send_file_via_ble(file_list[0]); else telnet.println("No files!"); print_main_menu_telnet(); break;
        case 5: clear_screen_telnet(); print_header("SYSTEM INFO"); draw_system_info_telnet(); telnet.println("Press 'menu' to return"); telnet.flush(); break;
        case 6: ui_state = PLAY_SOUP; playSoupGame(); break;  // Запуск игры
        default: telnet.println("Not implemented"); print_main_menu_telnet(); break;
      }
    } else if (input == "1") {
      ui_state = FILE_MANAGER; print_file_manager_telnet();
    } else if (input == "2") {
      ui_state = ENTER_FILENAME; clear_screen_telnet(); print_header("ENTER FILENAME"); telnet.println("Type filename (e.g., 'test.bas') or 'back':"); draw_system_info_telnet(); telnet.flush();
    }
  } else if (ui_state == ENTER_FILENAME) {
    if (input.length() > 0 && input != "back") {
      current_file = input;
      enter_editor(current_file);
    }
  } else if (ui_state == SAVE_EXTENSION) {
    bool changed = false;
    if (input == "bas" && !current_file.endsWith(".bas")) { current_file += ".bas"; changed = true; }
    else if (input == "asm" && !current_file.endsWith(".asm")) { current_file += ".asm"; changed = true; }
    else if (input == "c" && !current_file.endsWith(".c")) { current_file += ".c"; changed = true; }
    else if (input == "py" && !current_file.endsWith(".py")) { current_file += ".py"; changed = true; }
    else if (input == "skip") { changed = true; }
    else { telnet.println("Invalid. Try 'bas', 'skip'"); return; }

    if (changed || input == "skip") {
      File file = SPIFFS.open("/" + current_file, "w");
      if (file) {
        for (const String& line : editor_lines) file.println(line);
        file.close();
        telnet.printf("Saved '%s' with %d lines!\n", current_file.c_str(), (int)editor_lines.size());
        scan_files();
        delay(1500);
        ui_state = EDITOR;
        print_editor();
        Serial.println("Saved: " + current_file);
      } else {
        telnet.println("Save failed!"); 
        ui_state = EDITOR; 
        print_editor();
      }
    }
  } else if (ui_state == FILE_MANAGER) {
    if (input.startsWith("new ")) {
      String new_name = input.substring(4);
      new_name.trim();
      current_file = new_name;
      if (current_file.indexOf('.') == -1) current_file += ".bas";
      enter_editor(current_file);
    } else if (input.startsWith("run ")) {
      String num_str = input.substring(4);
      num_str.trim();
      int file_num = num_str.toInt();
      if (file_num > 0 && file_num <= (int)file_list.size() && file_list[file_num - 1].endsWith(".bas")) {
        Serial.println("BASIC: Launching " + file_list[file_num - 1]);
        run_basic_program(file_list[file_num - 1]);  // Прямой вызов из .cpp
        ui_state = RUN_BASIC;  // Пауза: ждём back
      } else {
        telnet.println("Invalid file or not .bas!");
      }
    } else if (input.startsWith("edit ")) {  // Фикс: edit <num>
      String num_str = input.substring(5);
      num_str.trim();
      int file_num = num_str.toInt();
      if (file_num > 0 && file_num <= (int)file_list.size()) {
        enter_editor(file_list[file_num - 1]);
      } else {
        telnet.println("Invalid. Use 'edit <num>'");
      }
    } else {
      String num_str = input;
      num_str.trim();
      int file_num = num_str.toInt();
      if (file_num > 0 && file_num <= (int)file_list.size()) {
        enter_editor(file_list[file_num - 1]);  // <num> = edit
      } else {
        telnet.println("Invalid. Use number, 'edit <num>', 'new <name>' or 'run <num>'");
      }
    }
  } else if (ui_state == EDITOR) {
    if (input == "save") {
      save_editor_to_file();
    } else if (input == "up") {
      editor_cursor_line = std::max(0, editor_cursor_line - 1);
      if (editor_cursor_line < editor_scroll_offset) editor_scroll_offset = editor_cursor_line;
      print_editor();
    } else if (input == "down") {
      editor_cursor_line = std::min((int)editor_lines.size() - 1, editor_cursor_line + 1);
      if (editor_cursor_line >= editor_scroll_offset + 20) editor_scroll_offset = editor_cursor_line - 19;
      print_editor();
    } else if (input == "add") {
      editor_lines.push_back("");
      editor_cursor_line = editor_lines.size() - 1;
      print_editor();
    } else if (input.startsWith("add ")) {
      String pos_str = input.substring(4);
      pos_str.trim();
      int pos = pos_str.toInt();
      if (pos > 0 && pos <= (int)editor_lines.size() + 1) {
        editor_lines.insert(editor_lines.begin() + pos - 1, "");
        editor_cursor_line = pos - 1;
        print_editor();
      }
    } else if (editor_cursor_line < (int)
editor_lines.size() && input.length() > 0) {  // Edit line
      editor_lines[editor_cursor_line] = input;
      print_editor();
    }
  } else if (ui_state == ASSEMBLER) {
    if (input == "assemble") {
      assemble_code();
    } else if (input == "run") {
      assemble_code();  // Auto-assemble
      run_assembled_code();
    } else if (input == "clear") {
      bytecode.clear();
      regs = {0, 0, 0, 0, 0, 0, 0, 0};
      telnet.println("Cleared registers and bytecode.");
      delay(1000);
      print_assembler();
    } else if (input == "save") {
      current_file = "test.asm";
      File file = SPIFFS.open("/" + current_file, "w");
      if (file) {
        for (const String& line : editor_lines) file.println(line);
        file.close();
        telnet.printf("Saved '%s'!\n", current_file.c_str());
        scan_files();
        delay(1000);
      } else {
        telnet.println("Save failed!");
      }
      print_assembler();
    } else if (input == "up") {
      editor_cursor_line = std::max(0, editor_cursor_line - 1);
      if (editor_cursor_line < editor_scroll_offset) editor_scroll_offset = editor_cursor_line;
      print_assembler();
    } else if (input == "down") {
      editor_cursor_line = std::min((int)editor_lines.size() - 1, editor_cursor_line + 1);
      if (editor_cursor_line >= editor_scroll_offset + 20) editor_scroll_offset = editor_cursor_line - 19;
      print_assembler();
    } else if (input == "add") {
      editor_lines.push_back("");
      editor_cursor_line = editor_lines.size() - 1;
      print_assembler();
    } else if (editor_cursor_line < (int)editor_lines.size() && input.length() > 0 && 
               input != "assemble" && input != "run" && input != "save" && input != "clear" && input != "back") {  // Edit line (игнор команд)
      editor_lines[editor_cursor_line] = input;
      print_assembler();
    }
  }
}



// Новая функция для игры (добавь в конец .ino файла)
void playSoupGame() {
    clear_screen_telnet();
    print_header("Игра 'Свари суп'");
    telnet.println("Выбери ингредиенты для ретро-компьютера:");
    telnet.println("1=BASIC, 2=ASM, 3=WiFi");
    telnet.println("Твой выбор (или 'back' для выхода):");
    draw_system_info_telnet();
    telnet.flush();
    game_active = true;
    game_score = 0;
}

void handleGameInput(String input) {
    Serial.println("Game input: " + input);  // Debug в Serial
    if (!game_active) {
        telnet.println("Игра не активна!");
        return;
    }
    int choice = input.toInt();
    telnet.println("Обработан выбор: " + String(choice));  // Debug
    if (choice == 1) {
        telnet.println("Добавляем BASIC! +1 очко");
        game_score++;
    } else if (choice == 2) {
        telnet.println("Добавляем ASM! +1 очко");
        game_score++;
    } else if (choice == 3) {
        telnet.println("Добавляем WiFi! +1 очко");
        game_score++;
    } else if (input == "back") {
        game_active = false;
        ui_state = MENU;
        print_main_menu_telnet();
        return;
    } else {
        telnet.println("Неверный выбор! Попробуй 1, 2 или 3.");
        return;
    }
    telnet.println("Очки: " + String(game_score));  // Показать прогресс
    if (game_score >= 1) {
        telnet.println("Суп готов! Соавторы: Ты + Claude от Anthropic 🧑‍💻🤖 при участии Cursor");
        telnet.println("Ещё раз? (y/n или 'back')");
    } else {
        telnet.println("Суп недоварен... Попробуй снова!");
        playSoupGame();
    }
}


// Setup/Loop (без изменений)
void setup() {
  Serial.begin(115200);
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed");
    return;
  }

  // WiFi
  WiFi.begin("itel A16 Plus", "");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(1000); 
    Serial.print("."); 
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi failed! Starting AP...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ZX_Spectrum_ESP32", "12345678");
    Serial.println("AP IP: 192.168.4.1. Telnet: 192.168.4.1:23");
  }

  // Telnet
  telnet.begin(23);
  telnet.onConnect(on_telnet_connect);
  telnet.onInputReceived(on_input_received);
  telnet.setLineMode(true);
  telnet.println("Welcome to ZX Spectrum ESP32! Type 'menu' if not visible.\n");
  Serial.println("Telnet ready.");

  // BLE
  BLEDevice::init("ZX Spectrum ESP32 BLE");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  BLEService* pService = pServer->createService(BLEUUID((uint16_t)0xFFE0));
  pTxCharacteristic = pService->createCharacteristic(BLEUUID((uint16_t)0xFFE1), BLECharacteristic::PROPERTY_NOTIFY);
  pTxCharacteristic->addDescriptor(new BLE2902());
  pTxCharacteristic->setCallbacks(new MyCallbacks());
  pService->start();
  pServer->getAdvertising()->start();
  Serial.println("BLE ready.");

  print_main_menu_telnet();
}

void loop() {
  telnet.loop();

  if (first_connect && telnet.isConnected()) {
    static unsigned long last_check = 0;
    if (millis() - last_check > 500) {
      Serial.println("Loop: Connected, printing menu...");
      print_main_menu_telnet();
     
      first_connect = false;
      last_check = millis();
      
    }
     
  }

  delay(10);
 
}
