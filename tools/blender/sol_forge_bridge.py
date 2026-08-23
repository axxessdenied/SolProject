# Send to Forge - a Blender bridge for SolProject (engine plan Phase 9 stage L).
#
# Exports the scene (or the selection) as glTF into the project's
# `blender-inbox/`, which the Forge watches. The Forge converts each object into
# a `.forge` part and opens the result, so the round trip is one button here and
# nothing at all in the other window.
#
# WHY THE INBOX IS NOT UNDER `assets/`
#   The cooker walks `assets/` recursively into one flat output directory keyed
#   on the file STEM, so `ship.gltf` under `assets/` and `ship.forge` in
#   `assets/meshes/` both cook to `ship.smesh` - and that guard aborts the whole
#   cook, not just the pair. The glTF is transport; the `.forge` the Forge writes
#   is the source that gets committed.
#
# WHAT THE EXPORT SETTINGS ARE FOR - none of these are taste:
#   +Y up          the engine is Y-up and Blender is Z-up
#   apply modifiers  a Subdivision Surface that is not applied exports as the
#                    cage, which is not the shape anyone modelled
#   no Draco       the importer in this repo has no Draco decompressor, and a
#                  compressed file fails at read rather than looking wrong
#   no materials   a mesh's texture comes from its `[[model]]` row, not glTF
#   no animation   nothing in this engine has a bone (plan Phase 9, out of scope)

bl_info = {
    "name": "Send to Forge (SolProject)",
    "author": "SolProject",
    "version": (1, 0, 0),
    "blender": (3, 0, 0),
    "location": "View3D > Sidebar > Forge, and File > Export",
    "description": "Export meshes into the Forge's inbox, which imports them as .forge parts",
    "category": "Import-Export",
}

import os

import bpy
from bpy.props import BoolProperty, StringProperty
from bpy.types import AddonPreferences, Operator, Panel

INBOX_NAME = "blender-inbox"


def _inbox_path(context):
    """The drop directory, or None with a reason."""
    prefs = context.preferences.addons[__name__].preferences
    root = bpy.path.abspath(prefs.project_root).strip()
    if not root:
        return None, "Set the SolProject root in Preferences > Add-ons > Send to Forge"
    if not os.path.isdir(root):
        return None, "Project root does not exist: {}".format(root)
    # A cheap sanity check that this is actually the repo and not, say, the
    # user's home directory - writing a stray inbox into the wrong tree is the
    # kind of thing nobody notices for a week.
    if not os.path.isdir(os.path.join(root, "assets", "meshes")):
        return None, "No assets/meshes under {} - is that the SolProject root?".format(root)
    inbox = os.path.join(root, INBOX_NAME)
    try:
        os.makedirs(inbox, exist_ok=True)
    except OSError as error:
        return None, "Cannot create {}: {}".format(inbox, error)
    return inbox, None


def _supported_kwargs(operator, wanted):
    """Only the keys this Blender's glTF exporter actually has.

    The exporter's argument list moves between versions and passing an unknown
    keyword is a hard error, so a bridge that names them literally breaks on the
    next Blender. Filtering against the operator's own RNA keeps one file
    working across versions and silently drops what is not there.
    """
    try:
        known = {p.identifier for p in operator.get_rna_type().properties}
    except Exception:
        return dict(wanted)
    return {key: value for key, value in wanted.items() if key in known}


def _export(context, filepath, use_selection):
    wanted = {
        "filepath": filepath,
        "export_format": "GLB",  # one self-contained file, and never deprecated
        "export_yup": True,
        "export_apply": True,  # modifiers, or the cage is what ships
        "use_selection": use_selection,
        "use_visible": not use_selection,
        "export_normals": True,
        "export_texcoords": True,
        "export_tangents": False,
        "export_materials": "NONE",
        "export_animations": False,
        "export_skins": False,
        "export_morph": False,
        "export_cameras": False,
        "export_lights": False,
        "export_extras": False,
        "export_draco_mesh_compression_enable": False,
    }
    kwargs = _supported_kwargs(bpy.ops.export_scene.gltf, wanted)
    return bpy.ops.export_scene.gltf(**kwargs)


