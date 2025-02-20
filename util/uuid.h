// SPDX-FileCopyrightText: 2023 David Zero <zero-one@zer0-one.net>
// SPDX-FileCopyrightText: 2023 Robin Lindén <dev@robinlinden.eu>
//
// SPDX-License-Identifier: BSD-2-Clause

#ifndef UTIL_UUID_H_
#define UTIL_UUID_H_

#include <iomanip>
#include <ios>
#include <random>
#include <sstream>
#include <utility>

namespace util {
inline std::string new_uuid() {
    unsigned char data[16];
    std::stringstream uuid_string;
    uuid_string << std::setfill('0');

    std::random_device rando;

    for (size_t i = 0; i < 16; i++) {
        data[i] = static_cast<unsigned char>(rando());
    }

    // Set UUID version bits
    data[6] &= 0x0f;
    data[6] |= 0x40;

    // Set UUID variant bits
    data[8] &= 0x3f;
    data[8] |= 0x80;

    for (size_t i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            uuid_string << '-';
        }

        uuid_string << std::setw(2) << std::hex << +data[i];
    }

    return std::move(uuid_string).str();
}

} // namespace util

#endif
