// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Unit tests for MountTask.

#include "cryptohome/mount_task.h"

#include <base/at_exit.h>
#include <base/bind.h>
#include <base/logging.h>
#include <base/synchronization/waitable_event.h>
#include <base/threading/platform_thread.h>
#include <base/threading/thread.h>
#include <base/time/time.h>
#include <gtest/gtest.h>

#include "cryptohome/mock_homedirs.h"
#include "cryptohome/mock_mount.h"

using base::PlatformThread;

namespace cryptohome {
using ::testing::Return;
using ::testing::_;

class MountTaskTest : public ::testing::Test {
 public:
  MountTaskTest()
      : runner_("RunnerThread"),
        event_(true, false),
        mount_(new MockMount),
        homedirs_(),
        result_() {
    wait_time_ = base::TimeDelta::FromSeconds(180);
  }

  virtual ~MountTaskTest() { }

  void SetUp() {
    runner_.Start();
  }

  void TearDown() {
    if (runner_.IsRunning()) {
      runner_.Stop();
    }
  }

 protected:
  base::Thread runner_;
  base::WaitableEvent event_;
  scoped_refptr<MockMount> mount_;
  MockHomeDirs homedirs_;
  MountTaskResult result_;
  base::TimeDelta wait_time_;
  UsernamePasskey empty_credentials_;

 private:
  DISALLOW_COPY_AND_ASSIGN(MountTaskTest);
};

class MountTaskNotifier : public MountTaskObserver {
 public:
  MountTaskNotifier()
      : notified_(false) { }
  virtual ~MountTaskNotifier() { }

  // MountTaskObserver
  virtual bool MountTaskObserve(const MountTaskResult& result) {
    notified_ = true;
    return false;
  }