class SOLFORGE_AddonPreferences(AddonPreferences):
    bl_idname = __name__

    project_root: StringProperty(
        name="SolProject root",
        description="The repository root - the directory holding assets/ and tools/",
        subtype="DIR_PATH",
        default="",
    )

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "project_root")
        inbox, error = _inbox_path(context)
        if error:
            layout.label(text=error, icon="ERROR")
        else:
            layout.label(text="Drops into {}".format(inbox), icon="CHECKMARK")


class SOLFORGE_OT_send(Operator):
    """Export to the Forge's inbox; the Forge picks it up and opens it"""

    bl_idname = "solforge.send"
    bl_label = "Send to Forge"
    bl_options = {"REGISTER"}

    use_selection: BoolProperty(
        name="Selected only",
        description="Export just the selected objects rather than everything visible",
        default=False,
    )

    # ⚑ The FILE STEM decides which `.forge` the import merges into, so it is
    # the single most consequential field here - getting it wrong writes a
    # second asset rather than updating the one you meant.
    name: StringProperty(
        name="Asset name",
        description="The .forge this becomes under assets/meshes/. Re-sending the "
        "same name updates that asset in place",
        default="",
    )

    def invoke(self, context, event):
        if not self.name:
            self.name = _default_name(context)
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        inbox, error = _inbox_path(context)
        if error:
            self.report({"ERROR"}, error)
            return {"CANCELLED"}

        stem = _sanitize(self.name) or _default_name(context)
        target = os.path.join(inbox, stem + ".glb")

        objects = (
            [o for o in context.selected_objects if o.type == "MESH"]
            if self.use_selection
            else [o for o in context.view_layer.objects if o.type == "MESH" and o.visible_get()]
        )
        if not objects:
            self.report({"ERROR"}, "No mesh objects to send")
            return {"CANCELLED"}

        try:
            result = _export(context, target, self.use_selection)
        except Exception as failure:  # the exporter raises on a bad combination
            self.report({"ERROR"}, "Export failed: {}".format(failure))
            return {"CANCELLED"}
        if "FINISHED" not in result:
            self.report({"ERROR"}, "Export was cancelled")
            return {"CANCELLED"}

        self.report(
            {"INFO"},
            "Sent {} object(s) as {}.glb - the Forge imports it as {}.forge".format(
                len(objects), stem, stem
            ),
        )
        return {"FINISHED"}


def _sanitize(name):
    """A file stem the cooker and the Forge will both accept."""
    kept = [c if (c.isalnum() or c in "_-") else "_" for c in name.strip()]
    return "".join(kept).strip("_")


def _default_name(context):
    """The .blend's own name, then the active object's, then a fallback."""
    blend = bpy.path.display_name_from_filepath(bpy.data.filepath)
    if blend:
        return _sanitize(blend)
    active = context.view_layer.objects.active
    if active is not None:
        return _sanitize(active.name)
    return "untitled"


class SOLFORGE_PT_panel(Panel):
    bl_label = "Forge"
    bl_idname = "SOLFORGE_PT_panel"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Forge"

    def draw(self, context):
        layout = self.layout
        inbox, error = _inbox_path(context)
        if error:
            layout.label(text="Not configured", icon="ERROR")
            layout.operator("preferences.addon_show", text="Open preferences").module = __name__
            return

        column = layout.column(align=True)
        column.scale_y = 1.5
        column.operator("solforge.send", text="Send to Forge", icon="EXPORT")

        row = layout.row(align=True)
        send_selected = row.operator("solforge.send", text="Send selection")
        send_selected.use_selection = True

        box = layout.box()
        box.label(text="Drops into", icon="FILE_FOLDER")
        box.label(text=INBOX_NAME + "/")
        box.label(text="Leave the Forge running and")
        box.label(text="it imports within a second.")


def _menu_export(self, context):
    self.layout.operator("solforge.send", text="Send to Forge (SolProject)")


_classes = (
    SOLFORGE_AddonPreferences,
    SOLFORGE_OT_send,
    SOLFORGE_PT_panel,
)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)
    bpy.types.TOPBAR_MT_file_export.append(_menu_export)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(_menu_export)
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
