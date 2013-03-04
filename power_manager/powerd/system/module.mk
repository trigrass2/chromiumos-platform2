# Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

include common.mk

CXX_STATIC_LIBRARY(powerd/libsystem.pie.a): \
	powerd/system/ambient_light_sensor.o \
	powerd/system/async_file_reader.o \
	powerd/system/audio_detector.o \
	powerd/system/display_power_setter.o \
	powerd/system/external_backlight.o \
	powerd/system/input.o \
	powerd/system/internal_backlight.o \
	powerd/system/peripheral_battery_watcher.o
CXX_STATIC_LIBRARY(powerd/libsystem.pie.a): CPPFLAGS += \
	$(GLIB_FLAGS) $(shell $(PKG_CONFIG) --cflags libcras)
CXX_STATIC_LIBRARY(powerd/libsystem.pie.a): LDLIBS += \
	$(GLIB_LIBS) -lrt -ludev $(shell $(PKG_CONFIG) --libs libcras)
clean: CLEAN(powerd/libsystem.pie.a)

CXX_STATIC_LIBRARY(powerd/libsystem_test.pie.a): \
	powerd/system/ambient_light_sensor_stub.o \
	powerd/system/backlight_stub.o \
	powerd/system/display_power_setter_stub.o
clean: CLEAN(powerd/libsystem_test.pie.a)

CXX_BINARY(powerd/system_unittest): \
	powerd/system/ambient_light_sensor_unittest.o \
	powerd/system/async_file_reader_unittest.o \
	powerd/system/input_unittest.o \
	powerd/system/internal_backlight_unittest.o \
	powerd/system/peripheral_battery_watcher_unittest.o \
	CXX_STATIC_LIBRARY(powerd/libsystem.pie.a) \
	CXX_STATIC_LIBRARY(common/libtestrunner.pie.a) \
	CXX_STATIC_LIBRARY(common/libutil.pie.a) \
	CXX_STATIC_LIBRARY(common/libutil_test.pie.a)
CXX_BINARY(powerd/system_unittest): \
	CPPFLAGS += $(GLIB_FLAGS)
CXX_BINARY(powerd/system_unittest): \
	LDLIBS += $(GLIB_LIBS) -lgtest -lgmock -lrt -ludev
clean: CXX_BINARY(powerd/system_unittest)
tests: TEST(CXX_BINARY(powerd/system_unittest))
