// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdint.h>

#include <cmath>
#include <cstdio>

#include <base/command_line.h>
#include <base/format_macros.h>
#include <base/logging.h>
#include <base/time/time.h>
#include <brillo/flag_helper.h>

#include "power_manager/common/power_constants.h"
#include "power_manager/powerd/system/internal_backlight.h"

namespace {

int64_t PercentToLevel(
    const power_manager::system::BacklightInterface& backlight,
    double percent,
    int64_t max_level) {
  percent = std::min(percent, 100.0);
  return static_cast<int64_t>(roundl(percent * max_level / 100.0));
}

}  // namespace

int main(int argc, char* argv[]) {
  DEFINE_bool(get_brightness, false, "Print current brightness level");
  DEFINE_bool(get_brightness_percent,
              false,
              "Print current brightness as linearly-calculated percent");
  DEFINE_bool(get_max_brightness, false, "Print max brightness level");
  DEFINE_bool(keyboard, false, "Use keyboard (rather than panel) backlight");
  DEFINE_int64(set_brightness, -1, "Set brightness level");
  DEFINE_double(set_brightness_percent,
                -1.0,
                "Set brightness as "
                "linearly-calculated percent in [0.0, 100.0]");
  DEFINE_int64(set_resume_brightness,
               -2,
               "Set brightness level on resume; -1 clears current level");
  DEFINE_double(set_resume_brightness_percent,
                -1.0,
                "Set resume brightness as linearly-calculated percent in "
                "[0.0, 100.0]");

  brillo::FlagHelper::Init(
      argc,
      argv,
      "Print or set the internal panel or keyboard backlight's brightness.");

  CHECK_LT((FLAGS_get_brightness + FLAGS_get_max_brightness +
            FLAGS_get_brightness_percent),
           2)
      << "-get_brightness, -get_brightness_percent, and -get_max_brightness "
      << "are mutually exclusive";
  CHECK(FLAGS_set_brightness < 0 || FLAGS_set_brightness_percent < 0.0)
      << "-set_brightness and -set_brightness_percent are mutually exclusive";
  CHECK(FLAGS_set_resume_brightness < -1 ||
        FLAGS_set_resume_brightness_percent < 0.0)
      << "-set_resume_brightness and -set_resume_brightness_percent are "
      << "mutually exclusive";

  power_manager::system::InternalBacklight backlight;
  if (!backlight.Init(
          base::FilePath(FLAGS_keyboard
                             ? power_manager::kKeyboardBacklightPath
                             : power_manager::kInternalBacklightPath),
          FLAGS_keyboard ? power_manager::kKeyboardBacklightPattern
                         : power_manager::kInternalBacklightPattern)) {
    return 1;
  }

  const int64_t level = backlight.GetCurrentBrightnessLevel();
  const int64_t max_level = backlight.GetMaxBrightnessLevel();

  if (FLAGS_get_brightness)
    printf("%" PRIi64 "\n", level);
  if (FLAGS_get_max_brightness)
    printf("%" PRIi64 "\n", max_level);
  if (FLAGS_get_brightness_percent)
    printf("%f\n", level * 100.0 / max_level);

  if (FLAGS_set_brightness >= 0) {
    CHECK(
        backlight.SetBrightnessLevel(FLAGS_set_brightness, base::TimeDelta()));
  }
  if (FLAGS_set_brightness_percent >= 0.0) {
    int64_t new_level =
        PercentToLevel(backlight, FLAGS_set_brightness_percent, max_level);
    CHECK(backlight.SetBrightnessLevel(new_level, base::TimeDelta()));
  }

  // -1 clears the currently-set resume brightness.
  if (FLAGS_set_resume_brightness >= -1)
    CHECK(backlight.SetResumeBrightnessLevel(FLAGS_set_resume_brightness));
  if (FLAGS_set_resume_brightness_percent >= 0.0) {
    int64_t new_level = PercentToLevel(
        backlight, FLAGS_set_resume_brightness_percent, max_level);
    CHECK(backlight.SetResumeBrightnessLevel(new_level));
  }

  return 0;
}
