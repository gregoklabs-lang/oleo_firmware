#pragma once

#include <string>

#include <esp_err.h>

namespace Config
{
  void init();

  esp_err_t setString(const char *ns, const char *key, const std::string &value);
  esp_err_t setInt(const char *ns, const char *key, int32_t value);

  std::string getString(const char *ns, const char *key, const std::string &def = "");
  int32_t getInt(const char *ns, const char *key, int32_t def = 0);

  bool exists(const char *ns, const char *key);

  void dump();
} // namespace Config

void config_dump();
