// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app_identity.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace saf {

std::string normalisePath(const std::string& path) {
    std::string result;
    result.reserve(path.size());
    for (char c : path) {
        if (c == '/') c = '\\';
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    // Strip a trailing separator, if any.
    while (!result.empty() && result.back() == '\\') result.pop_back();
    return result;
}

std::string makeAppKey(const std::string& exePath, const std::string& exeName, const std::string& aumid, bool isSystemSounds, uint32_t pid) {
    if (isSystemSounds) return "system:sounds";

    const std::string normalised = normalisePath(exePath);
    if (!normalised.empty()) {
        // A UWP app executed as WWAHost/ApplicationFrameHost is identified by
        // its application user model id, not by the hosting executable.
        const std::string loweredName = normalisePath(exeName);
        const bool hostedUwp = !aumid.empty()
            && (loweredName.find("wwahost.exe") != std::string::npos
                || loweredName.find("applicationframehost.exe") != std::string::npos
                || loweredName.find("winstore.app.exe") != std::string::npos);
        if (hostedUwp) return "uwp:" + aumid;
        return "exe:" + normalised;
    }

    if (!aumid.empty()) return "uwp:" + aumid;
    if (pid != 0) return "pid:" + std::to_string(pid);
    return "unknown";
}

bool isEphemeralKey(const std::string& appKey) {
    return appKey.rfind("pid:", 0) == 0 || appKey == "unknown";
}

std::vector<uint32_t> computeCaptureRoots(const std::vector<uint32_t>& pids, const std::vector<ProcessNode>& tree) {
    std::map<uint32_t, uint32_t> parents;
    for (const auto& node : tree) parents[node.pid] = node.parentPid;

    std::set<uint32_t> members(pids.begin(), pids.end());
    std::vector<uint32_t> roots;

    for (uint32_t pid : members) {
        bool hasMemberAncestor = false;
        std::set<uint32_t> visited;
        uint32_t current = pid;
        // Walk up at most as many steps as there are processes to avoid loops.
        for (size_t guard = 0; guard <= parents.size(); guard++) {
            const auto it = parents.find(current);
            if (it == parents.end() || it->second == 0 || it->second == current) break;
            if (!visited.insert(it->second).second) break;  // cycle
            if (members.count(it->second)) {
                hasMemberAncestor = true;
                break;
            }
            current = it->second;
        }
        if (!hasMemberAncestor) roots.push_back(pid);
    }

    std::sort(roots.begin(), roots.end());
    return roots;
}

std::vector<AppGroup> groupSessions(const std::vector<SessionInfo>& sessions, const std::vector<ProcessNode>& tree) {
    std::map<std::string, AppGroup> groups;

    for (const auto& session : sessions) {
        const std::string key = session.appKey.empty()
            ? makeAppKey(session.exePath, session.exeName, {}, session.isSystemSounds, session.pid)
            : session.appKey;

        AppGroup& group = groups[key];
        if (group.appKey.empty()) {
            group.appKey = key;
            group.displayName = session.displayName;
            group.exeName = session.exeName;
            group.exePath = session.exePath;
            group.isSystemSounds = session.isSystemSounds;
            group.isUwp = session.isUwp;
        }

        if (std::find(group.pids.begin(), group.pids.end(), session.pid) == group.pids.end()) {
            group.pids.push_back(session.pid);
        }
        group.sessionIds.push_back(session.sessionId);

        // A group is playing when at least one of its sessions is active.
        group.playing = group.playing || session.playing;
        if (session.peak > group.peak) group.peak = session.peak;

        // Not capturable wins over capturable: one broken session must be able
        // to hide the whole entry from the filter list.
        if (!session.capturable) {
            group.capturable = false;
            group.notCapturableReason = session.notCapturableReason;
        }

        // Prefer a nicer display name when the first session only had a file name.
        if (group.displayName.empty()) group.displayName = session.displayName;
        if (group.exeName.empty()) group.exeName = session.exeName;
        if (group.exePath.empty()) group.exePath = session.exePath;
    }

    std::vector<AppGroup> result;
    result.reserve(groups.size());
    for (auto& [key, group] : groups) {
        if (group.displayName.empty()) group.displayName = group.exeName.empty() ? key : group.exeName;
        group.roots = computeCaptureRoots(group.pids, tree);
        result.push_back(std::move(group));
    }

    // Stable order: playing first, then by display name.
    std::sort(result.begin(), result.end(), [](const AppGroup& a, const AppGroup& b) {
        if (a.playing != b.playing) return a.playing > b.playing;
        return a.displayName < b.displayName;
    });

    return result;
}

namespace {

using ParentMap = std::map<uint32_t, uint32_t>;

ParentMap parentMap(const std::vector<ProcessNode>& tree) {
    ParentMap parents;
    for (const auto& node : tree) parents[node.pid] = node.parentPid;
    return parents;
}

bool inTree(uint32_t pid, uint32_t ancestor, const ParentMap& parents) {
    if (pid == 0 || ancestor == 0) return false;
    if (pid == ancestor) return true;
    std::set<uint32_t> visited;
    uint32_t current = pid;
    for (size_t guard = 0; guard <= parents.size(); guard++) {
        const auto it = parents.find(current);
        if (it == parents.end() || it->second == 0 || it->second == current) return false;
        if (it->second == ancestor) return true;
        if (!visited.insert(it->second).second) return false;  // cycle
        current = it->second;
    }
    return false;
}

/** Strictly below: a root never "covers" itself. */
bool strictlyBelow(uint32_t pid, uint32_t ancestor, const ParentMap& parents) {
    return pid != ancestor && inTree(pid, ancestor, parents);
}

} // namespace

bool isInProcessTree(uint32_t pid, uint32_t ancestor, const std::vector<ProcessNode>& tree) {
    return inTree(pid, ancestor, parentMap(tree));
}

void protectProcessTrees(std::vector<AppGroup>& groups, const std::vector<ProcessNode>& tree,
    const std::vector<uint32_t>& protectedRoots, const std::string& reason) {
    if (protectedRoots.empty()) return;
    const ParentMap parents = parentMap(tree);
    for (auto& group : groups) {
        const bool inside = std::any_of(group.pids.begin(), group.pids.end(), [&](uint32_t pid) {
            return std::any_of(protectedRoots.begin(), protectedRoots.end(), [&](uint32_t root) { return inTree(pid, root, parents); });
        });
        if (!inside) continue;
        group.capturable = false;
        group.notCapturableReason = reason;
    }
}

std::map<std::string, CapturePlan> planCaptures(const std::vector<AppGroup>& groups, const std::vector<ProcessNode>& tree,
    const std::function<bool(const std::string&)>& transmit) {
    const ParentMap parents = parentMap(tree);

    auto wanted = [&](const AppGroup& group) {
        return group.capturable && !group.roots.empty() && transmit(group.appKey);
    };

    std::map<std::string, CapturePlan> plans;

    // 1. Block every transmitted application that has a process of a
    //    non transmitted one inside one of its trees.
    for (const auto& group : groups) {
        if (!wanted(group)) continue;
        CapturePlan& plan = plans[group.appKey];
        for (const auto& other : groups) {
            if (&other == &group || wanted(other)) continue;
            for (uint32_t root : group.roots) {
                const bool leaks = std::any_of(other.pids.begin(), other.pids.end(), [&](uint32_t pid) {
                    return strictlyBelow(pid, root, parents);
                });
                if (!leaks) continue;
                plan.blockedReason = other.displayName + " runs inside the process tree of " + group.displayName
                    + " and is not transmitted. Windows can only capture whole process trees, so " + group.displayName
                    + " is not sent either: otherwise the audio of " + other.displayName + " would reach the viewers.";
                break;
            }
            if (!plan.blockedReason.empty()) break;
        }
    }

    // 2. Drop roots that sit below the root of another (unblocked) capture.
    struct RootOwner {
        uint32_t pid;
        const AppGroup* group;
    };
    std::vector<RootOwner> active;
    for (const auto& group : groups) {
        if (!wanted(group) || !plans[group.appKey].blockedReason.empty()) continue;
        for (uint32_t root : group.roots) active.push_back({ root, &group });
    }

    for (const auto& group : groups) {
        if (!wanted(group)) continue;
        CapturePlan& plan = plans[group.appKey];
        if (!plan.blockedReason.empty()) continue;

        for (uint32_t root : group.roots) {
            const RootOwner* cover = nullptr;
            for (const auto& owner : active) {
                if (owner.group == &group) continue;  // same app: computeCaptureRoots already merged
                if (strictlyBelow(root, owner.pid, parents)) {
                    cover = &owner;
                    break;
                }
            }
            if (cover) {
                if (plan.coveredBy.empty()) plan.coveredBy = cover->group->displayName;
            } else {
                plan.roots.push_back(root);
            }
        }
        // Partially covered applications keep their own capture for the rest:
        // only report "covered" when nothing of their own is captured.
        if (!plan.roots.empty()) plan.coveredBy.clear();
    }

    return plans;
}

} // namespace saf
