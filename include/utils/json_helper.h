/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#if __has_include("cJSON.h")
#include "cJSON.h"
#elif __has_include(<cjson/cJSON.h>)
#include <cjson/cJSON.h>
#else
#error "cJSON header not found"
#endif
#include <cstddef>

namespace util {

class JsonValue {
public:
    JsonValue(cJSON *node = nullptr) : node_(node) {};

    bool is_valid() const { return node_ != nullptr; }

    bool is_object() const { return node_ && node_->type == cJSON_Object; }

    bool is_array() const { return node_ && node_->type == cJSON_Array; }

    bool is_string() const { return node_ && node_->type == cJSON_String; }

    bool is_number() const { return node_ && cJSON_IsNumber(node_); }

    int as_int(int def = 0) const {
        return (node_ && cJSON_IsNumber(node_)) ? node_->valueint : def;
    }
    
    float as_float(float def = 0.0f) const {
        return (node_ && cJSON_IsNumber(node_)) ? (float)node_->valuedouble : def;
    }

    double as_double(double def = 0.0) const {
        return (node_ && cJSON_IsNumber(node_)) ? (double)node_->valuedouble : def;
    }

    const char *as_string(const char *def = "") const {
        return (node_ && cJSON_IsString(node_) && node_->valuestring)
               ? node_->valuestring
               : def;
    }

    bool as_bool(bool def = false) const {
        return node_ ? cJSON_IsTrue(node_) : def;
    }

    JsonValue operator[](const char *key) const {
        if (!node_) return nullptr;
        return JsonValue(cJSON_GetObjectItem(node_, key));        
    }

    JsonValue operator[](int index) const {
        if (!node_) return nullptr;
        return JsonValue(cJSON_GetArrayItem(node_, index));        
    }

    cJSON* raw() const { return node_; }
private:
    cJSON *node_{nullptr};
};


class Json {

public:
    Json() : root_(nullptr) {}

    explicit Json(const char *str) { parse(str); }
    ~Json() { clear(); }

    Json(const Json&) = delete;
    Json& operator=(const Json&) = delete;

    Json(Json&& other) noexcept {
        root_ = other.root_;
        other.root_ = nullptr;
    }

    Json& operator=(Json&& other) noexcept {
        if ( this != &other ) {
            clear();
            root_ = other.root_;
            other.root_ = nullptr;
        }
        return *this;
    }

    bool parse(const char *str) {
        clear();
        if ( !str ) return false;

        root_ = cJSON_Parse(str);
        return root_ != nullptr;
    }

    char *dump() const {
        if (!root_) return nullptr;
        return cJSON_PrintUnformatted(root_);
    }

    cJSON *raw() const { return root_; }

    JsonValue operator[](const char *key) {
        return JsonValue(cJSON_GetObjectItem(root_, key));
    }

    JsonValue operator[](int index) {
        return JsonValue(cJSON_GetArrayItem(root_, index));
    }

    bool valid() { return root_ != nullptr; }

protected:
    void clear() {
        if(root_) {
            cJSON_Delete(root_);
            root_ = nullptr;
        }
    }

private:
    cJSON *root_{nullptr};
};

} // namespace util
