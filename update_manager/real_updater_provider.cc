// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "update_engine/update_manager/real_updater_provider.h"

#include <inttypes.h>

#include <string>

#include <base/bind.h>
#include <base/strings/stringprintf.h>
#include <base/time/time.h>
#include <chromeos/dbus/service_constants.h>

#include "update_engine/clock_interface.h"
#include "update_engine/omaha_request_params.h"
#include "update_engine/prefs.h"
#include "update_engine/update_attempter.h"

using base::StringPrintf;
using base::Time;
using base::TimeDelta;
using chromeos_update_engine::OmahaRequestParams;
using chromeos_update_engine::SystemState;
using std::string;

namespace chromeos_update_manager {

// A templated base class for all update related variables. Provides uniform
// construction and a system state handle.
template<typename T>
class UpdaterVariableBase : public Variable<T> {
 public:
  UpdaterVariableBase(const string& name, VariableMode mode,
                      SystemState* system_state)
      : Variable<T>(name, mode), system_state_(system_state) {}

 protected:
  // The system state used for pulling information from the updater.
  inline SystemState* system_state() const { return system_state_; }

 private:
  SystemState* const system_state_;
};

// Helper class for issuing a GetStatus() to the UpdateAttempter.
class GetStatusHelper {
 public:
  GetStatusHelper(SystemState* system_state, string* errmsg) {
    is_success_ = system_state->update_attempter()->GetStatus(
        &last_checked_time_, &progress_, &update_status_, &new_version_,
        &payload_size_);
    if (!is_success_ && errmsg)
      *errmsg = "Failed to get a status update from the update engine";
  }

  inline bool is_success() { return is_success_; }
  inline int64_t last_checked_time() { return last_checked_time_; }
  inline double progress() { return progress_; }
  inline const string& update_status() { return update_status_; }
  inline const string& new_version() { return new_version_; }
  inline int64_t payload_size() { return payload_size_; }

 private:
  bool is_success_;
  int64_t last_checked_time_;
  double progress_;
  string update_status_;
  string new_version_;
  int64_t payload_size_;
};

// A variable reporting the time when a last update check was issued.
class LastCheckedTimeVariable : public UpdaterVariableBase<Time> {
 public:
  LastCheckedTimeVariable(const string& name, SystemState* system_state)
      : UpdaterVariableBase<Time>(name, kVariableModePoll, system_state) {}

 private:
  const Time* GetValue(TimeDelta /* timeout */, string* errmsg) override {
    GetStatusHelper raw(system_state(), errmsg);
    if (!raw.is_success())
      return nullptr;

    return new Time(Time::FromTimeT(raw.last_checked_time()));
  }

  DISALLOW_COPY_AND_ASSIGN(LastCheckedTimeVariable);
};

// A variable reporting the update (download) progress as a decimal fraction
// between 0.0 and 1.0.
class ProgressVariable : public UpdaterVariableBase<double> {
 public:
  ProgressVariable(const string& name, SystemState* system_state)
      : UpdaterVariableBase<double>(name, kVariableModePoll, system_state) {}

 private:
  const double* GetValue(TimeDelta /* timeout */, string* errmsg) override {
    GetStatusHelper raw(system_state(), errmsg);
    if (!raw.is_success())
      return nullptr;

    if (raw.progress() < 0.0 || raw.progress() > 1.0) {
      if (errmsg) {
        *errmsg = StringPrintf("Invalid progress value received: %f",
                               raw.progress());
      }
      return nullptr;
    }

    return new double(raw.progress());
  }

  DISALLOW_COPY_AND_ASSIGN(ProgressVariable);
};

// A variable reporting the stage in which the update process is.
class StageVariable : public UpdaterVariableBase<Stage> {
 public:
  StageVariable(const string& name, SystemState* system_state)
      : UpdaterVariableBase<Stage>(name, kVariableModePoll, system_state) {}

 private:
  struct CurrOpStrToStage {
    const char* str;
    Stage stage;
  };
  static const CurrOpStrToStage curr_op_str_to_stage[];

  // Note: the method is defined outside the class so arraysize can work.
  const Stage* GetValue(TimeDelta /* timeout */, string* errmsg) override;

