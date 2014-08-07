// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "update_engine/update_manager/evaluation_context.h"

#include <algorithm>
#include <string>

#include <base/bind.h>
#include <base/json/json_writer.h>
#include <base/strings/string_util.h>
#include <base/values.h>

#include "update_engine/utils.h"

using base::Closure;
using base::Time;
using base::TimeDelta;
using chromeos_update_engine::ClockInterface;
using std::string;

namespace {

// Returns whether |curr_time| surpassed |ref_time|; if not, also checks whether
// |ref_time| is sooner than the current value of |*reeval_time|, in which case
// the latter is updated to the former.
bool IsTimeGreaterThanHelper(base::Time ref_time, base::Time curr_time,
                             base::Time* reeval_time) {
  if (curr_time > ref_time)
    return true;
  // Remember the nearest reference we've checked against in this evaluation.
  if (*reeval_time > ref_time)
    *reeval_time = ref_time;
  return false;
}

// If |expires| never happens (maximal value), returns the maximal interval;
// otherwise, returns the difference between |expires| and |curr|.
TimeDelta GetTimeout(base::Time curr, base::Time expires) {
  if (expires.is_max())
    return TimeDelta::Max();
  return expires - curr;
}

}  // namespace

namespace chromeos_update_manager {

EvaluationContext::EvaluationContext(ClockInterface* clock,
                                     TimeDelta evaluation_timeout,
                                     TimeDelta expiration_timeout)
    : clock_(clock),
      evaluation_timeout_(evaluation_timeout),
      expiration_timeout_(expiration_timeout),
      weak_ptr_factory_(this) {
  ResetEvaluation();
  ResetExpiration();
}

EvaluationContext::~EvaluationContext() {
  RemoveObserversAndTimeout();
}

void EvaluationContext::RemoveObserversAndTimeout() {
  for (auto& it : value_cache_) {
    if (it.first->GetMode() == kVariableModeAsync)
      it.first->RemoveObserver(this);
  }
  CancelMainLoopEvent(timeout_event_);
  timeout_event_ = kEventIdNull;
}

TimeDelta EvaluationContext::RemainingTime(Time monotonic_deadline) const {
  if (monotonic_deadline.is_max())
    return TimeDelta::Max();
  TimeDelta remaining = monotonic_deadline - clock_->GetMonotonicTime();
  return std::max(remaining, TimeDelta());
}

Time EvaluationContext::MonotonicDeadline(TimeDelta timeout) {
  return (timeout.is_max() ? Time::Max() :
          clock_->GetMonotonicTime() + timeout);
}

void EvaluationContext::ValueChanged(BaseVariable* var) {
  DLOG(INFO) << "ValueChanged() called for variable " << var->GetName();
  OnValueChangedOrTimeout();
}

void EvaluationContext::OnTimeout() {
  DLOG(INFO) << "OnTimeout() called due to "
             << (timeout_marks_expiration_ ? "expiration" : "poll interval");
  timeout_event_ = kEventIdNull;
  is_expired_ = timeout_marks_expiration_;
  OnValueChangedOrTimeout();
}

void EvaluationContext::OnValueChangedOrTimeout() {
  RemoveObserversAndTimeout();

  // Copy the callback handle locally, allowing it to be reassigned.
  scoped_ptr<Closure> callback(callback_.release());

  if (callback.get())
    callback->Run();
}

bool EvaluationContext::IsWallclockTimeGreaterThan(base::Time timestamp) {
  return IsTimeGreaterThanHelper(timestamp, evaluation_start_wallclock_,
                                 &reevaluation_time_wallclock_);
}

bool EvaluationContext::IsMonotonicTimeGreaterThan(base::Time timestamp) {
  return IsTimeGreaterThanHelper(timestamp, evaluation_start_monotonic_,
                                 &reevaluation_time_monotonic_);
}

void EvaluationContext::ResetEvaluation() {
  evaluation_start_wallclock_ = clock_->GetWallclockTime();
  evaluation_start_monotonic_ = clock_->GetMonotonicTime();
  reevaluation_time_wallclock_ = Time::Max();
  reevaluation_time_monotonic_ = Time::Max();
  evaluation_monotonic_deadline_ = MonotonicDeadline(evaluation_timeout_);

  // Remove the cached values of non-const variables
  for (auto it = value_cache_.begin(); it != value_cache_.end(); ) {
    if (it->first->GetMode() == kVariableModeConst) {
      ++it;
    } else {
      it = value_cache_.erase(it);
    }
  }
}

void EvaluationContext::ResetExpiration() {
  expiration_monotonic_deadline_ = MonotonicDeadline(expiration_timeout_);
  is_expired_ = false;
}

bool EvaluationContext::RunOnValueChangeOrTimeout(Closure callback) {
  // Check that the method was not called more than once.
  if (callback_.get() != nullptr) {
    LOG(ERROR) << "RunOnValueChangeOrTimeout called more than once.";
    return false;
  }

  // Check that the context did not yet expire.
  if (is_expired()) {
    LOG(ERROR) << "RunOnValueChangeOrTimeout called on an expired context.";
    return false;
  }

  // Handle reevaluation due to a Is{Wallclock,Monotonic}TimeGreaterThan(). We
  // choose the smaller of the differences between evaluation start time and
  // reevaluation time among the wallclock and monotonic scales.
  TimeDelta timeout = std::min(
      GetTimeout(evaluation_start_wallclock_, reevaluation_time_wallclock_),
      GetTimeout(evaluation_start_monotonic_, reevaluation_time_monotonic_));

  // Handle reevaluation due to async or poll variables.
  bool waiting_for_value_change = false;
  for (auto& it : value_cache_) {
    switch (it.first->GetMode()) {
      case kVariableModeAsync:
        DLOG(INFO) << "Waiting for value on " << it.first->GetName();
        it.first->AddObserver(this);
        waiting_for_value_change = true;
        break;
      case kVariableModePoll:
        timeout = std::min(timeout, it.first->GetPollInterval());
        break;
      case kVariableModeConst:
        // Ignored.
        break;
    }
  }

  // Check if the re-evaluation is actually being scheduled. If there are no
  // events waited for, this function should return false.
  if (!waiting_for_value_change && timeout.is_max())
    return false;

  // Ensure that we take into account the expiration timeout.
  TimeDelta expiration = RemainingTime(expiration_monotonic_deadline_);
  timeout_marks_expiration_ = expiration < timeout;
  if (timeout_marks_expiration_)
    timeout = expiration;

  // Store the reevaluation callback.
  callback_.reset(new Closure(callback));

  // Schedule a timeout event, if one is set.
  if (!timeout.is_max()) {
    DLOG(INFO) << "Waiting for timeout in "
               << chromeos_update_engine::utils::FormatTimeDelta(timeout);
    timeout_event_ = RunFromMainLoopAfterTimeout(
        base::Bind(&EvaluationContext::OnTimeout,
                   weak_ptr_factory_.GetWeakPtr()),
        timeout);
  }

  return true;
}

string EvaluationContext::DumpContext() const {
  base::DictionaryValue* variables = new base::DictionaryValue();
  for (auto& it : value_cache_) {
    variables->SetString(it.first->GetName(), it.second.ToString());
  }

  base::DictionaryValue value;
  value.Set("variables", variables);  // Adopts |variables|.
  value.SetString(
      "evaluation_start_wallclock",
      chromeos_update_engine::utils::ToString(evaluation_start_wallclock_));
  value.SetString(
      "evaluation_start_monotonic",
      chromeos_update_engine::utils::ToString(evaluation_start_monotonic_));

  string json_str;
  base::JSONWriter::WriteWithOptions(&value,
                                     base::JSONWriter::OPTIONS_PRETTY_PRINT,
                                     &json_str);
  base::TrimWhitespaceASCII(json_str, base::TRIM_TRAILING, &json_str);

  return json_str;
}

}  // namespace chromeos_update_manager
