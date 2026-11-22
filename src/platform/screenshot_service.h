/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>

namespace platform::screenshot {

struct CaptureResult {
    bool success{false};
    std::string path;
    std::string error;
};

CaptureResult capture_active_screen();
// MonsterMesh: snapshot the active screen (incl. lv_layer_top) to an explicit path.
CaptureResult capture_active_screen_to(const std::string& path);

} // namespace platform::screenshot
