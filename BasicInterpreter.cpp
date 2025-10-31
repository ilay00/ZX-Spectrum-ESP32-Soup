#include "BasicInterpreter.h"
#include <SPIFFS.h>  // Для файлов

// Определения глобалов
std::vector<String> basic_lines;
std::map<String, float> basic_vars_num;
std::map<String, String> basic_vars_str;
int basic_pc = 0;
int basic_stack[10];
int basic_stack_ptr = 0;
bool basic_running = false;

void basic_run() {
  basic_running = true;
  basic_pc = 0;
  basic_stack_ptr = 0;
  basic_vars_num.clear();
  basic_vars_str.clear();
  Serial.println("BASIC: Starting execution, lines: " + String(basic_lines.size()));
  while (basic_running && basic_pc < (int)basic_lines.size()) {
    String line = basic_lines[basic_pc];
    Serial.println("BASIC: Exec line " + String(basic_pc + 1) + ": " + line);
    int space_idx = line.indexOf(' ');
    if (space_idx == -1) {
      basic_pc++;
      continue;
    }
    String temp_cmd = line.substring(space_idx + 1);
    temp_cmd.trim();
    String cmd = temp_cmd;
    if (cmd.startsWith("PRINT ")) {
      String temp_arg = cmd.substring(6);
      temp_arg.trim();
      String arg = temp_arg;
      String output;
      if (arg.startsWith("\"") && arg.endsWith("\"")) {
        output = arg.substring(1, arg.length() - 1);
      } else if (arg.endsWith("$")) {
        output = basic_eval_expr_str(arg);
      } else {
        output = String(basic_eval_expr(arg));
      }
      telnet.println(output);
      Serial.println("BASIC: PRINT '" + output + "'");
    } else if (cmd.startsWith("LET ")) {
      int eq_idx = cmd.indexOf('=');
      if (eq_idx != -1) {
        String temp_var = cmd.substring(4, eq_idx);
        temp_var.trim();
        String var = temp_var;
        String temp_val = cmd.substring(eq_idx + 1);
        temp_val.trim();
        String val = temp_val;
        if (var.endsWith("$")) {
          if (val.startsWith("\"")) {
            basic_vars_str[var] = val.substring(1, val.length() - 1);
          } else{
            basic_vars_str[var] = basic_eval_expr_str(val);
          }
          Serial.println("BASIC: LET str " + var + " = '" + basic_vars_str[var] + "'");
        } else {
          basic_vars_num[var] = basic_eval_expr(val);
          Serial.println("BASIC: LET num " + var + " = " + String(basic_vars_num[var]));
        }
      }
    } else if (cmd.startsWith("IF ")) {
      int then_idx = cmd.indexOf(" THEN ");
      if (then_idx != -1) {
        String temp_cond = cmd.substring(3, then_idx);
        temp_cond.trim();
        String cond = temp_cond;
        String temp_then = cmd.substring(then_idx + 6);
        temp_then.trim();
        String then_part = temp_then;
        float cond_val = basic_eval_condition(cond);
        Serial.println("BASIC: IF '" + cond + "' = " + String(cond_val ? "true" : "false"));
        if (cond_val != 0) {
          if (then_part.startsWith("PRINT ")) {
            String temp_print_arg = then_part.substring(6);
            temp_print_arg.trim();
            String print_arg = temp_print_arg;
            String output;
            if (print_arg.startsWith("\"") && print_arg.endsWith("\"")) {
              output = print_arg.substring(1, print_arg.length() - 1);
            } else {
              output = String(basic_eval_expr(print_arg));
            }
            telnet.println(output);
            Serial.println("BASIC: IF PRINT '" + output + "'");
          } else {
            basic_lines.insert(basic_lines.begin() + basic_pc + 1, then_part);
            Serial.println("BASIC: IF inserted: " + then_part);
          }
        }
        basic_pc++;
      }
    } else if (cmd.startsWith("FOR ")) {
      int to_idx = cmd.indexOf(" TO ");
      if (to_idx != -1) {
        String temp_var_str = cmd.substring(4, to_idx);
        temp_var_str.trim();
        String var_str = temp_var_str;
        String temp_to_val = cmd.substring(to_idx + 4);
        temp_to_val.trim();
        String to_val = temp_to_val;
        String from_val = "1";  // Упрощённо FOR I=1 TO N
        basic_vars_num[var_str] = basic_eval_expr(from_val);
        basic_vars_num[var_str + "_TO"] = basic_eval_expr(to_val);
        Serial.println("BASIC: FOR " + var_str + " = " + String(basic_vars_num[var_str]) + " TO " + String(basic_vars_num[var_str + "_TO"]));
      }
    } else if (cmd.startsWith("NEXT ")) {
      String temp_var_str = cmd.substring(5);
      temp_var_str.trim();
      String var_str = temp_var_str;
      if (basic_vars_num.count(var_str) && basic_vars_num.count(var_str + "_TO")) {
        if (basic_vars_num[var_str] < basic_vars_num[var_str + "_TO"]) {
          basic_vars_num[var_str]++;
          basic_pc = basic_pc - 2;  // Вернись к FOR
          Serial.println("BASIC: NEXT " + var_str + "++ to " + String(basic_vars_num[var_str]));
        } else {
          basic_vars_num.erase(var_str);
          basic_vars_num.erase(var_str + "_TO");
          Serial.println("BASIC: NEXT end loop " + var_str);
        }
      }
    } else if (cmd.startsWith("GOSUB ")) {
      String temp_line_num = cmd.substring(6);
      temp_line_num.trim();
      String line_num_str = temp_line_num;
      int line_num = line_num_str.toInt();
      basic_stack[basic_stack_ptr++] = basic_pc;
      for (int i = 0; i < (int)basic_lines.size(); i++) {
        if (basic_lines[i].startsWith(String(line_num) + " ")) {
          basic_pc = i;
          Serial.println("BASIC: GOSUB to line " + String(line_num) + " (PC=" + String(basic_pc) + ")");
          break;
        }
      }
    } else if (cmd == "RETURN") {
      if (basic_stack_ptr > 0) {
        basic_pc = basic_stack[--basic_stack_ptr];
        Serial.println("BASIC: RETURN to PC=" + String(basic_pc));
      }
    } else if (cmd == "END") {
      basic_running = false;
      Serial.println("BASIC: END reached");
    } else if (cmd.startsWith("INPUT ")) {
      telnet.println("INPUT: Enter value (TODO: full impl)");
      Serial.println("BASIC: INPUT (stub)");
    } else {
      Serial.println("BASIC: Unknown cmd '" + cmd + "'");
    }
    basic_pc++;
    delay(500);  // Для Telnet/Serial вывода
  }
  basic_running = false;
  telnet.println("BASIC program ended. Type 'back' to menu.");
  Serial.println("BASIC: Execution finished");
}

