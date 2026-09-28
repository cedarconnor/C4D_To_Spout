"""Spike: 6-face viewport cube capture -> equirect, entirely on Blender's viewport drawing.

Run inside Blender (e.g. via the MCP bridge): exec this file, then call
`request(...)`. The capture runs in a SpaceView3D POST_PIXEL draw handler (draw_view3d
needs a live draw context), stores timings/images in bpy.app.driver_namespace['c2s_spike'].

Equirect convention (matches the C4D plugin): camera forward at the centre, camera right
at 3/4 width, left at 1/4, back at the seam, up at the top.
Blender camera local frame: forward = -Z, right = +X, up = +Y.
"""
import math
import time

import bpy
import gpu
import numpy as np
from gpu_extras.batch import batch_for_shader
from mathutils import Matrix

NS = bpy.app.driver_namespace.setdefault('c2s_spike', {})

# Face rotations in camera-local space (each maps local -Z onto the face direction).
FACES = {
    'F': Matrix.Identity(4),
    'R': Matrix.Rotation(math.radians(-90), 4, 'Y'),
    'B': Matrix.Rotation(math.radians(180), 4, 'Y'),
    'L': Matrix.Rotation(math.radians(90), 4, 'Y'),
    'U': Matrix.Rotation(math.radians(90), 4, 'X'),
    'D': Matrix.Rotation(math.radians(-90), 4, 'X'),
}
FACE_ORDER = ['F', 'R', 'B', 'L', 'U', 'D']

FRAG = """
void main()
{
    float lon = (uv.x - 0.5) * 6.28318530718;
    float lat = (uv.y - 0.5) * 3.14159265359;
    vec3 d = vec3(sin(lon) * cos(lat), sin(lat), -cos(lon) * cos(lat));
    vec3 a = abs(d);
    vec3 p;
    int face;
    if (a.x >= a.y && a.x >= a.z) {
        if (d.x > 0.0) { face = 1; p = vec3(d.z, d.y, -d.x); }   /* R */
        else           { face = 3; p = vec3(-d.z, d.y, d.x); }   /* L */
    }
    else if (a.y >= a.z) {
        if (d.y > 0.0) { face = 4; p = vec3(d.x, d.z, -d.y); }   /* U */
        else           { face = 5; p = vec3(d.x, -d.z, d.y); }   /* D */
    }
    else {
        if (d.z < 0.0) { face = 0; p = d; }                      /* F */
        else           { face = 2; p = vec3(-d.x, d.y, -d.z); }  /* B */
    }
    vec2 st = (p.xy / -p.z) / tan_half * 0.5 + 0.5;
    vec4 c;
    if      (face == 0) c = texture(t0, st);
    else if (face == 1) c = texture(t1, st);
    else if (face == 2) c = texture(t2, st);
    else if (face == 3) c = texture(t3, st);
    else if (face == 4) c = texture(t4, st);
    else                c = texture(t5, st);
    fragColor = vec4(c.rgb, 1.0);
}
"""

VERT = """
void main()
{
    uv = texCoord;
    gl_Position = vec4(pos, 0.0, 1.0);
}
"""


def _shader():
    if 'shader' in NS:
        return NS['shader'], NS['batch']
    iface = gpu.types.GPUStageInterfaceInfo('c2s_iface')
    iface.smooth('VEC2', 'uv')
    info = gpu.types.GPUShaderCreateInfo()
    info.vertex_in(0, 'VEC2', 'pos')
    info.vertex_in(1, 'VEC2', 'texCoord')
    info.vertex_out(iface)
    info.push_constant('FLOAT', 'tan_half')
    for i in range(6):
        info.sampler(i, 'FLOAT_2D', f't{i}')
    info.fragment_out(0, 'VEC4', 'fragColor')
    info.vertex_source(VERT)
    info.fragment_source(FRAG)
    sh = gpu.shader.create_from_info(info)
    batch = batch_for_shader(sh, 'TRI_FAN', {
        'pos': ((-1, -1), (1, -1), (1, 1), (-1, 1)),
        'texCoord': ((0, 0), (1, 0), (1, 1), (0, 1)),
    })
    NS['shader'], NS['batch'] = sh, batch
    return sh, batch


def _projection(fov, near, far):
    f = 1.0 / math.tan(fov * 0.5)
    return Matrix((
        (f, 0, 0, 0),
        (0, f, 0, 0),
        (0, 0, (far + near) / (near - far), 2 * far * near / (near - far)),
        (0, 0, -1, 0),
    ))


def _offscreen(key, w, h):
    off = NS.get(key)
    if off is None or off.width != w or off.height != h:
        if off is not None:
            off.free()
        off = gpu.types.GPUOffScreen(w, h, format='RGBA8')
        NS[key] = off
    return off


