#pragma once

// Moving a model's surface onto a material row (engine plan Phase 25 stage D).
//
// ⚑⚑ IT IS HERE RATHER THAN INSIDE `DefEditor` FOR ONE REASON: `def_editor.cpp`
// pulls ImGui, and this rule has nothing to do with a window. It is the one
// piece of stage D that edits two committed files at once, and the way it fails
// is silent - a comment dropped, or a key left behind on the row that may no
// longer carry it - so it wants a test more than any widget in the panel does.
// `outputs.hpp` and `material_state.hpp` made the same move for the same
// reason: the DECISION goes somewhere a test can reach.
//
// ⚑ WHY THE MOVE HAS TO BE ATOMIC. `parseModel` refuses a row that names a
// material AND sets any of `texture`, `emissive`, `translucent` or `alpha` -
// deliberately, because a precedence rule would be invisible in the file. So
// half of this operation is not a smaller version of it, it is a def file the
// game will not load.

#include "sol/assets/def_doc.hpp"

#include <string>

namespace forge {

// The four keys a `[[model]]` row may carry INSTEAD of naming a material. In
// the schema's own order, and the same set `kModelSurfaceKeys` refuses.
inline constexpr const char* kSurfaceKeys[] = {"texture", "emissive", "translucent", "alpha"};

// Moves whichever of those `model` carries onto `material`, then points `model`
// at it. `material` is expected to already have its `id`.
//
// ⚑⚑ VERBATIM, AND ONLY WHAT IS THERE. Re-emitting from a parsed `MaterialDef`
// would write all four every time - burying the one value an author set among
// three defaults - and would turn their `alpha = 0.30` into `0.3`, which is the
// exact corruption `def_doc`'s "values are verbatim text" rule exists to stop.
//
// ⚑⚑ THE `leading` TRIVIA TRAVELS WITH THE KEY, and in this game that trivia is
// usually the REASON: `models.toml` explains its cockpit `emissive` in three
// lines about a vacuum ambient of 1.2% being pitch black for a room somebody is
// sitting in. A move that dropped those would delete the only record of it,
// byte-exactly and unrecoverably, in a gesture an author would read as tidying.
inline void moveSurfaceToMaterial(sol::assets::DefRow& model, sol::assets::DefRow& material)
{
    for (const char* key : kSurfaceKeys) {
        const sol::assets::DefKey* found = model.find(key);
        if (found == nullptr) {
            continue;
        }
        const std::string leading = found->leading;
        material.set(key, found->value());
        if (sol::assets::DefKey* moved = material.find(key); moved != nullptr) {
            moved->leading = leading;
        }
        model.remove(key);
    }
    model.set("material", sol::assets::defString(std::string(material.id())));
}

} // namespace forge
