// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POWER_MANAGER_COMMON_TEST_MAIN_LOOP_RUNNER_H_
#define POWER_MANAGER_COMMON_TEST_MAIN_LOOP_RUNNER_H_

#include "base/basictypes.h"
#include "base/memory/scoped_ptr.h"
#include "base/time.h"
#include "base/timer.h"

namespace base {
class RunLoop;
}

namespace power_manager {

// This class makes it easier to write tests that need to wait for an
// asynchronous event to be run by the event loop.  The typical usage pattern is
// as follows:
//
// 1. Instantiate a TestMainLoopRunner.
// 2. Set up a callback for the asynchronous event that calls StopLoop().
// 3. Call StartLoop() and test that it returns true.
// 4. Test that the asynchronous event included the expected data.
class TestMainLoopRunner {
 public:
  TestMainLoopRunner();
  ~TestMainLoopRunner();

  // Runs |loop_| until StopLoop() is called or |timeout_delay| has elapsed.
  // Returns true if the loop was stopped via StopLoop() or false if the timeout
  // was hit.
  bool StartLoop(base::TimeDelta timeout_delay);

  // Stops |loop_|, resulting in control returning to StartLoop() (which will
  // return true).
  void StopLoop();

 private:
  // Called when |timeout_delay| elapses without the loop having been stopped.
  void OnTimeout();

  scoped_ptr<base::RunLoop> runner_;

  // Invokes OnTimeout().
  base::OneShotTimer<TestMainLoopRunner> timeout_timer_;

  // Was the loop stopped as a result of OnTimeout() being called rather than
  // StopLoop()?
  bool timed_out_;

  DISALLOW_COPY_AND_ASSIGN(TestMainLoopRunner);
};

}  // namespace power_manager

#endif  // POWER_MANAGER_COMMON_TEST_MAIN_LOOP_RUNNER_H_