def capture(context, width, face_size, overscan_deg, hide_overlays, save_faces):
    scn = context.scene
    cam = scn.camera
    space, region = context.space_data, context.region
    t0 = time.perf_counter()

    fov = math.radians(90.0 + overscan_deg)
    proj = _projection(fov, cam.data.clip_start, cam.data.clip_end)
    loc, rot, _ = cam.matrix_world.decompose()
    cam_m = Matrix.Translation(loc) @ rot.to_matrix().to_4x4()   # drop scale

    overlays = space.overlay.show_overlays
    if hide_overlays:
        space.overlay.show_overlays = False
    faces = []
    try:
        for i, key in enumerate(FACE_ORDER):
            off = _offscreen(f'face{i}', face_size, face_size)
            view = (cam_m @ FACES[key]).inverted()
            off.draw_view3d(scn, context.view_layer, space, region, view, proj,
                            do_color_management=True, draw_background=True)
            faces.append(off)
    finally:
        space.overlay.show_overlays = overlays
    t_faces = time.perf_counter()

    h = width // 2
    out = _offscreen('equirect', width, h)
    sh, batch = _shader()
    with out.bind():
        fb = gpu.state.active_framebuffer_get()
        fb.clear(color=(0.0, 0.0, 0.0, 1.0))
        with gpu.matrix.push_pop():
            gpu.matrix.load_identity()
            with gpu.matrix.push_pop_projection():
                gpu.matrix.load_projection_matrix(Matrix.Identity(4))
                gpu.state.depth_test_set('NONE')
                gpu.state.blend_set('NONE')
                sh.bind()
                sh.uniform_float('tan_half', math.tan(fov * 0.5))
                for i, off in enumerate(faces):
                    sh.uniform_sampler(f't{i}', off.texture_color)
                batch.draw(sh)
        buf = gpu.types.Buffer('UBYTE', width * h * 4)
        fb.read_color(0, 0, width, h, 4, 0, 'UBYTE', data=buf)
    t_unwrap = time.perf_counter()
    pixels = np.frombuffer(buf, dtype=np.uint8).reshape(h, width, 4).copy()
    t_numpy = time.perf_counter()

    res = {
        'width': width, 'height': h, 'face_size': face_size, 'overscan': overscan_deg,
        'shading': space.shading.type,
        'ms_faces': (t_faces - t0) * 1000,
        'ms_unwrap_readback': (t_unwrap - t_faces) * 1000,
        'ms_numpy': (t_numpy - t_unwrap) * 1000,
        'ms_total': (t_numpy - t0) * 1000,
    }
    NS['pixels'] = pixels
    if save_faces:
        fpx = []
        for off in faces:
            fbuf = gpu.types.Buffer('UBYTE', face_size * face_size * 4)
            with off.bind():
                gpu.state.active_framebuffer_get().read_color(
                    0, 0, face_size, face_size, 4, 0, 'UBYTE', data=fbuf)
            fpx.append(np.frombuffer(fbuf, dtype=np.uint8).reshape(face_size, face_size, 4).copy())
        NS['faces'] = fpx
    return res


def _draw_handler():
    req = NS.get('request')
    if not req:
        return
    NS['request'] = None
    ctx = bpy.context
    try:
        runs = []
        for _ in range(req.get('repeat', 1)):
            runs.append(capture(ctx, req['width'], req['face_size'], req['overscan'],
                                req['hide_overlays'], req['save_faces']))
        NS['result'] = runs
    except Exception as ex:  # surface errors to the MCP caller
        import traceback
        NS['result'] = {'error': traceback.format_exc()}


def install():
    old = NS.get('handler')
    if old is not None:
        bpy.types.SpaceView3D.draw_handler_remove(old, 'WINDOW')
    NS['handler'] = bpy.types.SpaceView3D.draw_handler_add(_draw_handler, (), 'WINDOW', 'POST_PIXEL')


def request(width=2048, face_size=768, overscan=0.0, hide_overlays=True, save_faces=False, repeat=1):
    NS['result'] = None
    NS['request'] = dict(width=width, face_size=face_size, overscan=overscan,
                         hide_overlays=hide_overlays, save_faces=save_faces, repeat=repeat)
    for w in bpy.context.window_manager.windows:
        for a in w.screen.areas:
            if a.type == 'VIEW_3D':
                a.tag_redraw()


def save_png(path, pixels):
    """Write bottom-up RGBA8 rows as a PNG, bytes untouched (no colour management)."""
    import struct
    import zlib
    h, w, _ = pixels.shape
    rows = np.flipud(pixels)
    raw = np.hstack([np.zeros((h, 1), np.uint8), rows.reshape(h, w * 4)]).tobytes()

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw, 6)))
        f.write(chunk(b'IEND', b''))
