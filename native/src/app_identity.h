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
#include <functional>
#include <map>
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

/**
 * True when @p pid is @p ancestor or lies anywhere below it in the process tree.
 * Walks parent links with a cycle guard; an unknown pid is not a descendant.
 */
bool isInProcessTree(uint32_t pid, uint32_t ancestor, const std::vector<ProcessNode>& tree);

/**
 * Marks every group with a process inside one of @p protectedRoots (Discord's
 * own process tree) as not capturable. Discord's voice chat and notification
 * sounds must never reach the share, whatever the rules say — and this must
 * not depend on an executable path, which changes with every Discord update.
 */
void protectProcessTrees(std::vector<AppGroup>& groups, const std::vector<ProcessNode>& tree,
    const std::vector<uint32_t>& protectedRoots, const std::string& reason);

/**
 * What one transmitted application really needs to be captured.
 *
 * Windows can only capture whole process trees. That has two consequences
 * across *different* applications, both handled here:
 *  * an excluded application running inside the tree of a transmitted one
 *    (a game started by a launcher) can not be separated from it. Capturing
 *    the launcher would leak the game, so the launcher is blocked (fail closed)
 *    and the reason is reported;
 *  * a transmitted application inside the tree of another transmitted one is
 *    already in that capture. Capturing it again would mix it in twice, so its
 *    covered roots are dropped (coveredBy names the capture it is part of).
 */
struct CapturePlan {
    /** Roots that need their own capture; empty when blocked or covered. */
    std::vector<uint32_t> roots;
    /** Non empty: this application must not be captured, and why. */
    std::string blockedReason;
    /** Non empty: every root is already captured through this application. */
    std::string coveredBy;
};

/**
 * @param transmit the effective rule for an app key (mode + overrides + gate)
 * @returns one entry per transmitted, capturable group with at least one root
 */
std::map<std::string, CapturePlan> planCaptures(const std::vector<AppGroup>& groups, const std::vector<ProcessNode>& tree,
    const std::function<bool(const std::string&)>& transmit);

} // namespace saf
