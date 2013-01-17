// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>

#include "base/command_line.h"
#include "base/logging.h"
#include "power_manager/common/fake_prefs.h"
#include "power_manager/common/power_constants.h"
#include "power_manager/powerd/internal_backlight_controller.h"
#include "power_manager/powerd/system/mock_backlight.h"

using ::testing::_;
using ::testing::DoAll;
using ::testing::NotNull;
using ::testing::Return;
using ::testing::SaveArg;
using ::testing::SetArgumentPointee;
using ::testing::Test;

namespace power_manager {

static const int64 kPluggedBrightness = 7;
static const int64 kDefaultBrightness = 5;
static const int64 kUnpluggedBrightness = 3;
static const int64 kMaxBrightness = 10;
static const int64 kPluggedBrightnessP = kPluggedBrightness * 100 /
                                                          kMaxBrightness;
static const int64 kUnpluggedBrightnessP = kUnpluggedBrightness * 100 /
                                                          kMaxBrightness;

class PlugDimmerTest : public Test {
 public:
  PlugDimmerTest()
      : backlight_ctl_(&backlight_, &prefs_, NULL),
        current_brightness_(0),
        target_brightness_(0) {
    EXPECT_CALL(backlight_, GetCurrentBrightnessLevel(NotNull()))
        .WillRepeatedly(DoAll(SetArgumentPointee<0>(current_brightness_),
                              Return(true)));
    EXPECT_CALL(backlight_, GetMaxBrightnessLevel(NotNull()))
        .WillRepeatedly(DoAll(SetArgumentPointee<0>(kMaxBrightness),
                              Return(true)));
    EXPECT_CALL(backlight_, SetBrightnessLevel(_, _))
        .WillRepeatedly(DoAll(SaveArg<0>(&current_brightness_),
                              SaveArg<0>(&target_brightness_),
                              Return(true)));

    prefs_.SetDouble(kPluggedBrightnessOffsetPref, kPluggedBrightnessP);
    prefs_.SetDouble(kUnpluggedBrightnessOffsetPref, kUnpluggedBrightnessP);
    CHECK(backlight_ctl_.Init());
  }

 protected:
  system::MockBacklight backlight_;
  FakePrefs prefs_;
  InternalBacklightController backlight_ctl_;

  int64 current_brightness_;
  int64 target_brightness_;
};

// Test that OnPlugEvent sets the brightness appropriately when
// the computer is plugged and unplugged.
TEST_F(PlugDimmerTest, TestPlug) {
  backlight_ctl_.OnPlugEvent(false);
  backlight_.ExpectSetBrightnessLevelRepeatedly(kUnpluggedBrightness, true);
  backlight_ctl_.SetPowerState(BACKLIGHT_ACTIVE);
  backlight_.ExpectSetBrightnessLevelRepeatedly(kPluggedBrightness, true);
  backlight_ctl_.OnPlugEvent(true);
  backlight_.ExpectSetBrightnessLevelRepeatedly(kUnpluggedBrightness, true);
  backlight_ctl_.OnPlugEvent(false);
  backlight_.ExpectSetBrightnessLevelRepeatedly(kPluggedBrightness, true);
  backlight_ctl_.OnPlugEvent(true);
}

// Test that OnPlugEvent sets the brightness appropriately when
// the computer is unplugged and plugged.
TEST_F(PlugDimmerTest, TestUnplug) {
  backlight_ctl_.OnPlugEvent(true);
  backlight_.ExpectSetBrightnessLevelRepeatedly(kPluggedBrightness, true);
  backlight_ctl_.SetPowerState(BACKLIGHT_ACTIVE);
  backlight_.ExpectSetBrightnessLevelRepeatedly(kUnpluggedBrightness, true);
  backlight_ctl_.OnPlugEvent(false);
  backlight_.ExpectSetBrightnessLevelRepeatedly(kPluggedBrightness, true);
  backlight_ctl_.OnPlugEvent(true);
  backlight_.ExpectSetBrightnessLevelRepeatedly(kUnpluggedBrightness, true);
  backlight_ctl_.OnPlugEvent(false);
}

// Test that OnPlugEvent does not mess with the user's brightness settings
// when we receive duplicate plug events.
TEST_F(PlugDimmerTest, TestDuplicatePlugEvent) {
  backlight_ctl_.OnPlugEvent(false);
  EXPECT_CALL(backlight_, SetBrightnessLevel(kUnpluggedBrightness, _))
      .Times(0);
  backlight_ctl_.SetPowerState(BACKLIGHT_ACTIVE);
  backlight_ctl_.OnPlugEvent(false);
  backlight_ctl_.SetPowerState(BACKLIGHT_ACTIVE);
  backlight_ctl_.OnPlugEvent(false);
}

}  // namespace power_manager
