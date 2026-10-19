#!/usr/bin/env python3
"""Host test of tools/native/nrr_tol.py on synthetic BMPs.

    python3 tools/native/test_nrr_tol.py
"""
import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "nrr_tol.py")
W, H = 400, 240


def write_bmp(path, px):
    """px: dict (x, y) -> (r, g, b); everything else mid grey. 24-bit bottom-up."""
    bpr = (W * 3 + 3) & ~3
    body = bytearray()
    for y in range(H - 1, -1, -1):
        row = bytearray()
        for x in range(W):
            r, g, b = px.get((x, y), (100, 100, 100))
            row += bytes((b, g, r))
        row += b"\x00" * (bpr - W * 3)
        body += row
    hdr = struct.pack("<2sIHHI", b"BM", 54 + len(body), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, W, H, 1, 24, 0, len(body), 2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(hdr + info + body)


def run(a, b, *extra):
    p = subprocess.run([sys.executable, TOOL, a, b] + list(extra), capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def shifted(points, delta):
    return {pt: (100 + delta, 100, 100) for pt in points}


class NrrTolTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.a = os.path.join(self.tmp.name, "a")
        self.b = os.path.join(self.tmp.name, "b")
        os.makedirs(self.a)
        os.makedirs(self.b)
        write_bmp(os.path.join(self.a, "f_L.bmp"), {})

    def tearDown(self):
        self.tmp.cleanup()

    def b_frame(self, px):
        write_bmp(os.path.join(self.b, "f_L.bmp"), px)

    def scattered(self, n):
        pts = [(10 + (i * 7) % 370, 10 + 2 * ((i * 7) // 370) + 2 * (i % 2)) for i in range(n)]
        return [(x, y) for x, y in pts]

    def test_identical(self):
        self.b_frame({})
        rc, out = run(self.a, self.b)
        self.assertEqual(rc, 0, out)
        self.assertIn("identical=1", out)

    def test_3d_count(self):
        pts = [(10 + 2 * (i % 180), 10 + 2 * (i // 180)) for i in range(400)]
        self.b_frame(shifted(pts, 30))
        rc, out = run(self.a, self.b, "--class", "3d")
        self.assertEqual(rc, 0, out)
        pts = [(10 + 2 * (i % 180), 10 + 2 * (i // 180)) for i in range(500)]
        self.b_frame(shifted(pts, 30))
        rc, out = run(self.a, self.b, "--class", "3d")
        self.assertEqual(rc, 1, out)
        self.assertIn("hot 500 > 480", out)

    def test_3d_blob_and_edge(self):
        blob = [(100 + x, 100 + y) for x in range(10) for y in range(10)]
        self.b_frame(shifted(blob, 40))
        rc, out = run(self.a, self.b, "--class", "3d")
        self.assertEqual(rc, 1, out)
        self.assertIn("blob 100 px > 64", out)
        edge = [(2 + x, 100 + y) for x in range(10) for y in range(10)]
        self.b_frame(shifted(edge, 40))
        rc, out = run(self.a, self.b, "--class", "3d")
        self.assertEqual(rc, 0, out)

    def test_3d_mean(self):
        everything = [(x, y) for x in range(W) for y in range(H)]
        self.b_frame(shifted(everything, 1))
        rc, out = run(self.a, self.b, "--class", "3d")
        self.assertEqual(rc, 1, out)
        self.assertIn("mean 1.000 > 0.5", out)

    def test_2d_rules(self):
        pts = [(10 + 3 * i, 50) for i in range(40)]
        self.b_frame(shifted(pts, 9))
        rc, out = run(self.a, self.b, "--class", "2d")
        self.assertEqual(rc, 0, out)
        pts = [(10 + 3 * i, 50) for i in range(49)]
        self.b_frame(shifted(pts, 9))
        rc, out = run(self.a, self.b, "--class", "2d")
        self.assertEqual(rc, 1, out)
        blob = [(200 + x, 100 + y) for x in range(3) for y in range(3)]
        self.b_frame(shifted(blob, 9))
        rc, out = run(self.a, self.b, "--class", "2d")
        self.assertEqual(rc, 1, out)
        self.assertIn("blob 9 px > 8", out)

    def test_2d_outside_hud_rects(self):
        mask = os.path.join(self.tmp.name, "hud.mask")
        with open(mask, "w") as f:
            f.write("mask 400 240\nrect 0 0 100 40 hud\n")
        self.b_frame(shifted([(10, 10)], 20))
        rc, out = run(self.a, self.b, "--class", "2d", "--hud-mask", mask)
        self.assertEqual(rc, 0, out)
        self.b_frame(shifted([(200, 200)], 20))
        rc, out = run(self.a, self.b, "--class", "2d", "--hud-mask", mask)
        self.assertEqual(rc, 1, out)
        self.assertIn("outside the HUD rects", out)

    def test_split(self):
        mask = os.path.join(self.tmp.name, "hud.mask")
        with open(mask, "w") as f:
            f.write("mask 400 240\nrect 0 0 100 40 hud\n")
        outside = [(200 + x, 100 + y) for x in range(3) for y in range(3)]
        self.b_frame(shifted(outside, 30))
        rc, out = run(self.a, self.b, "--class", "split", "--hud-mask", mask)
        self.assertEqual(rc, 0, out)
        inside = [(20 + x, 10 + y) for x in range(3) for y in range(3)]
        self.b_frame(shifted(inside, 30))
        rc, out = run(self.a, self.b, "--class", "split", "--hud-mask", mask)
        self.assertEqual(rc, 1, out)
        self.assertIn("2d: blob 9 px > 8", out)

    def test_missing_and_sheet(self):
        self.b_frame(shifted([(50, 50)], 30))
        write_bmp(os.path.join(self.a, "g_L.bmp"), {})
        sheets = os.path.join(self.tmp.name, "sheets")
        rc, out = run(self.a, self.b, "--sheets", sheets)
        self.assertEqual(rc, 1, out)
        self.assertIn("g_L.bmp MISSING in B", out)
        self.assertTrue(os.path.exists(os.path.join(sheets, "f_L.png")))


if __name__ == "__main__":
    unittest.main()
