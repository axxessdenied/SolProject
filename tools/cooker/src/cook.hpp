#pragma once

// The cook itself: what an extension means, where its output goes, when it can
// be skipped, and what the output directory is entitled to hold afterwards
// (engine plan Phase 24 stage T).
//
// ⚑⚑ WHY THIS IS A LIBRARY AND NOT `main.cpp` ANY MORE - TWO REASONS, AND THE
// SECOND IS WHAT THE STAGE EXISTS FOR. First, `outputs.hpp`'s rule, of which
// this file was the last outstanding debt: `cooker.unit` links the LIBRARY, and
// a rule that lives in the executable is a rule with no test. The extension
// dispatch, the staleness policy, the output-collision abort and the stray
// sweep had been untested since Phase 5 for no reason other than which file
// they sat in - and the collision abort is a rule that can stop an entire
// build, while the sweep is a rule that DELETES.
//
// ⚑⚑ Second: THE FORGE ALREADY LINKS `sol_cooker_lib`. A cook that is a
// library call is a cook the tool can run, which is what lets an author press a
// button instead of leaving the tool for a command line - and it does so
// WITHOUT shipping `cooker.exe`, so `packaging/check_layout.cmake`'s `cooker*`
// exclusion stays true rather than being deleted to make room for a feature.
//
// ⚑ The split inside this file is the one `outputs.hpp` already made:
// everything above `cookDirectory` is pure and DECIDES, and `cookDirectory` is
// the only thing here that touches a disk. The executable is now argv plus one
// call to it.

#include <string>
#include <vector>

namespace sol::cooker {

// What a source file cooks into, decided by its extension and nothing else.
//
// ⚑ `None` is the common case rather than an error: the cooker walks a source
// tree recursively, and that tree holds `.gitkeep`, the `.ttf` files a `.font`
// manifest names, and whatever else an author keeps beside their work. A file
// this enum does not name is skipped in silence, which is the behaviour the
// dispatch has always had and the reason it needs saying out loud here.
enum class CookKind
{
    None,
    Texture,    // .png       -> .stex   an imported image
    TextureDoc, // .tex       -> .stex   an authored texture document
    Mesh,       // .gltf/.glb -> .smesh  an imported mesh
    ForgeMesh,  // .forge     -> .smesh  an authored part tree, plus its levels
    Font,       // .font      -> .sfont  a manifest naming .ttf sources
    Sound,      // .wav/.ogg  -> .saud
};

// When a cook of this kind may be skipped because its output is current.
enum class StalenessRule
{
    // The output is newer than the source. The ordinary case.
    Timestamp,
    // ⚑ The output is newer than the source AND than everything beside it. A
    // `.font` manifest names TTFs in its own directory, so its own timestamp is
    // not enough - editing a font file has to force a re-cook too.
    TimestampAndSiblings,
    // ⚑⚑ NEVER SKIPPED, AND THIS IS A DELIBERATE REFUSAL TO BE CLEVER. A
    // `.forge` cooks to a SET of files since stage F - level 0 plus however many
    // levels the policy accepts - and how many that is cannot be known without
    // building the mesh and decimating it. A staleness check that cannot know
    // how many outputs to look for is exactly how a stale level survives a
    // re-cook and gets drawn at distance, where nobody is looking closely. A
    // part tree always cooks: it is cheap (the committed assets are 2,298
    // triangles between them) and it is the only version of this check that
    // cannot be wrong.
    AlwaysCook,
};

// The kind `path`'s extension names. Case-insensitive: Blender will hand back
// `.GLTF` on a case-preserving filesystem and mean the same thing.
[[nodiscard]] CookKind cookKindForSource(const std::string& path);

// The cooked extension a kind produces, with its dot; empty for `None`.
[[nodiscard]] const char* cookedExtension(CookKind kind);

[[nodiscard]] StalenessRule stalenessRuleFor(CookKind kind);

// One source file and where it cooks to.
struct CookJob
{
    std::string source;
    std::string output;
    CookKind kind = CookKind::None;
};

// Every job a listing implies, in listing order, with sources this cooker does
// not know left out. Pure: it is handed the listing rather than reading one, so
// the dispatch can be asserted against a directory that does not exist.
//
// ⚑ Output paths are `outputDirectory + "/" + stem + extension` - keyed on the
// file STEM, so the output directory is FLAT however deep the source tree is.
// That is what makes the collision below possible and is not incidental: a
// cooked name is what a `[[model]]` row names, and rows do not carry paths.
[[nodiscard]] std::vector<CookJob> planCook(const std::vector<std::string>& sources,
                                            const std::string& outputDirectory);

// Two sources that cook to one output.
struct CookCollision
{
    std::string firstSource;
    std::string secondSource;
    std::string output;
};

// ⚑⚑ Every pair of jobs sharing an output, and finding one must abort the WHOLE
// cook rather than skipping the pair. Whichever cooked last would silently win,
// and the staleness check would then report the loser as up to date - so the
// asset an author is editing stops changing and nothing says why. The hazard
// has been here since Phase 5 and was unreachable while `.gltf` was the only
// mesh source; `.forge` is what makes `gate.forge` beside `gate.gltf` something
// an author can type.
[[nodiscard]] std::vector<CookCollision> cookCollisions(const std::vector<CookJob>& jobs);

// The refusal a set of collisions produces, naming the first pair and counting
// the rest. Empty for no collisions.
//
// ⚑⚑ IT NAMES FILES BECAUSE A COUNT IS NOT AN ANSWER TO THIS QUESTION, and that
// was found by giving the Forge a Cook button (stage T) and then a second
// texture source (stage U2). The refusal used to read "2 output collision(s);
// nothing cooked" with the two filenames going only to the LOG - fine for
// `cooker.exe`, whose output IS the log, and useless in a status bar, which is
// the only surface an author in the tool has. ⚑ The whole content of this
// failure is WHICH TWO FILES, so the sentence carries them.
//
// ⚑ Pure and here rather than in the Forge, for `summary`'s reason one line
// down: a sentence an author reads is a sentence `cooker.unit` can assert. It
// also means the tool and the command line refuse in the same words.
[[nodiscard]] std::string collisionSentence(const std::vector<CookCollision>& collisions);

// What one run of `cookDirectory` did.
struct CookReport
{
    int cooked = 0;
    int skipped = 0;
    int failed = 0;
    // ⚑ Non-empty when the cook refused BEFORE running any job - an output
    // directory that cannot be created, or outputs that collide. It is separate
    // from `failed` because the two are different things to be told: `failed`
    // means some assets did not cook, and a refusal means NOTHING was written
    // and the directory is exactly as it was.
    std::string refusal;

    [[nodiscard]] bool ok() const { return failed == 0 && refusal.empty(); }

    // One line for a log or a status bar. Pure, so what the Forge shows an
    // author is the same sentence `cooker.unit` asserts.
    [[nodiscard]] std::string summary() const;
};

// Cooks every source under `sourceDirectory` into `outputDirectory`, creating
// the output directory if it is missing, and sweeps outputs whose source is
// gone. Recursive over the source tree, flat in the output.
//
// ⚑ THE ONE FUNCTION HERE THAT TOUCHES A DISK, and the whole of what
// `cooker.exe` does. Progress and every failure are logged as they happen -
// the report is a tally, not a transcript.
[[nodiscard]] CookReport cookDirectory(const std::string& sourceDirectory,
                                       const std::string& outputDirectory);

} // namespace sol::cooker
