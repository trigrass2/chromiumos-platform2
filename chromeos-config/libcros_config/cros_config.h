// Copyright 2016 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Library to provide access to the Chrome OS master configuration

#ifndef CHROMEOS_CONFIG_LIBCROS_CONFIG_CROS_CONFIG_H_
#define CHROMEOS_CONFIG_LIBCROS_CONFIG_CROS_CONFIG_H_

#include "chromeos-config/libcros_config/cros_config_interface.h"

#include <string>

#include <base/macros.h>
#include <brillo/brillo_export.h>

namespace base {
class CommandLine;
class FilePath;
}

namespace brillo {

class BRILLO_EXPORT CrosConfig : public CrosConfigInterface {
 public:
  CrosConfig();
  ~CrosConfig() override;

  // Prepare the configuration system for use.
  // This reads the configuration file into memory.
  // @return true if OK, false on error.
  bool Init();

  // Prepare the configuration system for testing.
  // This reads in the given configuration file and selects the supplied
  // model name.
  // @filepath: Patch to configuration .dtb file.
  // @model: Model name (e.g. 'reef').
  // @return true if OK, false on error.
  bool InitForTest(const base::FilePath& filepath, const std::string& model);

  // CrosConfigInterface:
  bool GetString(const std::string& path,
                 const std::string& prop,
                 std::string* val_out) override;

 private:
  // Common init function for both production and test code.
  // @filepath: path to configuration .dtb file.
  // @cmdline: command line to execute to find out the current model. This is
  // normally something that runs the 'mosys' tool.
  bool InitCommon(const base::FilePath& filepath,
                  const base::CommandLine& cmdline);

  std::string blob_;       // Device tree binary blob
  std::string model_;      // Model name for this device
  int model_offset_ = -1;  // Device tree offset of the model's node
  bool inited_ = false;    // true if the class is ready for use (Init() called)
  DISALLOW_COPY_AND_ASSIGN(CrosConfig);
};

}  // namespace brillo

#endif  // CHROMEOS_CONFIG_LIBCROS_CONFIG_CROS_CONFIG_H_
