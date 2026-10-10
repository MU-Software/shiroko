class Canvas:
    """8-bit coverage laid out like Ghostty's sprite canvas: (0, 0) is the cell's top left and a quarter cell of
    padding surrounds it; drawing outside the padding is dropped."""

    def __init__(self, width, height):
        self.width, self.height = width, height
        self.pad_x, self.pad_y = width // 4, height // 4
        self.stride = width + 2 * self.pad_x
        self.rows = height + 2 * self.pad_y
        self.buf = bytearray(self.stride * self.rows)
        self.clip_top = self.clip_left = self.clip_right = self.clip_bottom = 0

    def clear(self):
        self.buf[:] = bytes(len(self.buf))
        self.clip_top = self.clip_left = self.clip_right = self.clip_bottom = 0

    def pixel(self, x, y, a=255):
        px, py = x + self.pad_x, y + self.pad_y
        if 0 <= px < self.stride and 0 <= py < self.rows:
            self.buf[py * self.stride + px] = a

    def rect(self, x0, y0, x1, y1, a=255):
        """Half-open [x0, x1) x [y0, y1) in cell coordinates."""
        x0, x1 = max(x0 + self.pad_x, 0), min(x1 + self.pad_x, self.stride)
        y0, y1 = max(y0 + self.pad_y, 0), min(y1 + self.pad_y, self.rows)
        if x0 < x1:
            row = bytes([a]) * (x1 - x0)
            for y in range(y0, y1):
                self.buf[y * self.stride + x0:y * self.stride + x1] = row

    def invert(self):
        self.buf[:] = bytes(255 - v for v in self.buf)

    def flip_h(self):
        s = self.stride
        for y in range(self.rows):
            self.buf[y * s:(y + 1) * s] = self.buf[y * s:(y + 1) * s][::-1]
        self.clip_left, self.clip_right = self.clip_right, self.clip_left

    def clear_clipping_regions(self):
        """Zeroes the clipped margins; clip_* count padded-canvas pixels."""
        s, r = self.stride, self.rows
        for y in range(r):
            for x in list(range(self.clip_left)) + list(range(s - self.clip_right, s)):
                self.buf[y * s + x] = 0
        for y in list(range(self.clip_top)) + list(range(r - self.clip_bottom, r)):
            self.buf[y * s:(y + 1) * s] = bytes(s)

    def padded_bytes(self):
        return bytes(self.buf)

    def cell_bytes(self):
        s, x = self.stride, self.pad_x
        return b"".join(bytes(self.buf[(y + self.pad_y) * s + x:(y + self.pad_y) * s + x + self.width])
                        for y in range(self.height))
