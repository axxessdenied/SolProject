#pragma once

// What saves exist, where they live, and what each one says about itself
// (Phase 27). Before this the game had exactly one save - a `savePath` string
// in main.cpp - and "do you have a save" was a bool.
//
// ⚑ THE LAYOUT, and the reason it is directories rather than a naming scheme:
//
//   <user data>/saves/
//       <campaign name>/          one folder per run
//           save_01.sav           manual saves, numbered not named
//           auto_01.sav           the autosave ring
//           quick.sav             F9
//
// A campaign is a DIRECTORY, so it can be listed, deleted and reasoned about
// as one thing, and so two runs cannot collide however the player names their
// saves. The folder name IS the campaign name - see sanitizeCampaignName for
// why that is a restriction on the name rather than a mapping to be stored.
//
// ⚑⚑ SAVE FILES ARE NUMBERED AND NOT NAMED, WHICH IS THE OPPOSITE CHOICE. A
// save's display name lives in its HEADER (SaveInfo below), never in its
// filename: names are the player's, they contain anything, they get renamed,
// and two of them can be identical. A filename has to be unique and portable
// and is nobody's business. Keeping the two apart is what stops "My Save #2 :)"
// from being a filesystem question.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace game {

// What a save says about itself, without loading it.
//
// ⚑⚑ WHY THIS IS IN THE SAVE AND NOT IN A SIDECAR FILE. Everything here except
// `systemName` was already in the first forty bytes of the blob - seed, system
// index, credits, worldSeconds, hardcore - so the browser was always one read
// away from most of a row. The system NAME is the exception and it is the one
// that decided the shape: turning a system INDEX into a name needs that seed's
// whole galaxy generated (80 systems, plus mining params derived from the
// loaded defs), which is a second way to compute something the world already
// knows. Recording the name the world had at save time is one source of truth;
// regenerating it per listed row is two.
struct SaveInfo
{
    std::string displayName;        // what the player called it
    std::string systemName;         // resolved when the save was written
    std::uint64_t savedAtUnix = 0;  // platform::wallClockSeconds at write
    std::uint64_t universeSeed = 0; // which galaxy this is
    double credits = 0.0;           // player wealth, for the row
    double worldSeconds = 0.0;      // elapsed in-game time, i.e. playtime
    bool hardcore = false;          // the run's own rule, carried by the save
};

// Reads a save's header without restoring anything. False for a missing file,
// a foreign one, a truncated one, or one from another version - all of which
// the browser shows the same way, as a row it cannot offer.
//
// ⚑ Defined in space_world.cpp, beside the format it reads, rather than in
// save_catalog.cpp. The catalog owns the vocabulary; the world owns the bytes.
// A second reader of the header living away from the writer is how the two
// drift apart.
//
// ⚑ It reads the WHOLE file and parses the first block, because readFileBytes
// is all-or-nothing. A measured save is ~9 KB (game.unit's fixture worlds), so
// a browser listing a few dozen is reading well under a megabyte, once, when
// it opens. If saves ever grow enough for this to be felt, the fix is a
// bounded read in the platform layer, not a cache here - a cache would have to
// be invalidated by the game's own writes, which is a harder problem than the
// one it solves.
[[nodiscard]] bool readSaveInfo(const char* path, SaveInfo& out);

// Which of the three kinds of save a file is. The kind is carried by the
// FILENAME - `save_`, `auto_`, `quick` - because it governs how the file is
// replaced, and replacement policy is the catalog's business rather than the
// player's. A manual save is never overwritten by the game; an autosave is
// overwritten by the next one round the ring; the quicksave is overwritten
// every time.
enum class SaveKind : std::uint32_t
{
    Manual,
    Auto,
    Quick,
};

struct SaveSlot
{
    std::string path;     // full path, '/' separated
    std::string fileName; // leaf, e.g. "auto_02.sav"
    SaveInfo info;        // read from the file's own header
    SaveKind kind = SaveKind::Manual;
    std::uint64_t modifiedAt = 0; // platform::fileModificationTime, for order
    std::uint32_t index = 0;      // the number in the filename; 0 for quick
};

