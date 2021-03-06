// Copyright 2016 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "biod/fake_biometric.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <uuid/uuid.h>

#include <algorithm>
#include <utility>
#include <vector>

#include <base/bind.h>
#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/logging.h>
#include <base/values.h>

namespace biod {

const std::string& FakeBiometric::Enrollment::GetId() const {
  return id_;
}

const std::string& FakeBiometric::Enrollment::GetUserId() const {
  InternalEnrollment* internal = GetInternal();
  CHECK(internal)
      << "Attempted to get user ID for invalid Biometric Enrollment";
  return internal->user_id;
}

const std::string& FakeBiometric::Enrollment::GetLabel() const {
  InternalEnrollment* internal = GetInternal();
  CHECK(internal) << "Attempted to get label for invalid Biometric Enrollment";
  return internal->label;
}

bool FakeBiometric::Enrollment::SetLabel(std::string label) {
  InternalEnrollment* internal = GetInternal();
  if (internal) {
    internal->label = std::move(label);
    // Set label by overwriting enrollments in file.
    return biometric_->biod_storage_.WriteEnrollment(
        *this,
        std::unique_ptr<base::Value>(new base::StringValue("Hello, world!")));
  }
  LOG(ERROR) << "Attempt to set label for invalid Biometric Enrollment";
  return false;
}

bool FakeBiometric::Enrollment::Remove() {
  // Delete one enrollment.
  if (biometric_->biod_storage_.DeleteEnrollment(
          biometric_->enrollments_[id_].user_id, id_)) {
    return biometric_->enrollments_.erase(id_) > 0;
  } else {
    return false;
  }
}

FakeBiometric::InternalEnrollment* FakeBiometric::Enrollment::GetInternal()
    const {
  if (!biometric_)
    return nullptr;
  auto internal_enrollment = biometric_->enrollments_.find(id_);
  if (internal_enrollment == biometric_->enrollments_.end())
    return nullptr;
  return &internal_enrollment->second;
}

FakeBiometric::FakeBiometric()
    : session_weak_factory_(this),
      weak_factory_(this),
      biod_storage_(
          "FakeBiometric",
          base::Bind(&FakeBiometric::LoadEnrollment, base::Unretained(this))) {
  const char kFakeInputPath[] = "/tmp/fake_biometric";
  base::DeleteFile(base::FilePath(kFakeInputPath), false);
  CHECK_EQ(mkfifo(kFakeInputPath, 0600), 0)
      << "Failed to create fake biometric input";
  // The pipe gets opened read/write to avoid triggering a constant stream of
  // POLLHUP after the pipe is opened writable and closed. The pipe is never
  // actually written to here.
  fake_input_ = base::ScopedFD(open(kFakeInputPath, O_RDWR | O_NONBLOCK));
  CHECK_GE(fake_input_.get(), 0) << "Failed to open fake biometric input";

  fd_watcher_.reset(new base::MessageLoopForIO::FileDescriptorWatcher);

  CHECK(base::MessageLoopForIO::current()->WatchFileDescriptor(
      fake_input_.get(),
      true,
      base::MessageLoopForIO::WATCH_READ,
      fd_watcher_.get(),
      this))
      << "Failed to watch fake biometric input";
}

Biometric::Type FakeBiometric::GetType() {
  return Biometric::Type::kFingerprint;
}

Biometric::EnrollSession FakeBiometric::StartEnroll(std::string user_id,
                                                    std::string label) {
  if (mode_ == Mode::kNone) {
    mode_ = Mode::kEnroll;
    next_internal_enrollment_ = {std::move(user_id), std::move(label)};
    return Biometric::EnrollSession(session_weak_factory_.GetWeakPtr());
  }
  return Biometric::EnrollSession();
}

Biometric::AuthenticationSession FakeBiometric::StartAuthentication() {
  if (mode_ == Mode::kNone) {
    mode_ = Mode::kAuthentication;
    return Biometric::AuthenticationSession(session_weak_factory_.GetWeakPtr());
  }
  return Biometric::AuthenticationSession();
}

std::vector<std::unique_ptr<Biometric::Enrollment>>
FakeBiometric::GetEnrollments() {
  std::vector<std::unique_ptr<Biometric::Enrollment>> enrollments(
      enrollments_.size());
  std::transform(enrollments_.begin(),
                 enrollments_.end(),
                 enrollments.begin(),
                 [this](decltype(enrollments_)::value_type& enrollment) {
                   return std::unique_ptr<Biometric::Enrollment>(new Enrollment(
                       weak_factory_.GetWeakPtr(), enrollment.first));
                 });
  return enrollments;
}

bool FakeBiometric::DestroyAllEnrollments() {
  // Enumerate through enrollments_ and delete each enrollment.
  bool delete_all_enrollments = true;
  for (auto& enrollment_pair : enrollments_) {
    delete_all_enrollments &= biod_storage_.DeleteEnrollment(
        enrollment_pair.second.user_id, enrollment_pair.first);
  }
  enrollments_.clear();
  return delete_all_enrollments;
}

void FakeBiometric::RemoveEnrollmentsFromMemory() {
  enrollments_.clear();
}

bool FakeBiometric::ReadEnrollments(
    const std::unordered_set<std::string>& user_ids) {
  return biod_storage_.ReadEnrollments(user_ids);
}

void FakeBiometric::SetScannedHandler(const Biometric::ScanCallback& on_scan) {
  on_scan_ = on_scan;
}

void FakeBiometric::SetAttemptHandler(
    const Biometric::AttemptCallback& on_attempt) {
  on_attempt_ = on_attempt;
}

void FakeBiometric::SetFailureHandler(
    const Biometric::FailureCallback& on_failure) {
  on_failure_ = on_failure;
}

void FakeBiometric::EndEnroll() {
  CHECK(mode_ == Mode::kEnroll);
  session_weak_factory_.InvalidateWeakPtrs();
  mode_ = Mode::kNone;
}

void FakeBiometric::EndAuthentication() {
  CHECK(mode_ == Mode::kAuthentication);
  session_weak_factory_.InvalidateWeakPtrs();
  mode_ = Mode::kNone;
}

void FakeBiometric::OnFileCanWriteWithoutBlocking(int fd) {
  NOTREACHED();
}

void FakeBiometric::OnFileCanReadWithoutBlocking(int fd) {
  // We scan the stream for the magic bytes in case the previous input command
  // was not the correct length or malformed for whatever reason. This must be
  // done a single byte at a time because the input stream of bytes is totally
  // unaligned. Reading the length of magic bytes at once might consume some
  // garbage data and the start of the magic bytes, but that would fail to
  // validate, and subsequent reads would never see that correct instance of
  // magic bytes.
  size_t magic_index = 0;
  const uint8_t magic_start[] = {FAKE_BIOMETRIC_MAGIC_BYTES};
  while (magic_index < sizeof(magic_start)) {
    uint8_t magic;
    if (read(fd, &magic, 1) != 1)
      return;
    if (magic == magic_start[magic_index])
      magic_index++;
    else
      magic_index = 0;
  }

  uint8_t cmd;
  if (read(fd, &cmd, 1) != 1)
    return;
  switch (cmd) {
    case 'A': {
      uint8_t res_code;
      if (read(fd, &res_code, 1) != 1)
        return;
      Biometric::ScanResult res = static_cast<Biometric::ScanResult>(res_code);

      uint8_t recognized_count;
      if (read(fd, &recognized_count, 1) != 1)
        return;

      std::vector<std::string> recognized_user_ids(recognized_count);
      for (size_t i = 0; i < recognized_count; i++) {
        uint8_t id_size;
        if (read(fd, &id_size, 1) != 1)
          return;

        std::string& user_id = recognized_user_ids[i];
        user_id.resize(id_size);
        if (read(fd, &user_id.front(), id_size) != id_size)
          return;
        LOG(INFO) << "Recognized User " << user_id;
      }

      if (!on_attempt_.is_null() && mode_ == Mode::kAuthentication)
        on_attempt_.Run(res, recognized_user_ids);
      return;
    }
    case 'S': {
      uint8_t res_code;
      if (read(fd, &res_code, 1) != 1)
        return;
      Biometric::ScanResult res = static_cast<Biometric::ScanResult>(res_code);

      uint8_t done;
      if (read(fd, &done, 1) != 1)
        return;

      LOG(INFO) << "Scan result " << static_cast<int>(res_code) << " done "
                << static_cast<bool>(done);
      if (mode_ == Mode::kEnroll) {
        if (done) {
          std::string enrollment_id(biod_storage_.GenerateNewEnrollmentId());

          enrollments_[enrollment_id] = std::move(next_internal_enrollment_);
          Enrollment current_enrollment(weak_factory_.GetWeakPtr(),
                                        enrollment_id);

          if (!biod_storage_.WriteEnrollment(
                  current_enrollment,
                  std::unique_ptr<base::Value>(
                      new base::StringValue("Hello, world!")))) {
            enrollments_.erase(enrollment_id);
          }

          mode_ = Mode::kNone;
          session_weak_factory_.InvalidateWeakPtrs();
        }

        if (!on_scan_.is_null())
          on_scan_.Run(res, done);
      }
      return;
    }
    case 'F':
      LOG(INFO) << "Fake failure";
      if (!on_failure_.is_null())
        on_failure_.Run();
  }
}

bool FakeBiometric::LoadEnrollment(std::string user_id,
                                   std::string label,
                                   std::string enrollment_id,
                                   base::Value* data) {
  InternalEnrollment internal_enrollment = {std::move(user_id),
                                            std::move(label)};
  enrollments_[enrollment_id] = std::move(internal_enrollment);
  LOG(INFO) << "Load enrollment " << enrollment_id << " from disk.";
  return true;
}
}  // namespace biod
