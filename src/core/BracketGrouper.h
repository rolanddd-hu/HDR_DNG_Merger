#pragma once
#include "Types.h"
#include "FileScanner.h"
#include <vector>
#include <string>

namespace hdr {

struct BracketGroup {
    int id = 0;
    std::string name; // e.g. "Bracket 001"
    std::vector<FileEntry> files;
    int referenceIndex = -1; // -1 for auto (median exposure)
    
    enum class Status { Ok, Warning, Error } status = Status::Ok;
    std::string statusMessage;
    
    double timeSpan = 0.0;
    double evSpan = 0.0;

    int getResolvedReferenceIndex() const;
};

struct GroupingResult {
    std::vector<BracketGroup> groups;
    std::vector<FileEntry> ungrouped;
    std::string warning;
};

class BracketGrouper {
public:
    // Group sorted files into consecutive chunks of N
    static GroupingResult groupFixedN(const std::vector<FileEntry>& files, int n, double maxTimeSpanSec = 15.0);

    // Auto-detect bracket size N (from 2 to 9) based on EV repetition and time gaps
    static int autoDetectBracketSize(const std::vector<FileEntry>& files);

    // Validate a group's consistency
    static void validateGroup(BracketGroup& group, double maxTimeSpanSec = 15.0);
};

} // namespace hdr
