#pragma once

#include <string>

namespace pf {

// Native injection into the input host needs an elevated core, but autostart
// (the Run key) and the settings app start it unelevated. The first time the
// user approves UAC, the elevated core registers an on-demand task (no
// triggers) that runs it with highest privileges; from then on unelevated
// starts hand over to that task, so elevation survives a reboot.

bool IsElevated();

// Elevated only: creates or updates the task to run `exe`.
void RegisterElevatedTask(const std::wstring& exe);

// True when the task exists, still runs `exe`, and was started.
bool StartElevatedTask(const std::wstring& exe);

// False when the task exists but this (unelevated) process may not delete it.
bool RemoveElevatedTask();

}  // namespace pf
