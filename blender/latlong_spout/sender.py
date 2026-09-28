"""Spout sender on a dedicated thread with its own hidden OpenGL context (SpoutGL).

Keeping Spout off Blender's draw thread means it never touches Blender's GL state and works
whichever GPU backend Blender uses. Frames are latest-wins: a frame submitted while the
previous one is still being sent replaces any frame still waiting.
"""
import threading

GL_RGBA = 0x1908


class SpoutWorker:
    def __init__(self):
        self._cv = threading.Condition()
        self._pending = None
        self._stop = False
        self._thread = None
        self.error = ''
        self.frames_sent = 0

    @property
    def running(self):
        return self._thread is not None and self._thread.is_alive()

    def submit(self, name, pixels):
        """pixels: top-down (height, width, 4) uint8, C-contiguous."""
        if not self.running:
            self._start()
        with self._cv:
            self._pending = (name, pixels)
            self._cv.notify()

    def stop(self):
        if self._thread is None:
            return
        with self._cv:
            self._stop = True
            self._cv.notify()
        self._thread.join(timeout=5.0)
        self._thread = None

    def _start(self):
        self._stop = False
        self.error = ''
        self._thread = threading.Thread(target=self._run, name='LatLongSpout', daemon=True)
        self._thread.start()

    def _run(self):
        try:
            import SpoutGL
        except ImportError as ex:
            self.error = f'SpoutGL not available: {ex}'
            return
        sender = SpoutGL.SpoutSender()
        if not sender.createOpenGL():
            self.error = 'Spout could not create its OpenGL context'
            return
        current = None
        try:
            while True:
                with self._cv:
                    while self._pending is None and not self._stop:
                        self._cv.wait()
                    if self._stop:
                        break
                    name, pixels = self._pending
                    self._pending = None
                if name != current:
                    if current is not None:
                        sender.releaseSender()
                    sender.setSenderName(name)
                    current = name
                height, width = pixels.shape[:2]
                if sender.sendImage(pixels, width, height, GL_RGBA, False, 0):
                    self.frames_sent += 1
                    self.error = ''
                else:
                    self.error = 'Spout sendImage failed'
        except Exception as ex:  # keep the thread's failure visible in the UI
            self.error = f'Spout error: {ex}'
        finally:
            sender.releaseSender()
            sender.closeOpenGL()