struct Campaign
{
    std::string name;            // == the folder name; see sanitizeCampaignName
    std::string directory;       // full path, '/' separated, no trailing '/'
    std::vector<SaveSlot> saves; // newest first

    [[nodiscard]] bool empty() const { return saves.empty(); }

    // The row the main menu's Continue button acts on: the newest save in this
    // campaign, or nullptr when the folder holds none. Newest by file time
    // rather than by the header's stamp, because the file time is what the OS
    // guarantees and the header's stamp is what a copied file brings with it.
    [[nodiscard]] const SaveSlot* newest() const { return saves.empty() ? nullptr : &saves.front(); }
};

// Reduces a name the player typed to one a directory can be called, on both
// platforms and in an archive.
//
// ⚑⚑ THE FOLDER NAME IS THE CAMPAIGN NAME, so this is a restriction on what a
// campaign may be CALLED rather than a mapping from a name to a path. The
// alternative - store the real name in a sidecar and slug the folder - buys
// arbitrary names at the price of a second file that can go missing, be copied
// without its folder, or disagree with it. A campaign name is a label on a
// folder; making it look like one is not a compromise.
//
// Keeps letters, digits, spaces, '-' and '_'; collapses runs of spaces; trims
// both ends; caps length. An empty or fully-stripped result becomes "Campaign",
// because a nameless folder is not a thing the browser can show.
[[nodiscard]] std::string sanitizeCampaignName(std::string_view name);

// Formats a `SaveInfo::worldSeconds` as elapsed play time, e.g. "3h 20m".
[[nodiscard]] std::string formatPlaytime(double worldSeconds);

// Formats a `SaveInfo::savedAtUnix` in the machine's local zone, e.g.
// "2026-08-28 16:04". Empty when the stamp cannot be broken down, so a caller
// that prints it unguarded shows nothing rather than a wrong date.
[[nodiscard]] std::string formatSaveDate(std::uint64_t unixSeconds);

class SaveCatalog
{
public:
    // `savesDirectory` is created if missing. Everything this class touches is
    // under it, and it never looks outside - which is what makes the recursive
    // delete below safe to expose to a menu.
    void initialize(std::string savesDirectory);

    // Re-reads the whole tree: every campaign folder, every save's header.
    // Called when the browser opens rather than every frame - it is file I/O,
    // and nothing else changes these files while the game is running.
    void rescan();

    [[nodiscard]] const std::vector<Campaign>& campaigns() const { return m_campaigns; }

    [[nodiscard]] const std::string& root() const { return m_root; }

    [[nodiscard]] bool empty() const { return m_campaigns.empty(); }

    // The campaign holding the newest save anywhere, or nullptr if there is
    // none. This is what the main menu's Continue resumes.
    [[nodiscard]] const Campaign* mostRecentCampaign() const;
    [[nodiscard]] const SaveSlot* mostRecentSave() const;

    [[nodiscard]] const Campaign* find(std::string_view name) const;

    // Creates a campaign folder for `desiredName`, sanitized, with a numeric
    // suffix if that name is taken. Returns the created campaign, or nullptr
    // if the directory could not be made. The catalog is rescanned.
    const Campaign* createCampaign(std::string_view desiredName);

    // Where the next save of each kind belongs in `campaign`.
    //
    // ⚑ A manual save always takes a FRESH number - the game never overwrites
    // one, because a player who names a save is telling you it is worth
    // keeping. Autosaves rotate through `ringSize` slots, filling unused ones
    // first and then replacing the OLDEST; the quicksave is one fixed file.
    [[nodiscard]] std::string nextManualPath(const Campaign& campaign) const;
    [[nodiscard]] std::string nextAutoPath(const Campaign& campaign, std::uint32_t ringSize) const;
    [[nodiscard]] std::string quickPath(const Campaign& campaign) const;

    // Deletes one save file, or a campaign folder and everything in it. Both
    // rescan. `deleteCampaign` refuses a name that is not a campaign this
    // catalog listed, so a caller cannot hand it an arbitrary path.
    bool deleteSave(const SaveSlot& slot);
    bool deleteCampaign(std::string_view name);

private:
    std::string m_root; // no trailing '/'
    std::vector<Campaign> m_campaigns;
};

} // namespace game
