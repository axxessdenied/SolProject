#include "save_catalog.hpp"

#include "sol/core/log.hpp"
#include "sol/platform/file_io.hpp"
#include "sol/platform/time.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace game {

namespace {

using sol::platform::createDirectories;
using sol::platform::deleteDirectory;
using sol::platform::deleteFile;
using sol::platform::fileModificationTime;
using sol::platform::listDirectories;
using sol::platform::listFiles;

constexpr const char* kManualPrefix = "save_";
constexpr const char* kAutoPrefix = "auto_";
constexpr const char* kQuickName = "quick.sav";
constexpr const char* kExtension = ".sav";

// A cap rather than a guess: 64 is comfortably longer than anything a person
// types into a one-line field and comfortably shorter than the shortest path
// limit this has to survive, once a campaign name is joined to a saves
// directory that already sits under a user profile.
constexpr std::size_t kMaxCampaignName = 64;

// The suffix hunt for a taken campaign name stops here rather than looping
// forever. Reaching it means 999 campaigns share one name, which is a person
// doing something deliberate, and refusing is better than spinning.
constexpr std::uint32_t kMaxNameSuffix = 999;

[[nodiscard]] std::string leafOf(const std::string& path)
{
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// True when `text` starts with `prefix`. Written out rather than reached for
// through <string_view>::starts_with only because both operands here are
// std::string and the call sites read better this way.
[[nodiscard]] bool startsWith(const std::string& text, const char* prefix)
{
    const std::size_t length = std::char_traits<char>::length(prefix);
    return text.size() >= length && text.compare(0, length, prefix) == 0;
}

// The number in "save_07.sav" / "auto_03.sav". Zero when there is not one,
// which is also the quicksave's index - the two are never compared, because
// the kind is checked first everywhere this is read.
[[nodiscard]] std::uint32_t indexFromFileName(const std::string& fileName, const char* prefix)
{
    const std::size_t start = std::char_traits<char>::length(prefix);
    std::uint32_t value = 0;
    std::size_t digits = 0;
    for (std::size_t i = start; i < fileName.size() && std::isdigit(static_cast<unsigned char>(fileName[i]));
         ++i) {
        value = value * 10 + static_cast<std::uint32_t>(fileName[i] - '0');
        ++digits;
    }
    return digits == 0 ? 0 : value;
}

[[nodiscard]] bool classify(const std::string& fileName, SaveKind& kind, std::uint32_t& index)
{
    if (fileName == kQuickName) {
        kind = SaveKind::Quick;
        index = 0;
        return true;
    }
    if (fileName.size() <= 4 || fileName.compare(fileName.size() - 4, 4, kExtension) != 0) {
        return false; // not a save file at all; a stray anything is ignored
    }
    if (startsWith(fileName, kManualPrefix)) {
        kind = SaveKind::Manual;
        index = indexFromFileName(fileName, kManualPrefix);
        return index != 0;
    }
    if (startsWith(fileName, kAutoPrefix)) {
        kind = SaveKind::Auto;
        index = indexFromFileName(fileName, kAutoPrefix);
        return index != 0;
    }
    return false;
}

[[nodiscard]] std::string numberedName(const char* prefix, std::uint32_t index)
{
    char buffer[64] = {};
    // %02u so the common case sorts lexically as well as numerically; a
    // three-digit index simply widens, which is why nothing here depends on
    // the width being two.
    (void)std::snprintf(buffer, sizeof(buffer), "%s%02u%s", prefix, index, kExtension);
    return buffer;
}

} // namespace

std::string sanitizeCampaignName(std::string_view name)
{
    std::string out;
    out.reserve(name.size());
    bool pendingSpace = false;
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == ' ' || c == '\t') {
            // Collapsed, and only emitted once something else follows, which
            // trims the leading run for free.
            pendingSpace = !out.empty();
            continue;
        }
        const bool keep = std::isalnum(byte) != 0 || c == '-' || c == '_';
        if (!keep) {
            continue;
        }
        if (pendingSpace && out.size() + 1 < kMaxCampaignName) {
            out.push_back(' ');
        }
        pendingSpace = false;
        if (out.size() >= kMaxCampaignName) {
            break;
        }
        out.push_back(c);
    }
    // A trailing space cannot survive: Windows silently strips trailing spaces
    // and dots from directory names, so a folder created as "Run " is a folder
    // called "Run" that the catalog would then fail to find by the name it
    // asked for. Refusing to produce one is cheaper than reconciling later.
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out.empty() ? std::string("Campaign") : out;
}

