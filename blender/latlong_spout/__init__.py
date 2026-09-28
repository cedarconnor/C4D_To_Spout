"""LatLong Spout: send the active camera as a 2:1 lat-long (equirect) image over Spout.

The image comes from Blender's viewport drawing (Solid, Material Preview or EEVEE Rendered,
whatever the source 3D view shows), captured as a cube map from the camera position and
unwrapped on the GPU. See capture.py.

Flow: depsgraph/frame/setting changes mark the scene dirty -> a timer tags the source 3D view
for redraw (rate-limited) -> the view's POST_PIXEL draw handler captures and hands the pixels
to the Spout worker thread.
"""
import time

import bpy
import gpu
from bpy.props import BoolProperty, FloatProperty, IntProperty, PointerProperty, StringProperty
from mathutils import Matrix

from .capture import CubeCapture
from .sender import SpoutWorker

_TICK = 0.02            # seconds between timer ticks while enabled
_REDRAW_TIMEOUT = 1.0   # a requested capture with no redraw after this means no visible view


class _State:
    def __init__(self):
        self.capture = None
        self.worker = SpoutWorker()
        self.draw_handler = None
        self.dirty = True
        self.requested = 0.0        # time a capture was requested, 0 = none pending
        self.last_capture = 0.0
        self.signature = None
        self.target = 0             # as_pointer() of the source 3D view area
        self.status = 'Idle'
        self.warnings = []
        self.ms = 0.0
        self.size = (0, 0)


_state = _State()


@bpy.app.handlers.persistent
def _mark_dirty(*_args):
    _state.dirty = True


def _settings_changed(_self, _context):
    _state.dirty = True
    _tag_target()


def _enabled_changed(self, _context):
    if self.latlong_spout_enabled:
        _state.dirty = True
        _state.requested = 0.0
        if not bpy.app.timers.is_registered(_tick):
            bpy.app.timers.register(_tick, first_interval=0.0, persistent=True)
    else:
        _state.status = 'Auto update off'


class LatLongSpoutSettings(bpy.types.PropertyGroup):
    sender_name: StringProperty(
        name='Sender Name', default='Blender_LatLong', update=_settings_changed)
    width: IntProperty(
        name='Width', description='Output width in pixels; height is width / 2',
        default=2048, min=256, max=16384, step=256, update=_settings_changed)
    face_scale: FloatProperty(
        name='Face Supersampling',
        description='Cube face resolution relative to the output (1.0 = width / 4 per face)',
        default=2.0, min=0.5, max=4.0, update=_settings_changed)
    aa_grid: IntProperty(
        name='AA Samples',
        description='Samples per output pixel along each axis when unwrapping (n x n taps)',
        default=2, min=1, max=4, update=_settings_changed)
    max_rate: FloatProperty(
        name='Max Rate', description='Maximum frames per second while the scene changes',
        default=30.0, min=1.0, max=120.0, update=_settings_changed)


# ---------------------------------------------------------------------------------------------
# Source view: the largest visible 3D view.

def _find_target():
    best, best_area = None, 0
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D' and area.width * area.height > best_area:
                best, best_area = area, area.width * area.height
    return best


def _tag_target():
    area = _find_target()
    if area is not None:
        area.tag_redraw()


def _view_warnings(scene, space):
    warnings = []
    if scene.camera is None:
        warnings.append('No scene camera')
    shading = space.shading
    if shading.type == 'SOLID' and shading.light == 'STUDIO' and not shading.use_world_space_lighting:
        warnings.append('Solid: enable World Space Lighting or faces will not match')
    if shading.type == 'SOLID' and shading.light == 'MATCAP':
        warnings.append('Solid: MatCap is view-dependent; faces will not match')
    if shading.type == 'RENDERED' and scene.render.engine not in ('BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT', 'BLENDER_WORKBENCH'):
        warnings.append(f'Rendered mode with {scene.render.engine} is not supported; use EEVEE')
    if shading.type == 'WIREFRAME':
        warnings.append('Wireframe shading')
    return warnings


# ---------------------------------------------------------------------------------------------
# Timer: change detection and rate limiting.

def _tick():
    wm = bpy.context.window_manager
    if not wm.latlong_spout_enabled:
        return None
    scene = bpy.context.scene
    area = _find_target()
    settings = scene.latlong_spout
    # Changes the depsgraph does not report: view shading, source view, camera switch.
    signature = None
    if area is not None:
        sh = area.spaces.active.shading
        signature = (area.as_pointer(), sh.type, sh.light, sh.use_world_space_lighting,
                     sh.studio_light, scene.as_pointer(),
                     scene.camera.name if scene.camera else None)
    if signature != _state.signature:
        _state.signature = signature
        _state.dirty = True

    now = time.perf_counter()
    if _state.requested:
        if now - _state.requested > _REDRAW_TIMEOUT:
            _state.requested = 0.0
            _state.status = 'No visible 3D view to capture from'
        return _TICK
    if area is None:
        _state.status = 'No 3D view open'
        return _TICK
    if _state.dirty and now - _state.last_capture >= 1.0 / settings.max_rate:
        _state.target = area.as_pointer()
        _state.requested = now
        area.tag_redraw()
    return _TICK


def request_capture():
    area = _find_target()
    if area is None:
        return False
    _state.target = area.as_pointer()
    _state.requested = time.perf_counter()
    area.tag_redraw()
    return True


# ---------------------------------------------------------------------------------------------
# Draw handler: the capture itself.

