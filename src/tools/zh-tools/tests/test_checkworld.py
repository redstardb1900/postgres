"""Tests for zhtools.checkworld: check-world log sections, pg_regress / prove results, warnings."""
import os
import shutil
import tempfile
import unittest

from . import _path  # noqa: F401
from zhtools import checkworld

B = "/b"
S = "/s"

LOG = """make: Entering directory '/b'
make[1]: Entering directory '/b/src/test/regress'
# +++ regress check in src/test/regress +++
# parallel group (2 tests):  boolean char
ok 1         + boolean                                   45 ms
not ok 2     + char                                      30 ms
ok 3         - tablespace                                12 ms
ok 4         - skipme                                     0 ms # SKIP no locale
# 1 of 4 tests failed.
make[1]: *** [GNUmakefile:118: check] Error 1
make[1]: Leaving directory '/b/src/test/regress'
make[1]: Entering directory '/b/src/bin/pg_ctl'
# +++ tap check in src/bin/pg_ctl +++
t/001_start_stop.pl .. ok
t/002_status.pl ...... skipped: needs something
t/003_promote.pl ..... Dubious, test returned 1 (wstat 256, 0x100)
Failed 1/3 subtests
t/004_logrotate.pl ... 
Dubious, test returned 29 (wstat 7424, 0x1d00)
No subtests run 
Files=4, Tests=20, 12 wallclock secs
Result: FAIL
make[1]: Leaving directory '/b/src/bin/pg_ctl'
make[1]: Entering directory '/b/contrib/btree_gin'
# +++ regress check in contrib/btree_gin +++
ok 1         - install_btree_gin                          3 ms
# All 1 tests passed.
make[1]: Leaving directory '/b/contrib/btree_gin'
make[1]: Entering directory '/b/src/test/isolation'
# +++ isolation check in src/test/isolation +++
ok 1         - read-only-anomaly                        100 ms
# All 1 tests passed.
make[1]: Leaving directory '/b/src/test/isolation'
make[1]: Entering directory '/b/src/interfaces/libpq'
# +++ tap check in src/interfaces/libpq +++
t/001_uri.pl .. ok
Files=1, Tests=5, 1 wallclock secs
Result: PASS
make[1]: Leaving directory '/b/src/interfaces/libpq'
make[1]: Entering directory '/b/src/interfaces/ecpg/test'
./pg_regress --dbname=ecpg1_regression --schedule=/s/src/interfaces/ecpg/test/ecpg_schedule
ok 1         - compat_informix/dec_test                   31 ms
ok 2         - sql/twophase                               10 ms
# All 2 tests passed.
make[1]: Leaving directory '/b/src/interfaces/ecpg/test'
/s/src/backend/foo.c:12:3: warning: unused variable 'x' [-Wunused-variable]
make[2]: warning:  Clock skew detected.  Your build may be incomplete.
make: Leaving directory '/b'
"""


class CheckWorldParseTest(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.log = os.path.join(self.d, "cw.log")
        self.bld = os.path.join(self.d, "b")
        with open(self.log, "w") as f:
            f.write(LOG.replace("directory '/b", "directory '" + self.bld))
        os.makedirs(os.path.join(self.bld, "src/test/regress"))
        with open(os.path.join(self.bld, "src/test/regress/regression.diffs"), "w") as f:
            f.write("diff\n")
        with open(os.path.join(self.bld, "src/test/regress/regression.out"), "w") as f:
            f.write("out\n")
        os.makedirs(os.path.join(self.bld, "src/bin/pg_ctl/tmp_check/log"))
        with open(os.path.join(self.bld, "src/bin/pg_ctl/tmp_check/log/regress_log_001_start_stop"), "w") as f:
            f.write("x")
        os.makedirs(os.path.join(self.bld, "tmp_install/x"))
        with open(os.path.join(self.bld, "tmp_install/x/regression.diffs"), "w") as f:
            f.write("ignored\n")

    def tearDown(self):
        shutil.rmtree(self.d)

    def test_parse(self):
        r = checkworld.parse(self.log, self.bld, S)
        s = r["summary"]
        self.assertEqual(s["sections"], 6)
        self.assertEqual(s["sections_without_anchor"], 1)
        self.assertEqual(s["regress_suites"], 4)
        self.assertEqual(s["regress_tests"], 8)
        self.assertEqual(s["regress_failed_tests"], 1)
        self.assertEqual(s["regress_skipped_tests"], 1)
        self.assertEqual(s["regress_summary_failed"], 1)
        self.assertEqual(s["regress_suites_without_summary"], 0)
        self.assertEqual(s["tap_suites"], 2)
        self.assertEqual(s["tap_files"], 5)
        self.assertEqual(s["tap_failed_files"], 2)
        self.assertEqual(s["tap_unknown_files"], 0)
        self.assertEqual(s["tap_skipped_files"], 1)
        self.assertEqual(s["tap_results"], {"PASS": 1, "FAIL": 1, "NOTESTS": 0})
        # the anchor-less ecpg pg_regress run is its own section, not folded into libpq's TAP section
        libpq = r["sections"][4]
        ecpg = r["sections"][5]
        self.assertEqual(libpq["dir"], "src/interfaces/libpq")
        self.assertTrue(libpq["anchor"])
        self.assertEqual([t["name"] for t in libpq["tests"]], ["t/001_uri.pl"])
        self.assertEqual(ecpg["dir"], "src/interfaces/ecpg/test")
        self.assertEqual(ecpg["kind"], "regress")
        self.assertFalse(ecpg["anchor"])
        self.assertEqual(len(ecpg["tests"]), 2)
        self.assertEqual(ecpg["summary"], {"total": 2, "failed": 0, "ignored": 0})
        self.assertEqual(s["compiler_warnings"], 1)
        self.assertEqual(s["make_warnings"], 1)
        self.assertEqual(s["make_errors"], 1)
        self.assertEqual(s["regression_diffs_files"], 1)  # tmp_install ignored
        self.assertEqual(s["regression_out_files"], 1)
        self.assertEqual(s["tap_log_dirs"], 1)
        regress = r["sections"][0]
        self.assertEqual(regress["dir"], "src/test/regress")
        self.assertEqual(regress["cwd"], "src/test/regress")
        self.assertEqual([t["status"] for t in regress["tests"]], ["ok", "failed", "ok", "skipped"])
        self.assertEqual(regress["summary"], {"total": 4, "failed": 1, "ignored": 0})
        tap = r["sections"][1]
        self.assertEqual(tap["prove"], {"files": 4, "tests": 20, "result": "FAIL"})
        self.assertEqual(tap["tests"][2]["status"], "failed")
        self.assertIn("Failed 1/3 subtests", tap["tests"][2]["detail"])
        # progress line without status + standalone "Dubious"/"No subtests run" lines
        self.assertEqual(tap["tests"][3]["name"], "t/004_logrotate.pl")
        self.assertEqual(tap["tests"][3]["status"], "failed")
        self.assertIn("Dubious, test returned 29", tap["tests"][3]["detail"])
        self.assertIn("No subtests run", tap["tests"][3]["detail"])
        self.assertIn("SRC/src/backend/foo.c:12:3: warning: unused variable 'x' [-Wunused-variable]", r["warnings"])


if __name__ == "__main__":
    unittest.main()
