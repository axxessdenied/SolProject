#pragma once

// The texture editor (engine plan Phase 9 stage G) - what turns the Forge from
// something that shows a texture into something that authors one.
//
// It owns the open `.tex` document and every edit made to it, and reports "the
// document changed" so the caller rebuilds the image and re-uploads it. Same
// arrangement as `PartEditor` and for the same reason: there is one path from a
// document to the thing the GPU holds, and the editor does not own a second.
//
// ⚑ Every value here is an INTEGER, which is the one way this panel is simpler
// than the part editor's. A texture document has no lengths, no angles and no
// round-off: a pixel coordinate and a colour channel are exact, so none of the
// double-vs-float care `part_editor.hpp` documents applies. What does carry over
// is the discipline behind it - write back only when a widget reports an edit,
// so redrawing a panel never dirties a file nobody touched.

#include "sol/assets/texture_doc.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace forge {

class TextureEditor
{
public:
    // Starts a new document with one fill in it, for the same reason the part
    // editor starts with a box: a tool that opens on nothing gives an author
    // nothing to grab.
    void openNew(const std::string& directory);

    [[nodiscard]] bool openFile(const std::string& path, std::string& status);
    [[nodiscard]] bool save(std::string& status);
    void close();

    // Draws the panel. Returns true when the document changed and the caller
    // must rebuild and re-upload.
    [[nodiscard]] bool draw();

    // Undo is a copy of the document, per the E1 precedent: `TextureDoc` is a
    // plain value and the three committed sources are 0.6-3 KB, so a command
    // pattern would be a second description of every edit for no gain.
    void beginEdit();
    [[nodiscard]] bool undo();

    [[nodiscard]] bool isOpen() const { return m_open; }
    [[nodiscard]] const sol::assets::TextureDoc& doc() const { return m_doc; }
    [[nodiscard]] bool dirty() const { return m_dirty; }
    [[nodiscard]] const std::string& path() const { return m_path; }
    void setBuildError(const std::string& error) { m_buildError = error; }

private:
    [[nodiscard]] bool drawOpList();
    [[nodiscard]] bool drawSelectedOp();
    [[nodiscard]] bool drawParams(sol::assets::TextureLayer& layer);

    static constexpr std::size_t kUndoDepth = 64;

    sol::assets::TextureDoc m_doc;
    std::vector<sol::assets::TextureDoc> m_undo;
    std::string m_path;
    std::string m_buildError;
    std::string m_saveName;
    int m_selected = -1;
    bool m_open = false;
    bool m_dirty = false;
};

} // namespace forge
