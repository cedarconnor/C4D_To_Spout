"""Cube-map viewport capture -> 2:1 equirect, using Blender's own viewport drawing.

Six square 90-degree `GPUOffScreen.draw_view3d` renders from the camera position are unwrapped
by a fragment shader. Anti-aliasing comes from rendering the faces above the output resolution
and averaging an n x n grid of samples per output pixel in the unwrap. (Re-rendering the faces
with a jittered projection is much slower in EEVEE: every projection change resets its per-view
state.)

Must run inside a SpaceView3D draw callback (draw_view3d needs a live draw context).

Equirect convention (matches the C4D plugin): camera forward at the centre, camera right at
3/4 width, left at 1/4, back at the seam, up at the top. Blender camera local frame:
forward = -Z, right = +X, up = +Y.
"""
import math

import gpu
import numpy as np
from gpu_extras.batch import batch_for_shader
from mathutils import Matrix

# Rotations in camera-local space; each maps local -Z onto its face direction.
# Order must match the face indices in _UNWRAP_FRAG.
_FACE_ROTATIONS = (
    Matrix.Identity(4),                            # 0 forward
    Matrix.Rotation(math.radians(-90), 4, 'Y'),    # 1 right
    Matrix.Rotation(math.radians(180), 4, 'Y'),    # 2 back
    Matrix.Rotation(math.radians(90), 4, 'Y'),     # 3 left
    Matrix.Rotation(math.radians(90), 4, 'X'),     # 4 up
    Matrix.Rotation(math.radians(-90), 4, 'X'),    # 5 down
)

_VERT = """
void main()
{
    uv = texCoord;
    gl_Position = vec4(pos, 0.0, 1.0);
}
"""

# p = face-view-space direction (the inverse face rotation applied to d).
_UNWRAP_FRAG = """
vec3 sample_dir(vec2 e)
{
    float lon = (e.x - 0.5) * 6.28318530718;
    float lat = (e.y - 0.5) * 3.14159265359;
    vec3 d = vec3(sin(lon) * cos(lat), sin(lat), -cos(lon) * cos(lat));
    vec3 a = abs(d);
    vec3 p;
    int face;
    if (a.x >= a.y && a.x >= a.z) {
        if (d.x > 0.0) { face = 1; p = vec3(d.z, d.y, -d.x); }
        else           { face = 3; p = vec3(-d.z, d.y, d.x); }
    }
    else if (a.y >= a.z) {
        if (d.y > 0.0) { face = 4; p = vec3(d.x, d.z, -d.y); }
        else           { face = 5; p = vec3(d.x, -d.z, d.y); }
    }
    else {
        if (d.z < 0.0) { face = 0; p = d; }
        else           { face = 2; p = vec3(-d.x, d.y, -d.z); }
    }
    vec2 st = (p.xy / -p.z) * 0.5 + 0.5;
    if      (face == 0) return texture(t0, st).rgb;
    else if (face == 1) return texture(t1, st).rgb;
    else if (face == 2) return texture(t2, st).rgb;
    else if (face == 3) return texture(t3, st).rgb;
    else if (face == 4) return texture(t4, st).rgb;
    return texture(t5, st).rgb;
}

void main()
{
    /* n x n grid over this output pixel's footprint. */
    int n = int(grid);
    vec2 pixel = vec2(pixel_w, pixel_h);
    vec3 sum = vec3(0.0);
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < n; i++) {
            vec2 offset = (vec2(float(i), float(j)) + 0.5) / grid - 0.5;
            sum += sample_dir(uv + offset * pixel);
        }
    }
    fragColor = vec4(sum / (grid * grid), 1.0);
}
"""


def _projection(near, far):
    """Square 90-degree perspective."""
    return Matrix((
        (1, 0, 0, 0),
        (0, 1, 0, 0),
        (0, 0, (far + near) / (near - far), 2 * far * near / (near - far)),
        (0, 0, -1, 0),
    ))


class CubeCapture:
    def __init__(self):
        self._offscreens = {}
        self._shader = None
        self._batch = None

    def free(self):
        for off in self._offscreens.values():
            off.free()
        self._offscreens.clear()

    def _offscreen(self, key, width, height):
        off = self._offscreens.get(key)
        if off is None or (off.width, off.height) != (width, height):
            if off is not None:
                off.free()
            off = gpu.types.GPUOffScreen(width, height, format='RGBA8')
            self._offscreens[key] = off
        return off

    def _unwrap_shader(self):
        if self._shader is None:
            iface = gpu.types.GPUStageInterfaceInfo('c2s_unwrap_iface')
            iface.smooth('VEC2', 'uv')
            info = gpu.types.GPUShaderCreateInfo()
            info.vertex_in(0, 'VEC2', 'pos')
            info.vertex_in(1, 'VEC2', 'texCoord')
            info.vertex_out(iface)
            for name in ('grid', 'pixel_w', 'pixel_h'):
                info.push_constant('FLOAT', name)
            for i in range(6):
                info.sampler(i, 'FLOAT_2D', f't{i}')
            info.fragment_out(0, 'VEC4', 'fragColor')
            info.vertex_source(_VERT)
            info.fragment_source(_UNWRAP_FRAG)
            self._shader = gpu.shader.create_from_info(info)
            self._batch = batch_for_shader(self._shader, 'TRI_FAN', {
                'pos': ((-1, -1), (1, -1), (1, 1), (-1, 1)),
                'texCoord': ((0, 0), (1, 0), (1, 1), (0, 1)),
            })
        return self._shader

    def capture(self, context, camera_matrix, near, far, width, face_size, grid):
        """Return the equirect as a top-down (height, width, 4) uint8 array.

        camera_matrix: camera world matrix without scale. The view is drawn with the
        shading of context.space_data and baked through the scene's view transform.
        grid: samples per output pixel along each axis (grid * grid taps).
        """
        height = width // 2
        shader = self._unwrap_shader()
        faces = [self._offscreen(f'face{i}', face_size, face_size) for i in range(6)]
        out = self._offscreen('out', width, height)
        proj = _projection(near, far)
        space, region = context.space_data, context.region

        for off, rot in zip(faces, _FACE_ROTATIONS):
            off.draw_view3d(context.scene, context.view_layer, space, region,
                            (camera_matrix @ rot).inverted(), proj,
                            do_color_management=True, draw_background=True)

        buf = gpu.types.Buffer('UBYTE', width * height * 4)
        with out.bind():
            gpu.state.depth_test_set('NONE')
            gpu.state.blend_set('NONE')
            shader.bind()
            shader.uniform_float('grid', float(grid))
            shader.uniform_float('pixel_w', 1.0 / width)
            shader.uniform_float('pixel_h', 1.0 / height)
            for i, off in enumerate(faces):
                shader.uniform_sampler(f't{i}', off.texture_color)
            with gpu.matrix.push_pop(), gpu.matrix.push_pop_projection():
                gpu.matrix.load_identity()
                gpu.matrix.load_projection_matrix(Matrix.Identity(4))
                self._batch.draw(shader)
            gpu.state.active_framebuffer_get().read_color(0, 0, width, height, 4, 0, 'UBYTE', data=buf)

        # GL rows are bottom-up; Spout textures are top-down.
        return np.flipud(np.frombuffer(buf, dtype=np.uint8).reshape(height, width, 4)).copy()
