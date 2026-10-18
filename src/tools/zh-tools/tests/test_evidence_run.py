"""Tests for evidence_run.py: the frozen-environment short-output helpers.

parse_mount_fstype() is what the macOS/BSD filesystem probe relies on (BSD
``stat`` has no ``-c`` and does not print a filesystem type), so both the BSD
and the Linux ``mount`` line shapes are pinned here.
"""
import tempfile
import unittest

from . import _path  # noqa: F401
from zhtools import evidence_run


BSD_MOUNT = (
    "/dev/disk3s1 on / (apfs, local, read-only, journaled)\n"
    "/dev/disk3s5 on /System/Volumes/Data (apfs, local, journaled)\n"
    "/dev/disk1s2 on /Volumes/Ext (hfs, local, nodev, nosuid, journaled)\n"
)

GNU_MOUNT = (
    "/dev/sda1 on / type ext4 (rw,relatime)\n"
    "tmpfs on /tmp type tmpfs (rw,nosuid,nodev)\n"
)


class ParseMountFstypeTest(unittest.TestCase):
    def test_bsd_longest_mount_point_wins(self):
        self.assertEqual(evidence_run.parse_mount_fstype(BSD_MOUNT, "/System/Volumes/Data/x"), "apfs")
        self.assertEqual(evidence_run.parse_mount_fstype(BSD_MOUNT, "/Volumes/Ext"), "hfs")
        self.assertEqual(evidence_run.parse_mount_fstype(BSD_MOUNT, "/usr/local"), "apfs")

    def test_gnu_mount_line_shape(self):
        self.assertEqual(evidence_run.parse_mount_fstype(GNU_MOUNT, "/etc"), "ext4")
        self.assertEqual(evidence_run.parse_mount_fstype(GNU_MOUNT, "/tmp/x"), "tmpfs")

    def test_no_match_and_garbage_lines(self):
        self.assertEqual(evidence_run.parse_mount_fstype("", "/x"), "")
        self.assertEqual(evidence_run.parse_mount_fstype("just some text\n", "/x"), "")
        # "/data" must not match "/database" (whole path components only)
        table = "/dev/sda1 on /data type ext4 (rw)\n"
        self.assertEqual(evidence_run.parse_mount_fstype(table, "/database"), "")
        self.assertEqual(evidence_run.parse_mount_fstype(table, "/data/x"), "ext4")

    def test_probe_current_host_returns_a_type(self):
        # Whatever the host, / and the temp dir sit on a real filesystem.
        self.assertTrue(evidence_run.fstype("/"))
        with tempfile.TemporaryDirectory() as d:
            self.assertTrue(evidence_run.fstype(d))


if __name__ == "__main__":
    unittest.main()