  DISALLOW_COPY_AND_ASSIGN(StageVariable);
};

const StageVariable::CurrOpStrToStage StageVariable::curr_op_str_to_stage[] = {
  {update_engine::kUpdateStatusIdle, Stage::kIdle},
  {update_engine::kUpdateStatusCheckingForUpdate, Stage::kCheckingForUpdate},
  {update_engine::kUpdateStatusUpdateAvailable, Stage::kUpdateAvailable},
  {update_engine::kUpdateStatusDownloading, Stage::kDownloading},
  {update_engine::kUpdateStatusVerifying, Stage::kVerifying},
  {update_engine::kUpdateStatusFinalizing, Stage::kFinalizing},
  {update_engine::kUpdateStatusUpdatedNeedReboot, Stage::kUpdatedNeedReboot},
  {  // NOLINT(whitespace/braces)
    update_engine::kUpdateStatusReportingErrorEvent,
    Stage::kReportingErrorEvent
  },
  {update_engine::kUpdateStatusAttemptingRollback, Stage::kAttemptingRollback},
};

const Stage* StageVariable::GetValue(TimeDelta /* timeout */,
                                     string* errmsg) {
  GetStatusHelper raw(system_state(), errmsg);
  if (!raw.is_success())
    return nullptr;

  for (auto& key_val : curr_op_str_to_stage)
    if (raw.update_status() == key_val.str)
      return new Stage(key_val.stage);

  if (errmsg)
    *errmsg = string("Unknown update status: ") + raw.update_status();
  return nullptr;
}

// A variable reporting the version number that an update is updating to.
class NewVersionVariable : public UpdaterVariableBase<string> {
 public:
  NewVersionVariable(const string& name, SystemState* system_state)
      : UpdaterVariableBase<string>(name, kVariableModePoll, system_state) {}

 private:
  const string* GetValue(TimeDelta /* timeout */, string* errmsg) override {
    GetStatusHelper raw(system_state(), errmsg);
    if (!raw.is_success())
      return nullptr;

    return new string(raw.new_version());
  }

  DISALLOW_COPY_AND_ASSIGN(NewVersionVariable);
};

// A variable reporting the size of the update being processed in bytes.
class PayloadSizeVariable : public UpdaterVariableBase<int64_t> {
 public:
  PayloadSizeVariable(const string& name, SystemState* system_state)
      : UpdaterVariableBase<int64_t>(name, kVariableModePoll, system_state) {}

 private:
  const int64_t* GetValue(TimeDelta /* timeout */, string* errmsg) override {
    GetStatusHelper raw(system_state(), errmsg);
    if (!raw.is_success())
      return nullptr;

    if (raw.payload_size() < 0) {
      if (errmsg)
        *errmsg = string("Invalid payload size: %" PRId64, raw.payload_size());
      return nullptr;
    }

    return new int64_t(raw.payload_size());
  }

  DISALLOW_COPY_AND_ASSIGN(PayloadSizeVariable);
};

// A variable reporting the point in time an update last completed in the
// current boot cycle.
//
// TODO(garnold) In general, both the current boottime and wallclock time
// readings should come from the time provider and be moderated by the
// evaluation context, so that they are uniform throughout the evaluation of a
// policy request.
class UpdateCompletedTimeVariable : public UpdaterVariableBase<Time> {
 public:
  UpdateCompletedTimeVariable(const string& name, SystemState* system_state)
      : UpdaterVariableBase<Time>(name, kVariableModePoll, system_state) {}

 private:
  const Time* GetValue(TimeDelta /* timeout */, string* errmsg) override {
    Time update_boottime;
    if (!system_state()->update_attempter()->GetBootTimeAtUpdate(
            &update_boottime)) {
      if (errmsg)
        *errmsg = "Update completed time could not be read";
      return nullptr;
    }

    chromeos_update_engine::ClockInterface* clock = system_state()->clock();
    Time curr_boottime = clock->GetBootTime();
    if (curr_boottime < update_boottime) {
      if (errmsg)
        *errmsg = "Update completed time more recent than current time";
      return nullptr;
    }
    TimeDelta duration_since_update = curr_boottime - update_boottime;
    return new Time(clock->GetWallclockTime() - duration_since_update);
  }

  DISALLOW_COPY_AND_ASSIGN(UpdateCompletedTimeVariable);
};

// Variables reporting the current image channel.
class CurrChannelVariable : public UpdaterVariableBase<string> {
 public:
  CurrChannelVariable(const string& name, SystemState* system_state)
      : UpdaterVariableBase<string>(name, kVariableModePoll, system_state) {}

 private:
  const string* GetValue(TimeDelta /* timeout */, string* errmsg) override {
    OmahaRequestParams* request_params = system_state()->request_params();
    string channel = request_params->current_channel();
    if (channel.empty()) {
      if (errmsg)
        *errmsg = "No current channel";
      return nullptr;
    }
    return new string(channel);
  }

  DISALLOW_COPY_AND_ASSIGN(CurrChannelVariable);
};

// Variables reporting the new image channel.
class NewChannelVariable : public UpdaterVariableBase<string> {
 public:
  NewChannelVariable(const string& name, SystemState* system_state)
      : UpdaterVariableBase<string>(name, kVariableModePoll, system_state) {}

 private:
  const string* GetValue(TimeDelta /* timeout */, string* errmsg) override {
    OmahaRequestParams* request_params = system_state()->request_params();
    string channel = request_params->target_channel();
    if (channel.empty()) {
      if (errmsg)
        *errmsg = "No new channel";
      return nullptr;
    }
    return new string(channel);
  }

  DISALLOW_COPY_AND_ASSIGN(NewChannelVariable);
};

// A variable class for reading Boolean prefs values.
class BooleanPrefVariable : public UpdaterVariableBase<bool> {
 public:
  BooleanPrefVariable(const string& name, SystemState* system_state,
                      const char* key, bool default_val)
      : UpdaterVariableBase<bool>(name, kVariableModePoll, system_state),
        key_(key), default_val_(default_val) {}

