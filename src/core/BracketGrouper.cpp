#include "BracketGrouper.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace hdr {

int BracketGroup::getResolvedReferenceIndex() const {
    if (referenceIndex >= 0 && referenceIndex < static_cast<int>(files.size()) && files[referenceIndex].enabled) {
        return referenceIndex;
    }
    if (files.empty()) return 0;

    std::vector<std::pair<double, int>> expList;
    for (int i = 0; i < static_cast<int>(files.size()); ++i) {
        if (files[i].enabled) {
            expList.emplace_back(files[i].info.relativeExposure(), i);
        }
    }
    if (expList.empty()) {
        return 0;
    }
    std::sort(expList.begin(), expList.end());
    return expList[expList.size() / 2].second;
}

void BracketGrouper::validateGroup(BracketGroup& group, double maxTimeSpanSec) {
    if (group.files.empty()) {
        group.status = BracketGroup::Status::Error;
        group.statusMessage = "Empty bracket group";
        return;
    }

    for (const auto& f : group.files) {
        if (f.enabled && !f.status) {
            group.status = BracketGroup::Status::Error;
            group.statusMessage = f.status.message;
            return;
        }
    }

    if (group.files.size() == 1) {
        group.status = BracketGroup::Status::Warning;
        group.statusMessage = "Group contains only 1 frame";
        return;
    }

    const auto& first = group.files[0].info;
    double minExp = first.relativeExposure();
    double maxExp = minExp;

    double tStart = first.timestamp;
    double tEnd = tStart;

    bool sameCamera = true;
    for (size_t i = 0; i < group.files.size(); ++i) {
        const auto& inf = group.files[i].info;
        if (inf.make != first.make || inf.model != first.model ||
            inf.width != first.width || inf.height != first.height) {
            sameCamera = false;
        }

        double e = inf.relativeExposure();
        if (e < minExp) minExp = e;
        if (e > maxExp) maxExp = e;

        if (inf.timestamp > 0.0) {
            if (tStart <= 0.0 || inf.timestamp < tStart) tStart = inf.timestamp;
            if (inf.timestamp > tEnd) tEnd = inf.timestamp;
        }
    }

    if (!sameCamera) {
        group.status = BracketGroup::Status::Error;
        group.statusMessage = "Mismatched camera or image dimensions";
        return;
    }

    group.timeSpan = (tStart > 0.0 && tEnd >= tStart) ? (tEnd - tStart) : 0.0;
    group.evSpan = (minExp > 0.0 && maxExp >= minExp) ? std::log2(maxExp / minExp) : 0.0;

    if (group.timeSpan > maxTimeSpanSec) {
        group.status = BracketGroup::Status::Warning;
        group.statusMessage = "Time span exceeds limit (" + std::to_string(static_cast<int>(group.timeSpan)) + "s)";
        return;
    }

    if (group.evSpan < 0.5) {
        group.status = BracketGroup::Status::Warning;
        group.statusMessage = "Low exposure variation (< 0.5 EV)";
        return;
    }

    group.status = BracketGroup::Status::Ok;
    group.statusMessage = "Ok";
}

GroupingResult BracketGrouper::groupFixedN(const std::vector<FileEntry>& files, int n, double maxTimeSpanSec) {
    GroupingResult result;
    if (n <= 0 || files.empty()) {
        result.ungrouped = files;
        return result;
    }

    size_t numFullGroups = files.size() / n;
    result.groups.reserve(numFullGroups);

    for (size_t g = 0; g < numFullGroups; ++g) {
        BracketGroup group;
        group.id = static_cast<int>(g + 1);
        std::ostringstream ss;
        ss << "Bracket " << std::setfill('0') << std::setw(3) << group.id;
        group.name = ss.str();

        for (int i = 0; i < n; ++i) {
            group.files.push_back(files[g * n + i]);
        }

        validateGroup(group, maxTimeSpanSec);
        result.groups.push_back(std::move(group));
    }

    size_t remainder = files.size() % n;
    if (remainder > 0) {
        for (size_t i = numFullGroups * n; i < files.size(); ++i) {
            result.ungrouped.push_back(files[i]);
        }
        result.warning = std::to_string(remainder) + " file(s) leftover and not grouped (expected groups of " + std::to_string(n) + ")";
    }

    return result;
}

int BracketGrouper::autoDetectBracketSize(const std::vector<FileEntry>& files) {
    if (files.size() < 2) return 3;

    int bestN = 3;
    int bestScore = -1;

    for (int n = 2; n <= 9; ++n) {
        if (files.size() < static_cast<size_t>(n)) continue;

        int score = 0;
        size_t numGroups = files.size() / n;
        for (size_t g = 0; g < numGroups; ++g) {
            double t0 = files[g * n].info.timestamp;
            double t1 = files[g * n + n - 1].info.timestamp;
            if (t0 > 0 && t1 >= t0 && (t1 - t0) <= 12.0) {
                score += 10;
            }
            // Check time gap between groups
            if (g + 1 < numGroups) {
                double nextT0 = files[(g + 1) * n].info.timestamp;
                if (nextT0 > t1 && (nextT0 - t1) > 2.0) {
                    score += 5;
                }
            }
        }

        if (files.size() % n == 0) {
            score += 15;
        }

        if (score > bestScore) {
            bestScore = score;
            bestN = n;
        }
    }

    return bestN;
}

} // namespace hdr