std::string formatPlaytime(double worldSeconds)
{
    if (worldSeconds < 0.0) {
        worldSeconds = 0.0;
    }
    const auto total = static_cast<std::uint64_t>(worldSeconds);
    const std::uint64_t hours = total / 3600;
    const std::uint64_t minutes = (total % 3600) / 60;
    char buffer[64] = {};
    if (hours > 0) {
        (void)std::snprintf(buffer,
                            sizeof(buffer),
                            "%lluh %llum",
                            static_cast<unsigned long long>(hours),
                            static_cast<unsigned long long>(minutes));
    } else {
        // Minutes alone under an hour: "0h 04m" reads like a stopwatch nobody
        // asked for, and the first minutes of a run are when this is shown.
        (void)std::snprintf(buffer, sizeof(buffer), "%llum", static_cast<unsigned long long>(minutes));
    }
    return buffer;
}

std::string formatSaveDate(std::uint64_t unixSeconds)
{
    sol::platform::CalendarTime when;
    if (unixSeconds == 0 || !sol::platform::localCalendarTime(unixSeconds, when)) {
        return {}; // the caller prints nothing rather than a wrong date
    }
    char buffer[32] = {};
    (void)std::snprintf(buffer,
                        sizeof(buffer),
                        "%04d-%02d-%02d %02d:%02d",
                        when.year,
                        when.month,
                        when.day,
                        when.hour,
                        when.minute);
    return buffer;
}

void SaveCatalog::initialize(std::string savesDirectory)
{
    while (!savesDirectory.empty() && (savesDirectory.back() == '/' || savesDirectory.back() == '\\')) {
        savesDirectory.pop_back();
    }
    m_root = std::move(savesDirectory);
    if (!m_root.empty() && !createDirectories(m_root.c_str())) {
        // Not fatal and must not be: the menus still come up, they simply have
        // nothing to list. A warning is what stops it looking like data loss.
        SOL_LOG_WARN("saves: could not create %s - saving will not work", m_root.c_str());
    }
    rescan();
}

void SaveCatalog::rescan()
{
    m_campaigns.clear();
    if (m_root.empty()) {
        return;
    }
    for (const std::string& directory : listDirectories(m_root.c_str())) {
        Campaign campaign;
        campaign.directory = directory;
        campaign.name = leafOf(directory);
        if (campaign.name.empty()) {
            continue;
        }
        for (const std::string& file : listFiles(directory.c_str())) {
            SaveSlot slot;
            slot.fileName = leafOf(file);
            if (!classify(slot.fileName, slot.kind, slot.index)) {
                continue; // a stray file in a save folder is not an error
            }
            if (!readSaveInfo(file.c_str(), slot.info)) {
                // ⚑ A save this build cannot read is DROPPED, not listed as a
                // broken row. The version check is exact, so "cannot read"
                // means "from another version" far more often than it means
                // "corrupt", and a list of rows that refuse to load is worse
                // than a short list. The file is left on disk either way.
                SOL_LOG_INFO("saves: skipping '%s' (unreadable or from another version)", file.c_str());
                continue;
            }
            slot.path = file;
            slot.modifiedAt = fileModificationTime(file.c_str());
            campaign.saves.push_back(std::move(slot));
        }
        // Newest first, by FILE time rather than by the header's stamp: the
        // file time is the one the OS maintains, and a save copied in from
        // another machine brings its old stamp with it.
        std::sort(campaign.saves.begin(), campaign.saves.end(), [](const SaveSlot& a, const SaveSlot& b) {
            return a.modifiedAt > b.modifiedAt;
        });
        m_campaigns.push_back(std::move(campaign));
    }
    // Campaigns in most-recently-touched order too, so the one being played
    // sits at the top of the browser. An empty campaign has no time of its own
    // and sorts last rather than first - it is still listed, which is the
    // whole reason listDirectories exists.
    std::sort(m_campaigns.begin(), m_campaigns.end(), [](const Campaign& a, const Campaign& b) {
        const std::uint64_t left = a.saves.empty() ? 0 : a.saves.front().modifiedAt;
        const std::uint64_t right = b.saves.empty() ? 0 : b.saves.front().modifiedAt;
        if (left != right) {
            return left > right;
        }
        return a.name < b.name; // stable, readable order for the timeless ones
    });
}

const Campaign* SaveCatalog::mostRecentCampaign() const
{
    for (const Campaign& campaign : m_campaigns) {
        if (!campaign.saves.empty()) {
            return &campaign; // already sorted newest-first
        }
    }
    return nullptr;
}

const SaveSlot* SaveCatalog::mostRecentSave() const
{
    const Campaign* campaign = mostRecentCampaign();
    return campaign == nullptr ? nullptr : campaign->newest();
}

const Campaign* SaveCatalog::find(std::string_view name) const
{
    for (const Campaign& campaign : m_campaigns) {
        if (campaign.name == name) {
            return &campaign;
        }
    }
    return nullptr;
}