def _draw():
    if not _state.requested:
        return
    context = bpy.context
    if context.area is None or context.area.as_pointer() != _state.target:
        return
    _state.requested = 0.0
    _state.dirty = False
    scene = context.scene
    settings = scene.latlong_spout
    _state.warnings = _view_warnings(scene, context.space_data)
    cam = scene.camera
    if cam is None:
        _state.status = 'Not sent: no scene camera'
        return
    try:
        t0 = time.perf_counter()
        cam_eval = cam.evaluated_get(context.evaluated_depsgraph_get())
        loc, rot, _scale = cam_eval.matrix_world.decompose()
        cam_matrix = Matrix.Translation(loc) @ rot.to_matrix().to_4x4()
        width = settings.width - settings.width % 2
        face = max(16, round(width / 4 * settings.face_scale))
        if _state.capture is None:
            _state.capture = CubeCapture()
        overlays = context.space_data.overlay.show_overlays
        context.space_data.overlay.show_overlays = False
        try:
            pixels = _state.capture.capture(context, cam_matrix, cam.data.clip_start,
                                            cam.data.clip_end, width, face, settings.aa_grid)
        finally:
            context.space_data.overlay.show_overlays = overlays
        _state.worker.submit(settings.sender_name, pixels)
        _state.ms = (time.perf_counter() - t0) * 1000.0
        _state.size = (width, width // 2)
        _state.status = f'Sent {width}x{width // 2} in {_state.ms:.0f} ms'
    except Exception as ex:  # never let a failure break viewport drawing
        _state.status = f'Capture failed: {ex}'
    finally:
        _state.last_capture = time.perf_counter()
    if _state.worker.error:
        _state.status = _state.worker.error


# ---------------------------------------------------------------------------------------------
# Handlers that mark the scene dirty.

@bpy.app.handlers.persistent
def _on_depsgraph(_scene, depsgraph):
    for update in depsgraph.updates:
        # Selection alone arrives as a flag-less Scene update; everything else counts.
        if (update.is_updated_geometry or update.is_updated_transform or update.is_updated_shading
                or not isinstance(update.id, bpy.types.Scene)):
            _state.dirty = True
            return


@bpy.app.handlers.persistent
def _on_load(_dummy):
    _state.dirty = True
    _state.signature = None
    wm = bpy.context.window_manager
    if wm.latlong_spout_enabled and not bpy.app.timers.is_registered(_tick):
        bpy.app.timers.register(_tick, first_interval=0.0, persistent=True)


# ---------------------------------------------------------------------------------------------
# UI.

class LATLONGSPOUT_OT_send_now(bpy.types.Operator):
    bl_idname = 'latlong_spout.send_now'
    bl_label = 'Send Now'
    bl_description = 'Capture the active camera as lat-long and send it over Spout'

    def execute(self, context):
        if not request_capture():
            self.report({'WARNING'}, 'No 3D view open to capture from')
            return {'CANCELLED'}
        return {'FINISHED'}


class LATLONGSPOUT_PT_panel(bpy.types.Panel):
    bl_idname = 'LATLONGSPOUT_PT_panel'
    bl_label = 'LatLong Spout'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'Spout'

    def draw(self, context):
        layout = self.layout
        settings = context.scene.latlong_spout
        row = layout.row(align=True)
        row.prop(context.window_manager, 'latlong_spout_enabled', text='Auto Update', toggle=True)
        row.operator(LATLONGSPOUT_OT_send_now.bl_idname, text='', icon='FILE_REFRESH')
        col = layout.column()
        col.use_property_split = True
        col.use_property_decorate = False
        col.prop(settings, 'sender_name', text='Sender')
        col.prop(settings, 'width')
        col.prop(settings, 'aa_grid')
        col.prop(settings, 'face_scale', text='Face Scale')
        col.prop(settings, 'max_rate', text='Max FPS')

        box = layout.box()
        area = _find_target()
        if area is not None:
            box.label(text=f'Source: {area.spaces.active.shading.type.title()} view', icon='VIEW3D')
        cam = context.scene.camera
        box.label(text=f'Camera: {cam.name}' if cam else 'Camera: none', icon='CAMERA_DATA')
        box.label(text=_state.status, icon='INFO')
        for warning in _state.warnings:
            box.label(text=warning, icon='ERROR')
        if gpu.platform.backend_type_get() != 'OPENGL':
            box.label(text=f'{gpu.platform.backend_type_get()} backend: untested', icon='ERROR')


_classes = (LatLongSpoutSettings, LATLONGSPOUT_OT_send_now, LATLONGSPOUT_PT_panel)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)
    bpy.types.Scene.latlong_spout = PointerProperty(type=LatLongSpoutSettings)
    # On the window manager so it is not saved into .blend files.
    bpy.types.WindowManager.latlong_spout_enabled = BoolProperty(
        name='Auto Update', description='Send a new frame whenever the scene changes',
        default=False, update=_enabled_changed)
    _state.draw_handler = bpy.types.SpaceView3D.draw_handler_add(_draw, (), 'WINDOW', 'POST_PIXEL')
    bpy.app.handlers.depsgraph_update_post.append(_on_depsgraph)
    bpy.app.handlers.frame_change_post.append(_mark_dirty)
    bpy.app.handlers.load_post.append(_on_load)


def unregister():
    if bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)
    for handlers, fn in ((bpy.app.handlers.depsgraph_update_post, _on_depsgraph),
                         (bpy.app.handlers.frame_change_post, _mark_dirty),
                         (bpy.app.handlers.load_post, _on_load)):
        if fn in handlers:
            handlers.remove(fn)
    if _state.draw_handler is not None:
        bpy.types.SpaceView3D.draw_handler_remove(_state.draw_handler, 'WINDOW')
        _state.draw_handler = None
    _state.worker.stop()
    if _state.capture is not None:
        _state.capture.free()
        _state.capture = None
    del bpy.types.WindowManager.latlong_spout_enabled
    del bpy.types.Scene.latlong_spout
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