 private:
  const bool* GetValue(TimeDelta /* timeout */, string* errmsg) override {
    bool result = default_val_;
    chromeos_update_engine::PrefsInterface* prefs = system_state()->prefs();
    if (prefs && prefs->Exists(key_) && !prefs->GetBoolean(key_, &result)) {
      if (errmsg)
        *errmsg = string("Could not read boolean pref ") + key_;
      return nullptr;
    }
    return new bool(result);
  }

  // The Boolean preference key and default value.
  const char* const key_;
  const bool default_val_;

  DISALLOW_COPY_AND_ASSIGN(BooleanPrefVariable);
};

// A variable returning the number of consecutive failed update checks.
class ConsecutiveFailedUpdateChecksVariable
    : public UpdaterVariableBase<unsigned int> {
 public:
  ConsecutiveFailedUpdateChecksVariable(const string& name,
                                        SystemState* system_state)
      : UpdaterVariableBase<unsigned int>(name, kVariableModePoll,
                                          system_state) {}

 private:
  const unsigned int* GetValue(TimeDelta /* timeout */,
                               string* /* errmsg */) override {
    return new unsigned int(
        system_state()->update_attempter()->consecutive_failed_update_checks());
  }

  DISALLOW_COPY_AND_ASSIGN(ConsecutiveFailedUpdateChecksVariable);
};

// A variable returning the server-dictated poll interval.
class ServerDictatedPollIntervalVariable
    : public UpdaterVariableBase<unsigned int> {
 public:
  ServerDictatedPollIntervalVariable(const string& name,
                                     SystemState* system_state)
      : UpdaterVariableBase<unsigned int>(name, kVariableModePoll,
                                          system_state) {}

 private:
  const unsigned int* GetValue(TimeDelta /* timeout */,
                               string* /* errmsg */) override {
    return new unsigned int(
        system_state()->update_attempter()->server_dictated_poll_interval());
  }

  DISALLOW_COPY_AND_ASSIGN(ServerDictatedPollIntervalVariable);
};

// An async variable that tracks changes to interactive update requests.
class InteractiveUpdateRequestedVariable : public UpdaterVariableBase<bool> {
 public:
  InteractiveUpdateRequestedVariable(const string& name,
                                     SystemState* system_state)
      : UpdaterVariableBase<bool>::UpdaterVariableBase(name, kVariableModeAsync,
                                                       system_state) {
    system_state->update_attempter()->set_interactive_update_pending_callback(
        new base::Callback<void(bool)>(  // NOLINT(readability/function)
            base::Bind(&InteractiveUpdateRequestedVariable::Reset,
                       base::Unretained(this))));
  }

 private:
  const bool* GetValue(TimeDelta /* timeout */,
                       string* /* errmsg */) override {
    return new bool(interactive_update_requested_);
  }

  void Reset(bool value) {
    if (interactive_update_requested_ != value) {
      interactive_update_requested_ = value;
      NotifyValueChanged();
    }
  }

  bool interactive_update_requested_ = false;

  DISALLOW_COPY_AND_ASSIGN(InteractiveUpdateRequestedVariable);
};

// RealUpdaterProvider methods.

RealUpdaterProvider::RealUpdaterProvider(SystemState* system_state)
  : system_state_(system_state),
    var_updater_started_time_("updater_started_time",
                              system_state->clock()->GetWallclockTime()),
    var_last_checked_time_(
        new LastCheckedTimeVariable("last_checked_time", system_state_)),
    var_update_completed_time_(
        new UpdateCompletedTimeVariable("update_completed_time",
                                        system_state_)),
    var_progress_(new ProgressVariable("progress", system_state_)),
    var_stage_(new StageVariable("stage", system_state_)),
    var_new_version_(new NewVersionVariable("new_version", system_state_)),
    var_payload_size_(new PayloadSizeVariable("payload_size", system_state_)),
    var_curr_channel_(new CurrChannelVariable("curr_channel", system_state_)),
    var_new_channel_(new NewChannelVariable("new_channel", system_state_)),
    var_p2p_enabled_(
        new BooleanPrefVariable("p2p_enabled", system_state_,
                                chromeos_update_engine::kPrefsP2PEnabled,
                                false)),
    var_cellular_enabled_(
        new BooleanPrefVariable(
            "cellular_enabled", system_state_,
            chromeos_update_engine::kPrefsUpdateOverCellularPermission,
            false)),
    var_consecutive_failed_update_checks_(
        new ConsecutiveFailedUpdateChecksVariable(
            "consecutive_failed_update_checks", system_state_)),
    var_server_dictated_poll_interval_(
        new ServerDictatedPollIntervalVariable(
            "server_dictated_poll_interval", system_state_)),
    var_interactive_update_requested_(
        new InteractiveUpdateRequestedVariable(
            "interactive_update_requested", system_state_)) {}

}  // namespace chromeos_update_manager