  bool notified_;
};

TEST_F(MountTaskTest, ResultCopyConstructorTest) {
  MountTaskResult result1;

  result1.set_sequence_id(1337);
  result1.set_return_status(true);
  result1.set_return_code(MOUNT_ERROR_FATAL);

  MountTaskResult result2(result1);

  ASSERT_EQ(result1.sequence_id(), result2.sequence_id());
  ASSERT_EQ(result1.return_status(), result2.return_status());
  ASSERT_EQ(result1.return_code(), result2.return_code());
}

TEST_F(MountTaskTest, ResultEqualsTest) {
  MountTaskResult result1;

  result1.set_sequence_id(1337);
  result1.set_return_status(true);
  result1.set_return_code(MOUNT_ERROR_FATAL);

  MountTaskResult result2;
  result2 = result1;

  ASSERT_EQ(result1.sequence_id(), result2.sequence_id());
  ASSERT_EQ(result1.return_status(), result2.return_status());
  ASSERT_EQ(result1.return_code(), result2.return_code());
}

TEST_F(MountTaskTest, EventTest) {
  ASSERT_FALSE(event_.IsSignaled());
  scoped_refptr<MountTask> mount_task
      = new MountTask(NULL, NULL);
  mount_task->set_complete_event(&event_);
  mount_task->set_result(&result_);
  runner_.message_loop()->PostTask(FROM_HERE,
      base::Bind(&MountTask::Run, mount_task.get()));
  event_.TimedWait(wait_time_);
  ASSERT_TRUE(event_.IsSignaled());
}

TEST_F(MountTaskTest, ObserveTest) {
  MountTaskNotifier notifier;
  scoped_refptr<MountTask> mount_task
      = new MountTask(&notifier, NULL);
  mount_task->set_result(&result_);
  runner_.message_loop()->PostTask(FROM_HERE,
      base::Bind(&MountTask::Run, mount_task.get()));
  for (unsigned int i = 0; i < 64; i++) {
    if (!notifier.notified_) {
      PlatformThread::Sleep(base::TimeDelta::FromMilliseconds(100));
    } else {
      break;
    }
  }
  ASSERT_TRUE(notifier.notified_);
}

TEST_F(MountTaskTest, NopTest) {
  ASSERT_FALSE(event_.IsSignaled());
  scoped_refptr<MountTaskNop> mount_task = new MountTaskNop(NULL);
  mount_task->set_complete_event(&event_);
  mount_task->set_result(&result_);
  runner_.message_loop()->PostTask(FROM_HERE,
      base::Bind(&MountTaskNop::Run, mount_task.get()));
  event_.TimedWait(wait_time_);
  ASSERT_TRUE(event_.IsSignaled());
}

TEST_F(MountTaskTest, MountTest) {
  EXPECT_CALL(*mount_, MountCryptohome(_, _, _))
      .WillOnce(Return(true));

  ASSERT_FALSE(event_.IsSignaled());
  Mount::MountArgs mount_args;
  scoped_refptr<MountTaskMount> mount_task =
      new MountTaskMount(NULL, mount_.get(), empty_credentials_, mount_args);
  mount_task->set_complete_event(&event_);
  mount_task->set_result(&result_);
  runner_.message_loop()->PostTask(FROM_HERE,
      base::Bind(&MountTaskMount::Run, mount_task.get()));
  event_.TimedWait(wait_time_);
  ASSERT_TRUE(event_.IsSignaled());
}

TEST_F(MountTaskTest, MountGuestTest) {
  EXPECT_CALL(*mount_, MountGuestCryptohome())
      .WillOnce(Return(true));

  ASSERT_FALSE(event_.IsSignaled());
  scoped_refptr<MountTaskMountGuest> mount_task
      = new MountTaskMountGuest(NULL, mount_.get());
  mount_task->set_complete_event(&event_);
  mount_task->set_result(&result_);
  runner_.message_loop()->PostTask(FROM_HERE,
      base::Bind(&MountTaskMountGuest::Run, mount_task.get()));
  event_.TimedWait(wait_time_);
  ASSERT_TRUE(event_.IsSignaled());
}

TEST_F(MountTaskTest, MigratePasskeyTest) {
  MockHomeDirs homedirs;
  EXPECT_CALL(homedirs, Migrate(_, _))
      .WillOnce(Return(true));

  ASSERT_FALSE(event_.IsSignaled());
  scoped_refptr<MountTaskMigratePasskey> mount_task
      = new MountTaskMigratePasskey(NULL, &homedirs, empty_credentials_, "");
  mount_task->set_complete_event(&event_);
  mount_task->set_result(&result_);
  runner_.message_loop()->PostTask(FROM_HERE,
      base::Bind(&MountTaskMigratePasskey::Run, mount_task.get()));
  event_.TimedWait(wait_time_);
  ASSERT_TRUE(event_.IsSignaled());
}

TEST_F(MountTaskTest, AddPasskeyTest) {
  MockHomeDirs homedirs;
  EXPECT_CALL(homedirs, AddKeyset(_, _, _, _, _))
      .WillOnce(Return(CRYPTOHOME_ERROR_NOT_SET));

  ASSERT_FALSE(event_.IsSignaled());
  scoped_refptr<MountTaskAddPasskey> mount_task
      = new MountTaskAddPasskey(NULL, &homedirs, empty_credentials_, "");
  mount_task->set_complete_event(&event_);
  mount_task->set_result(&result_);
  runner_.message_loop()->PostTask(FROM_HERE,
      base::Bind(&MountTaskAddPasskey::Run, mount_task.get()));
  event_.TimedWait(wait_time_);
  ASSERT_TRUE(event_.IsSignaled());
}

TEST_F(MountTaskTest, UnmountTest) {
  EXPECT_CALL(*mount_, UnmountCryptohome())
      .WillOnce(Return(true));

  ASSERT_FALSE(event_.IsSignaled());
  scoped_refptr<MountTaskUnmount> mount_task
      = new MountTaskUnmount(NULL, mount_.get());
  mount_task->set_complete_event(&event_);
  mount_task->set_result(&result_);
  runner_.message_loop()->PostTask(FROM_HERE,
      base::Bind(&MountTaskUnmount::Run, mount_task.get()));
  event_.TimedWait(wait_time_);
  ASSERT_TRUE(event_.IsSignaled());
}

TEST_F(MountTaskTest, TestCredentialsMountTest) {
  EXPECT_CALL(*mount_, AreValid(_))
      .WillOnce(Return(true));

  ASSERT_FALSE(event_.IsSignaled());
  scoped_refptr<MountTaskTestCredentials> mount_task
      = new MountTaskTestCredentials(NULL, mount_.get(), NULL,
                                     empty_credentials_);
  mount_task->set_complete_event(&event_);
  mount_task->set_result(&result_);
  runner_.message_loop()->PostTask(FROM_HERE,
      base::Bind(&MountTaskTestCredentials::Run, mount_task.get()));
  event_.TimedWait(wait_time_);
  ASSERT_TRUE(event_.IsSignaled());
}

TEST_F(MountTaskTest, TestCredentialsHomeDirsTest) {
  MockHomeDirs homedirs;
  EXPECT_CALL(homedirs, AreCredentialsValid(_))
      .WillOnce(Return(true));

  ASSERT_FALSE(event_.IsSignaled());
  scoped_refptr<MountTaskTestCredentials> mount_task
      = new MountTaskTestCredentials(NULL, NULL, &homedirs, empty_credentials_);
  mount_task->set_complete_event(&event_);
  mount_task->set_result(&result_);
  runner_.message_loop()->PostTask(FROM_HERE,
      base::Bind(&MountTaskTestCredentials::Run, mount_task.get()));
  event_.TimedWait(wait_time_);
  ASSERT_TRUE(event_.IsSignaled());
}

TEST_F(MountTaskTest, RemoveTest) {
  MockHomeDirs homedirs;
  EXPECT_CALL(homedirs, Remove(_))
      .WillOnce(Return(true));

  ASSERT_FALSE(event_.IsSignaled());
  scoped_refptr<MountTaskRemove> mount_task
      = new MountTaskRemove(NULL, NULL, empty_credentials_, &homedirs);
  mount_task->set_complete_event(&event_);
  mount_task->set_result(&result_);
  runner_.message_loop()->PostTask(FROM_HERE,
      base::Bind(&MountTaskRemove::Run, mount_task.get()));
  event_.TimedWait(wait_time_);
  ASSERT_TRUE(event_.IsSignaled());
}

TEST_F(MountTaskTest, ResetTpmContext) {
  ASSERT_FALSE(event_.IsSignaled());
  scoped_refptr<MountTaskResetTpmContext> mount_task
      = new MountTaskResetTpmContext(NULL, NULL);
  mount_task->set_complete_event(&event_);
  mount_task->set_result(&result_);
  runner_.message_loop()->PostTask(FROM_HERE,
      base::Bind(&MountTaskResetTpmContext::Run, mount_task.get()));
  event_.TimedWait(wait_time_);
  ASSERT_TRUE(event_.IsSignaled());
}

TEST_F(MountTaskTest, AutomaticFreeDiskSpace) {
  ASSERT_FALSE(event_.IsSignaled());
  scoped_refptr<MountTaskAutomaticFreeDiskSpace> mount_task
      = new MountTaskAutomaticFreeDiskSpace(NULL, &homedirs_);
  mount_task->set_complete_event(&event_);
  mount_task->set_result(&result_);
  runner_.message_loop()->PostTask(FROM_HERE,
      base::Bind(&MountTaskAutomaticFreeDiskSpace::Run, mount_task.get()));
  event_.TimedWait(wait_time_);
  ASSERT_TRUE(event_.IsSignaled());
}

}  // namespace cryptohome
