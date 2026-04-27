// Argv parsing and subcommand dispatch.
//
// Single dispatch function used by main_popy.cpp. Returns the desired exit
// code; never calls exit().
#pragma once
#include <string>
#include <vector>

namespace popy::cli {

int run(int argc, char** argv);

}