const Campaign* SaveCatalog::createCampaign(std::string_view desiredName)
{
    if (m_root.empty()) {
        return nullptr;
    }
    const std::string base = sanitizeCampaignName(desiredName);
    std::string name = base;
    // Two runs may honestly want the same name; the second gets " 2".
    for (std::uint32_t suffix = 2; find(name) != nullptr; ++suffix) {
        if (suffix > kMaxNameSuffix) {
            SOL_LOG_WARN("saves: too many campaigns named '%s'", base.c_str());
            return nullptr;
        }
        char buffer[16] = {};
        (void)std::snprintf(buffer, sizeof(buffer), " %u", suffix);
        name = base;
        // The suffix must not push the name past what the folder can hold, and
        // truncating the BASE is the only way to keep the suffix - which is
        // the part that makes it unique.
        const std::size_t room = kMaxCampaignName - std::char_traits<char>::length(buffer);
        if (name.size() > room) {
            name.resize(room);
        }
        name += buffer;
    }
    const std::string directory = m_root + "/" + name;
    if (!createDirectories(directory.c_str())) {
        SOL_LOG_ERROR("saves: could not create campaign directory %s", directory.c_str());
        return nullptr;
    }
    rescan();
    return find(name);
}

std::string SaveCatalog::nextManualPath(const Campaign& campaign) const
{
    // One past the highest manual number PRESENT, which is the only promise
    // that matters: the result never names a file that already exists, so a
    // manual save can never overwrite another one.
    //
    // ⚑ A deleted save's number IS handed out again, and that is fine rather
    // than merely tolerable - the player never sees a filename. A save's name
    // lives in its header, so `save_02.sav` is an implementation detail twice
    // over, and "remember every number ever issued" would need persistent
    // state of its own to answer a question nobody asks. The first draft of
    // this comment claimed the opposite and its test caught it.
    std::uint32_t highest = 0;
    for (const SaveSlot& slot : campaign.saves) {
        if (slot.kind == SaveKind::Manual) {
            highest = std::max(highest, slot.index);
        }
    }
    return campaign.directory + "/" + numberedName(kManualPrefix, highest + 1);
}

std::string SaveCatalog::nextAutoPath(const Campaign& campaign, std::uint32_t ringSize) const
{
    if (ringSize == 0) {
        ringSize = 1; // a ring of none is a ring of one; never divide by zero
    }
    // Fill an unused slot before replacing any used one, so a fresh campaign
    // grows its ring rather than overwriting auto_01 forever.
    for (std::uint32_t index = 1; index <= ringSize; ++index) {
        const std::string name = numberedName(kAutoPrefix, index);
        const bool used = std::any_of(campaign.saves.begin(),
                                      campaign.saves.end(),
                                      [&name](const SaveSlot& slot) { return slot.fileName == name; });
        if (!used) {
            return campaign.directory + "/" + name;
        }
    }
    // Ring full: the oldest autosave WITHIN the ring is replaced. Slots above
    // the ring size are left alone - lowering the setting must not delete the
    // autosaves a higher setting already made.
    const SaveSlot* oldest = nullptr;
    for (const SaveSlot& slot : campaign.saves) {
        if (slot.kind != SaveKind::Auto || slot.index > ringSize) {
            continue;
        }
        if (oldest == nullptr || slot.modifiedAt < oldest->modifiedAt) {
            oldest = &slot;
        }
    }
    return oldest != nullptr ? oldest->path : campaign.directory + "/" + numberedName(kAutoPrefix, 1);
}

std::string SaveCatalog::quickPath(const Campaign& campaign) const
{
    return campaign.directory + "/" + kQuickName;
}

bool SaveCatalog::deleteSave(const SaveSlot& slot)
{
    // The path is copied before the delete: `slot` is a reference INTO
    // m_campaigns, and rescan() clears it out from under us.
    const std::string path = slot.path;
    if (!deleteFile(path.c_str())) {
        SOL_LOG_ERROR("saves: could not delete %s", path.c_str());
        return false;
    }
    rescan();
    return true;
}

bool SaveCatalog::deleteCampaign(std::string_view name)
{
    // ⚑⚑ THE PATH COMES FROM THE CATALOG, NEVER FROM THE CALLER. This is the
    // one recursive delete in the game, and it will be wired to a menu button;
    // looking the name up here means a caller cannot hand it a path at all,
    // let alone one outside the saves directory.
    const Campaign* campaign = find(name);
    if (campaign == nullptr) {
        return false;
    }
    const std::string directory = campaign->directory;
    if (!deleteDirectory(directory.c_str())) {
        SOL_LOG_ERROR("saves: could not delete campaign %s", directory.c_str());
        return false;
    }
    SOL_LOG_INFO("saves: deleted campaign %s", directory.c_str());
    rescan();
    return true;
}

} // namespace game
