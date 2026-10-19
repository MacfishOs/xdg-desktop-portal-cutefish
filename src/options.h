// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "base/dbus_types.h"

#include <cstdint>
#include <string>
#include <vector>

// Values of an a{sv} option map. A missing key gives the fallback; a value of
// another type is converted where that makes sense (numbers of any width,
// numbers as booleans, a string for a string list), and gives false, 0 or
// empty otherwise, which is what clients have always been able to rely on.
bool optionBool(const cutefish::base::dbus::VariantMap &options, const std::string &key, bool fallback);
uint32_t optionUInt(const cutefish::base::dbus::VariantMap &options, const std::string &key, uint32_t fallback);
std::string optionString(const cutefish::base::dbus::VariantMap &options, const std::string &key);

bool toBool(const cutefish::base::dbus::Variant &value);
uint32_t toUInt(const cutefish::base::dbus::Variant &value);
std::string toString(const cutefish::base::dbus::Variant &value);
std::vector<std::string> toStringList(const cutefish::base::dbus::Variant &value);
// The bytes of an "ay", or a string's.
std::string toBytes(const cutefish::base::dbus::Variant &value);
