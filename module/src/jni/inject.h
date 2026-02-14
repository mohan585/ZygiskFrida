#ifndef ZYGISKFRIDA_INJECT_H
#define ZYGISKFRIDA_INJECT_H

#include "config.h"

void inject_lib(std::string const& lib_path, std::string const& logContext);
void start_injection(target_config const& cfg);
bool check_and_inject(std::string const& app_name);

#endif  // ZYGISKFRIDA_INJECT_H