float basic_eval_expr(String expr) {
  String temp_expr = expr;
  temp_expr.trim();
  expr = temp_expr;
  if (basic_vars_num.count(expr)) {
    return basic_vars_num[expr];
  }
  return expr.toFloat();
}

String basic_eval_expr_str(String expr) {
  String temp_expr = expr;
  temp_expr.trim();
  expr = temp_expr;
  if (basic_vars_str.count(expr)) {
    return basic_vars_str[expr];
  }
  return expr;
}

float basic_eval_condition(String cond) {
  String temp_cond = cond;
  temp_cond.trim();
  cond = temp_cond;
  int op_idx = cond.indexOf(' ');
  if (op_idx == -1) {
    return basic_eval_expr(cond);
  }
  String temp_left = cond.substring(0, op_idx);
  temp_left.trim();
  String left = temp_left;
  String temp_op_right = cond.substring(op_idx + 1);
  temp_op_right.trim();
  String op_right = temp_op_right;
  int op2 = op_right.indexOf(' ');
  if (op2 == -1) {
    return 0.0;
  }
  String temp_op = op_right.substring(0, op2);
  temp_op.trim();
  String op = temp_op;
  String temp_right = op_right.substring(op2 + 1);
  temp_right.trim();
  String right = temp_right;
  float lval = basic_eval_expr(left);
  float rval = basic_eval_expr(right);
  if (op == ">") return lval > rval ? 1.0f : 0.0f;
  if (op == "<") return lval < rval ? 1.0f : 0.0f;
  if (op == "=" || op == "==") return lval == rval ? 1.0f : 0.0f;
  return 0.0f;
}

void run_basic_program(const String& filename) {
  basic_lines.clear();
  File file = SPIFFS.open("/" + filename, "r");
  if (file) {
    while (file.available()) {
      String line_raw = file.readStringUntil('\n');
      String temp_line = line_raw;
      temp_line.trim();
      String line = temp_line;
      if (line.length() > 0) {
        basic_lines.push_back(line);
      }
    }
    file.close();
    Serial.println("Loaded BASIC: " + filename + ", lines: " + String(basic_lines.size()));
  } else {
    telnet.println("File not found: " + filename);
    Serial.println("BASIC: File not found " + filename);
    return;
  }
  if (basic_lines.empty()) {
    telnet.println("No BASIC code!");
    Serial.println("BASIC: Empty file");
    return;
  }
  basic_running = true;
  telnet.println("Running BASIC program...");
  Serial.println("BASIC: Starting run_basic_program");
  basic_run();
  // НЕ возвращаемся в File Manager сразу — ждём 'back'
  // ui_state = RUN_BASIC;  // Установи в .ino вызове
  telnet.println("BASIC done. Type 'back' or 'menu'.");
}
