// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stable application identity and process grouping.
//
// Requirements this file implements:
//  * Rules must survive restarts, so they can not be keyed by PID.
//  * Multiple processes of one application (Chrome!) must become one entry.
//  * Different applications that happen to have the same display name must NOT
//    be merged: the key is the executable path, not the display name.
//  * When several processes of one application each have audio sessions, only
//    the process tree roots may be captured, otherwise the same audio would be
//    mixed in twice.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "capture.h"

namespace saf {

/** Lowercases and normalises a Windows path for use in a stable key. */
std::string normalisePath(const std::string& path);

/**
 * Builds the stable application key.
 *  * UWP/Store apps: "uwp:<package family name>"
 *  * "System Sounds": "system:sounds"
 *  * everything else: "exe:<lowercased full path>"
 *  * if no path is available: "pid:<pid>" (flagged ephemeral by the caller)
 */
std::string makeAppKey(const std::string& exePath, const std::string& exeName, const std::string& aumid, bool isSystemSounds, uint32_t pid);

/** True for keys that can not survive a restart. */
bool isEphemeralKey(const std::string& appKey);

/**
 * Picks the process tree roots of one application.
 *
 * @param pids     all processes of the application
 * @param tree     the whole process table
 * @returns the subset of @p pids that are not a descendant of another entry of
 *          @p pids. Deterministically ordered by ascending pid.
 */
std::vector<uint32_t> computeCaptureRoots(const std::vector<uint32_t>& pids, const std::vector<ProcessNode>& tree);

/** Groups sessions of the same application key. */
struct AppGroup {
    std::string appKey;
    std::string displayName;
    std::string exeName;
    std::string exePath;
    std::vector<uint32_t> pids;
    std::vector<uint32_t> roots;
    bool isSystemSounds = false;
    bool isUwp = false;
    bool capturable = true;
    std::string notCapturableReason;
    bool playing = false;
    float peak = -1.0f;
    std::vector<std::string> sessionIds;
};

/**
 * Merges sessions into one entry per application.
 *
 * Merging happens only when the executable path (or the UWP identity) is
 * identical, never based on the display name.
 */
std::vector<AppGroup> groupSessions(const std::vector<SessionInfo>& sessions, const std::vector<ProcessNode>& tree);

} // namespace saf
