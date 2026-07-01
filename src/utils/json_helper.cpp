/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "utils/json_helper.h"

#include <fstream>
#include <iterator>

namespace util {

bool Json::load_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    clear();
    return false;
  }

  const std::string content((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
  return parse(content.c_str());
}

bool Json::save_file(const std::string& path, bool formatted) const {
  if (!root_) {
    return false;
  }

  const std::string content = to_string(formatted);
  if (content.empty()) {
    return false;
  }

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }

  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  output.put('\n');
  return static_cast<bool>(output);
}

std::string Json::to_string(bool formatted) const {
  if (!root_) {
    return {};
  }

  char* printed = formatted ? cJSON_Print(root_) : cJSON_PrintUnformatted(root_);
  if (!printed) {
    return {};
  }

  std::string result(printed);
  cJSON_free(printed);
  return result;
}

Json Json::object() { return Json(cJSON_CreateObject()); }

Json Json::array() { return Json(cJSON_CreateArray()); }

cJSON* Json::string(const char* value) { return cJSON_CreateString(value ? value : ""); }

cJSON* Json::number(double value) { return cJSON_CreateNumber(value); }

cJSON* Json::boolean(bool value) { return cJSON_CreateBool(value); }

cJSON* Json::null() { return cJSON_CreateNull(); }

bool Json::add(const char* key, cJSON* item) {
  if (!root_ || !cJSON_IsObject(root_) || !key || !item) {
    if (item) {
      cJSON_Delete(item);
    }
    return false;
  }
  return cJSON_AddItemToObject(root_, key, item) != 0;
}

bool Json::append(cJSON* item) {
  if (!root_ || !cJSON_IsArray(root_) || !item) {
    if (item) {
      cJSON_Delete(item);
    }
    return false;
  }
  return cJSON_AddItemToArray(root_, item) != 0;
}

}  // namespace util
