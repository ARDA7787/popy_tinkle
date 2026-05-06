// `popy` — quarantine CLI entrypoint.
//
// The dispatch lives in cli/commands.cpp; this is just the thin shell.

#include <curl/curl.h>

#include <iostream>

#include "cli/commands.h"

int main(int argc, char** argv) {
  std::cout
      << "popy startup: mode=direct_stream enforcement_mode=popy_only "
         "guarantee_level=guaranteed\n";
  curl_global_init(CURL_GLOBAL_DEFAULT);
  int rc = popy::cli::run(argc, argv);
  curl_global_cleanup();
  return rc;
}
