#include "DiffCaptureStatus.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <ctime>

// ---------------------------------------------------------------------------
//  DiffCaptureStatus — standalone rank-file update status tracker
// ---------------------------------------------------------------------------

DiffCaptureStatus& GetDiffCaptureStatus() {
    static DiffCaptureStatus instance;
    return instance;
}

DiffCaptureStatus::DiffCaptureStatus() = default;

std::string DiffCaptureStatus::CategorizeFile(const std::string& filename) {
    if (filename.find(".material.") != std::string::npos ||
        filename.find("_material.") != std::string::npos ||
        filename.find("materialdiff") != std::string::npos ||
        filename.find("material_param") != std::string::npos ||
        filename.find(".param.") != std::string::npos ||
        filename.find(".png") != std::string::npos ||
        filename.find(".jpg") != std::string::npos ||
        filename.find(".jpeg") != std::string::npos) {
        return "texture";
    }
    if (filename.find("clip") != std::string::npos ||
        filename.find("Clip") != std::string::npos ||
        filename.find("_anim.") != std::string::npos) {
        return "anim";
    }
    if (filename.find(".usda") != std::string::npos) {
        return "geom";
    }
    return "other";
}

uint64_t DiffCaptureStatus::NowSec() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

void DiffCaptureStatus::RegisterRank(int rank, const std::string& hostname) {
    std::lock_guard<std::mutex> lock(mutex_);
    rankHosts_[rank] = hostname;
}

void DiffCaptureStatus::OnNotification(int sourceRank, const char* filename, uint64_t timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);

    RankFileUpdate upd;
    upd.filename = filename;
    upd.category = CategorizeFile(filename);
    upd.timestamp = timestamp;
    upd.rank = sourceRank;

    lastUpdate_[sourceRank] = upd;

    history_.push_back(upd);
    if (history_.size() > 500) {
        history_.erase(history_.begin());
    }
}

void DiffCaptureStatus::Disable() {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = false;
}

size_t DiffCaptureStatus::ActiveRankCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastUpdate_.size();
}

size_t DiffCaptureStatus::UpdatesInLastWindow(uint64_t windowSec) const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t now = NowSec();
    size_t count = 0;
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        if (now - it->timestamp > windowSec) break;
        ++count;
    }
    return count;
}

// ---------------------------------------------------------------------------
//  ANSI helpers
// ---------------------------------------------------------------------------
static const char* ANSI_CLEAR  = "\033[2J\033[H";
static const char* ANSI_CYAN   = "\033[96m";
static const char* ANSI_GREEN  = "\033[92m";
static const char* ANSI_YELLOW = "\033[93m";
static const char* ANSI_RED    = "\033[91m";
static const char* ANSI_BRIGHT = "\033[1m";
static const char* ANSI_RESET  = "\033[0m";

static const char* categoryColor(const std::string& cat) {
    if (cat == "geom")    return ANSI_GREEN;
    if (cat == "texture") return ANSI_YELLOW;
    if (cat == "anim")    return ANSI_CYAN;
    return ANSI_RESET;
}

