#pragma once

#include <string>
#include <vector>

namespace gv::app {

struct GuiOptions {
   bool no_auto_login = false;
   bool debug = false;
   bool highlight_objects = false;
   bool highlight_game = false;
   bool detect_only = false;
   bool ocr_only = false;
   double fcr = 0.0;
};

void attach_console ();
void enable_dpi ();
int run_gui (int argc, char** argv, GuiOptions options);
int run_cli (int argc, char** argv, const std::vector<std::string>& args);

}
