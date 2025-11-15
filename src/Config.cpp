#include "Config.hpp"

#include <Arduino.h>
#include <algorithm>
#include <cstring>
#include <nvs.h>
#include <nvs_flash.h>
#include <string>
#include <vector>

namespace Config
{
  namespace
  {
    bool g_initialized = false;
    std::vector<std::string> g_loggedMissingKeys;

    constexpr const char *kNamespaces[] = {"aws", "wifi", "device", "certs", "diag"};

    bool hasLogged(const char *ns, const char *key)
    {
      const std::string token = std::string(ns ? ns : "") + "/" + (key ? key : "");
      if (std::find(g_loggedMissingKeys.begin(), g_loggedMissingKeys.end(), token) !=
          g_loggedMissingKeys.end())
      {
        return true;
      }
      g_loggedMissingKeys.push_back(token);
      return false;
    }

    void logMissing(const char *ns, const char *key)
    {
      if (hasLogged(ns, key))
      {
        return;
      }
      Serial.printf("[CONFIG] Clave faltante %s/%s\n", ns, key);
    }

    esp_err_t ensureInit()
    {
      if (g_initialized)
      {
        return ESP_OK;
      }

      esp_err_t err = nvs_flash_init();
      if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
      {
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_flash_erase());
        err = nvs_flash_init();
      }

      if (err == ESP_OK)
      {
        g_initialized = true;
      }
      else
      {
        Serial.printf("[CONFIG] Error inicializando NVS (%d)\n", static_cast<int>(err));
      }
      return err;
    }
  } // namespace

  void init() { ensureInit(); }

  esp_err_t setString(const char *ns, const char *key, const std::string &value)
  {
    esp_err_t err = ensureInit();
    if (err != ESP_OK)
    {
      return err;
    }

    nvs_handle_t handle;
    err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
      Serial.printf("[CONFIG] No se pudo abrir namespace %s (%d)\n", ns, static_cast<int>(err));
      return err;
    }

    err = nvs_set_str(handle, key, value.c_str());
    if (err == ESP_OK)
    {
      err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK)
    {
      Serial.printf("[CONFIG] Error al guardar %s/%s (%d)\n", ns, key, static_cast<int>(err));
    }
    return err;
  }

  esp_err_t setInt(const char *ns, const char *key, int32_t value)
  {
    esp_err_t err = ensureInit();
    if (err != ESP_OK)
    {
      return err;
    }

    nvs_handle_t handle;
    err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
      Serial.printf("[CONFIG] No se pudo abrir namespace %s (%d)\n", ns, static_cast<int>(err));
      return err;
    }

    err = nvs_set_i32(handle, key, value);
    if (err == ESP_OK)
    {
      err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK)
    {
      Serial.printf("[CONFIG] Error al guardar %s/%s (%d)\n", ns, key, static_cast<int>(err));
    }
    return err;
  }

  std::string getString(const char *ns, const char *key, const std::string &def)
  {
    esp_err_t err = ensureInit();
    if (err != ESP_OK)
    {
      return def;
    }

    nvs_handle_t handle;
    err = nvs_open(ns, NVS_READONLY, &handle);
    if (err != ESP_OK)
    {
      logMissing(ns, key);
      return def;
    }

    size_t length = 0;
    err = nvs_get_str(handle, key, nullptr, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
      nvs_close(handle);
      logMissing(ns, key);
      return def;
    }
    if (err != ESP_OK || length == 0)
    {
      nvs_close(handle);
      return def;
    }

    std::string value;
    value.resize(length);
    err = nvs_get_str(handle, key, &value[0], &length);
    nvs_close(handle);
    if (err != ESP_OK)
    {
      return def;
    }
    if (!value.empty() && value.back() == '\0')
    {
      value.pop_back();
    }
    return value;
  }

  int32_t getInt(const char *ns, const char *key, int32_t def)
  {
    esp_err_t err = ensureInit();
    if (err != ESP_OK)
    {
      return def;
    }

    nvs_handle_t handle;
    err = nvs_open(ns, NVS_READONLY, &handle);
    if (err != ESP_OK)
    {
      logMissing(ns, key);
      return def;
    }

    int32_t value = def;
    err = nvs_get_i32(handle, key, &value);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
      logMissing(ns, key);
      return def;
    }
    if (err != ESP_OK)
    {
      return def;
    }
    return value;
  }

  bool exists(const char *ns, const char *key)
  {
    if (ensureInit() != ESP_OK)
    {
      return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &handle);
    if (err != ESP_OK)
    {
      return false;
    }

    size_t length = 0;
    err = nvs_get_str(handle, key, nullptr, &length);
    if (err == ESP_OK || err == ESP_ERR_NVS_INVALID_LENGTH)
    {
      nvs_close(handle);
      return true;
    }
    if (err == ESP_ERR_NVS_TYPE_MISMATCH)
    {
      int32_t dummy = 0;
      err = nvs_get_i32(handle, key, &dummy);
      nvs_close(handle);
      return err == ESP_OK;
    }

    nvs_close(handle);
    return false;
  }

  void dump()
  {
    if (ensureInit() != ESP_OK)
    {
      return;
    }

    for (const char *ns : kNamespaces)
    {
      Serial.printf("[CONFIG] Namespace '%s'\n", ns);
      nvs_iterator_t it = nvs_entry_find(NVS_DEFAULT_PART_NAME, ns, NVS_TYPE_ANY);
      if (!it)
      {
        Serial.println("  (vacio)");
        continue;
      }

      nvs_handle_t handle;
      if (nvs_open(ns, NVS_READONLY, &handle) != ESP_OK)
      {
        Serial.println("  (no se puede abrir)");
        nvs_release_iterator(it);
        continue;
      }

      nvs_iterator_t first = it;
      while (it)
      {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        switch (info.type)
        {
        case NVS_TYPE_STR:
        {
          size_t size = 0;
          if (nvs_get_str(handle, info.key, nullptr, &size) == ESP_OK && size > 0)
          {
            std::string value;
            value.resize(size);
            if (nvs_get_str(handle, info.key, &value[0], &size) == ESP_OK)
            {
              if (!value.empty() && value.back() == '\0')
              {
                value.pop_back();
              }
              Serial.printf("  %s = %s\n", info.key, value.c_str());
            }
          }
          break;
        }
        case NVS_TYPE_I32:
        {
          int32_t v = 0;
          if (nvs_get_i32(handle, info.key, &v) == ESP_OK)
          {
            Serial.printf("  %s = %ld\n", info.key, static_cast<long>(v));
          }
          break;
        }
        default:
          Serial.printf("  %s = (tipo %u)\n", info.key, static_cast<unsigned>(info.type));
          break;
        }
        it = nvs_entry_next(it);
      }
      nvs_close(handle);
      nvs_release_iterator(first);
    }
  }
} // namespace Config

void config_dump()
{
  Config::dump();
}