// ---------------------------------------------------------------------------
//  PrintTable
// ---------------------------------------------------------------------------
void DiffCaptureStatus::PrintTable(
    int    clientPort,
    const std::string& brokerIP,
    int    intervalSec)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return;

    uint64_t now = NowSec();

    // --- Collect sorted ranks ---
    std::vector<int> ranks;
    for (auto& kv : rankHosts_) ranks.push_back(kv.first);
    std::sort(ranks.begin(), ranks.end());

    // --- Compute totals ---
    uint64_t windowSec = 60;
    size_t updatesInWindow = 0;
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        if (now - it->timestamp > windowSec) break;
        ++updatesInWindow;
    }

    // --- Build SSH line ---
    std::ostringstream ssh;
    ssh << "ssh -N -L " << clientPort << ":" << brokerIP << ":" << clientPort
        << " -i ~/.ssh/ed_25519_universal_openssh george2@jureca04.fz-juelich.de";

    int width = 80;

    // --- Clear + print ---
    std::cerr << ANSI_CLEAR;

    // Header box
    std::cerr << ANSI_BRIGHT;
    std::cerr << "+" << std::string(width - 2, '=') << "+" << std::endl;
    std::cerr << "| " << ANSI_CYAN << "DIFF-CAPTURE STATUS TABLE" << ANSI_RESET;
    std::string hdr = std::string(width - 4 - 23, ' ');
    if (!hdr.empty()) std::cerr << hdr;
    std::cerr << "| " << ANSI_GREEN << "interval: " << intervalSec << "s" << ANSI_RESET << " |" << std::endl;
    std::cerr << "+" << std::string(width - 2, '=') << "+" << std::endl;

    // SSH command line
    if ((int)ssh.str().size() <= width - 4) {
        std::cerr << "| " << ANSI_YELLOW << ssh.str();
        std::cerr << std::string(width - 4 - (int)ssh.str().size(), ' ') << " |" << std::endl;
    } else {
        std::cerr << "| " << ANSI_YELLOW << ssh.str().substr(0, width - 8) << "..." << ANSI_RESET << " |" << std::endl;
    }

    // Totals line
    std::ostringstream tot;
    tot << "Ranks: " << ranks.size()
        << "  |  Active: " << lastUpdate_.size()
        << "  |  Updates (last " << windowSec << "s): " << updatesInWindow;
    std::cerr << "| " << ANSI_BRIGHT << tot.str();
    std::string pad = std::string(width - 4 - (int)tot.str().size(), ' ');
    if (!pad.empty()) std::cerr << pad;
    std::cerr << " |" << std::endl;
    std::cerr << "+" << std::string(width - 2, '-') << "+" << std::endl;

    // Column headers
    std::cerr << "| " << std::left << std::setw(6) << "Rank"
              << " " << std::setw(14) << "Updated"
              << " " << std::setw(8) << "Category"
              << " " << std::setw(38) << "File Updated"
              << " |" << std::endl;
    std::cerr << "+" << std::string(width - 2, '-') << "+" << std::endl;

    // Rows
    if (ranks.empty()) {
        std::cerr << "| " << ANSI_YELLOW << "  (no ranks registered yet)" << ANSI_RESET;
        std::cerr << std::string(width - 35, ' ') << " |" << std::endl;
    } else {
        for (int r : ranks) {
            auto uit = lastUpdate_.find(r);
            if (uit == lastUpdate_.end()) {
                std::cerr << "| " << std::left << std::setw(6) << r
                          << " " << std::setw(14) << "---"
                          << " " << std::setw(8) << "--"
                          << " " << std::setw(38) << "(no update yet)"
                          << " |" << std::endl;
            } else {
                uint64_t ago = (now >= uit->second.timestamp)
                    ? (now - uit->second.timestamp) : 0;
                std::string agoStr;
                if (ago < 60) {
                    agoStr = std::to_string(ago) + "s ago";
                } else if (ago < 3600) {
                    agoStr = std::to_string(ago / 60) + "m" + std::to_string(ago % 60) + "s";
                } else {
                    agoStr = std::to_string(ago / 3600) + "h" +
                             std::to_string((ago % 3600) / 60) + "m";
                }

                std::string cat = uit->second.category;
                std::string fn = uit->second.filename;
                if ((int)fn.size() > 38) {
                    fn = "..." + fn.substr(fn.size() - 35);
                }

                bool isRecent = (ago < (uint64_t)intervalSec);
                std::string rowColor = isRecent ? ANSI_GREEN : ANSI_RESET;

                std::cerr << rowColor
                          << "| " << std::left << std::setw(6) << r
                          << " " << std::setw(14) << agoStr
                          << " " << categoryColor(cat) << std::setw(8) << cat << ANSI_RESET
                          << " " << std::setw(38) << fn
                          << " |" << ANSI_RESET << std::endl;
            }
        }
    }

    std::cerr << "+" << std::string(width - 2, '=') << "+" << std::endl;
    std::cerr << "| " << std::left << "Legend: "
              << std::string(70, ' ') << " |" << std::endl;
    std::cerr << "+ " << ANSI_GREEN << " geom  " << ANSI_RESET << "= geometry (.usda)    "
              << ANSI_YELLOW << " texture " << ANSI_RESET << "= materials/textures  "
              << ANSI_CYAN << " anim  " << ANSI_RESET << "= animation clips"
              << std::string(17, ' ') << "+" << std::endl;
    std::cerr << "+" << std::string(width - 2, '=') << "+" << std::endl;

    std::cerr.flush();
}
