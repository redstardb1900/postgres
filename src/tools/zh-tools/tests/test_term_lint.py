import json
import os
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True

from tests._path import TOOLS_DIR  # noqa: E402
from zhtools import termlint  # noqa: E402

PHRASEBOOK = (
    "id\ten_pattern\tzh_required\tzh_forbidden\tscope\tstatus\tnote\n"
    "P01\t\\bcould not\\b\t无法\t不能|未能|没能\tall\tdecided\tcould not → 无法\n"
    "P02\t\\bcannot\\b\t无法\t不能|不可以|不可\\b\tall\tdecided\t\n"
    "P90\t\\bfoo\\b\t甲\t乙\tall\tdraft\tnot decided -> ignored\n"
    "P91\t\\bbar\\b\t丙\t\tall\tdecided\tno forbidden column -> ignored\n"
)
GLOSSARY = "English\t中文\nWAL\tWAL\ncluster\t数据库集簇\nLSN\tLSN\n"


def rules_fixture(phrasebook_severity=termlint.WARNING):
    with open(os.path.join(TOOLS_DIR, "term_rules.json"), encoding="utf-8") as f:
        tr, pt = termlint.load_term_rules(json.load(f))
    rules = tr + termlint.load_phrasebook(PHRASEBOOK, severity=phrasebook_severity)
    rules += termlint.preserved_term_rules(pt + termlint.load_glossary_preserved(GLOSSARY))
    return rules


def ids(hits):
    return sorted(h.rule for h in hits)


def errors(hits):
    return sorted(h.rule for h in hits if h.severity == termlint.ERROR)


def warnings(hits):
    return sorted(h.rule for h in hits if h.severity == termlint.WARNING)


class RuleLoading(unittest.TestCase):
    def test_phrasebook_only_decided_with_forbidden(self):
        rules = termlint.load_phrasebook(PHRASEBOOK)
        self.assertEqual([r.id for r in rules], ["P01", "P02"])

    def test_glossary_preserved(self):
        self.assertEqual(termlint.load_glossary_preserved(GLOSSARY), ["WAL", "LSN"])

    def test_term_rules_file_is_valid(self):
        rules = rules_fixture()
        self.assertTrue(any(r.id == "TL01" for r in rules))
        self.assertTrue(any(r.id == "PT_WAL" for r in rules))
        with open(os.path.join(TOOLS_DIR, "term_rules.json"), encoding="utf-8") as f:
            raw = json.load(f)
        for r in raw["rules"]:
            self.assertIn(r["severity"], termlint.SEVERITIES, r["id"])
        # co-occurrence-prone rules must not be errors
        sev = {r["id"]: r["severity"] for r in raw["rules"]}
        for rid in ("TL08", "TL02", "TL03", "TL10", "TL12"):
            self.assertEqual(sev[rid], "warning", rid)

    def test_severity_is_mandatory_in_term_rules(self):
        with self.assertRaises(ValueError):
            termlint.load_term_rules({"rules": [{"id": "X", "en": "a", "zh_forbidden": "b"}]})
        with self.assertRaises(ValueError):
            termlint.load_term_rules({"rules": [{"id": "X", "en": "a", "zh_forbidden": "b", "severity": "fatal"}]})

    def test_phrasebook_severity_parameter(self):
        self.assertEqual({r.severity for r in termlint.load_phrasebook(PHRASEBOOK)}, {termlint.ERROR})
        self.assertEqual({r.severity for r in termlint.load_phrasebook(PHRASEBOOK, severity=termlint.WARNING)},
                         {termlint.WARNING})


class PairLinting(unittest.TestCase):
    def setUp(self):
        self.rules = rules_fixture()

    def test_no_hit_for_good_translation(self):
        hits = termlint.lint_pair(self.rules, "could not open WAL file for the cluster",
                                  "无法为数据库集簇打开 WAL 文件")
        self.assertEqual(hits, [])

    def test_unchanged_text_is_not_judged(self):
        self.assertEqual(termlint.lint_pair(self.rules, "the cluster 集群", "the cluster 集群"), [])

    def test_explicit_hits(self):
        hits = termlint.lint_pair(self.rules, "could not open WAL file for the cluster",
                                  "不能为集群打开日志文件")
        self.assertEqual(ids(hits), ["P01", "PT_WAL", "TL01"])
        # phrasebook hit is a warning for comment input, the others are errors
        self.assertEqual(errors(hits), ["PT_WAL", "TL01"])
        self.assertEqual(warnings(hits), ["P01"])
        # ... and an error when the phrasebook is applied to PO messages
        hits_po = termlint.lint_pair(rules_fixture(termlint.ERROR), "could not open file", "不能打开文件")
        self.assertEqual(errors(hits_po), ["P01"])

    def test_review_repro_requires_requirement_is_not_an_error(self):
        # review finding #3: 'requires' correctly rendered 需要, 'requirement'
        # rendered 要求 in the same segment must not fail the run
        en = "A primary key requires a lock; this is a requirement."
        zh = "主键需要一个锁；这是一个要求。"
        hits = termlint.lint_pair(self.rules, en, zh)
        self.assertEqual(errors(hits), [])
        self.assertEqual(warnings(hits), ["TL08"])

    def test_cooccurrence_phrasebook_is_only_a_warning_for_comments(self):
        en = "could not open the file; the caller must not retry."
        zh = "无法打开该文件；调用者不能重试。"
        hits = termlint.lint_pair(self.rules, en, zh)
        self.assertEqual(errors(hits), [])
        self.assertEqual(warnings(hits), ["P01"])

    def test_forbidden_without_english_trigger_is_not_reported(self):
        # 集群 appears but the English never said "cluster": not an explicit hit
        hits = termlint.lint_pair(self.rules, "the nodes form a group", "这些节点组成集群")
        self.assertEqual(ids(hits), [])

    def test_bare_cu_rule(self):
        self.assertEqual(warnings(termlint.lint_pair(self.rules, "cluster", "簇")), ["TL02"])
        self.assertEqual(ids(termlint.lint_pair(self.rules, "cluster", "集簇")), [])
        self.assertEqual(ids(termlint.lint_pair(self.rules, "CLUSTER the table", "对表执行聚簇")), [])

    def test_quotes_rule(self):
        self.assertEqual(ids(termlint.lint_pair(self.rules, 'relation "%s"', "关系“%s”")), ["TL11"])
        self.assertEqual(ids(termlint.lint_pair(self.rules, 'relation "%s"', '关系 "%s"')), [])

    def test_preserved_term_word_boundary(self):
        self.assertEqual(ids(termlint.lint_pair(self.rules, "LSN pointer", "日志序列号指针")), ["PT_LSN"])
        # WALInsertLock is not the term WAL
        self.assertEqual(ids(termlint.lint_pair(self.rules, "take WALInsertLock", "获取 WALInsertLock")), [])

    def test_allowlist(self):
        hits = termlint.lint_pair(self.rules, "a cluster of nodes", "多节点集群", path="src/x.c")
        self.assertEqual(ids(hits), ["TL01"])
        allow = termlint.Allowlist([{"rule": "TL01", "path": "src/**", "pattern": "cluster of nodes",
                                     "reason": "distributed multi-node context"}])
        self.assertTrue(allow.allows(hits[0]))
        other = termlint.Allowlist([{"rule": "TL01", "path": "contrib/**", "reason": "x"}])
        self.assertFalse(other.allows(hits[0]))
        with self.assertRaises(ValueError):
            termlint.Allowlist([{"rule": "TL01", "path": "src/**"}])  # no reason


class PoParsing(unittest.TestCase):
    def test_parse(self):
        po = ('msgid ""\nmsgstr ""\n"Language: zh_CN\\n"\n\n'
              '#: a.c:1\nmsgid "could not open file \\"%s\\""\nmsgstr "不能打开文件 \\"%s\\""\n\n'
              'msgid "one"\nmsgid_plural "many"\nmsgstr[0] "一"\n\n'
              '#~ msgid "old"\n#~ msgstr "旧"\n\n'
              'msgid "multi"\n"line"\nmsgstr "多"\n"行"\n')
        entries = termlint.parse_po(po)
        self.assertEqual([(a, b) for a, b, _ in entries],
                         [('could not open file "%s"', '不能打开文件 "%s"'), ("many", "一"), ("multiline", "多行")])


class Cli(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="zhtools-tl-")
        self.addCleanup(self.tmp.cleanup)
        self.pgnls = os.path.join(self.tmp.name, "pgnls")
        os.makedirs(os.path.join(self.pgnls, "ref"))
        with open(os.path.join(self.pgnls, "ref", "phrasebook.tsv"), "w", encoding="utf-8") as f:
            f.write(PHRASEBOOK)
        with open(os.path.join(self.pgnls, "ref", "glossary.tsv"), "w", encoding="utf-8") as f:
            f.write(GLOSSARY)
        env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@x", GIT_COMMITTER_NAME="t",
                   GIT_COMMITTER_EMAIL="t@x", GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_NOSYSTEM="1")
        for cmd in (["init", "-q"], ["add", "-A"], ["commit", "-q", "-m", "x"]):
            subprocess.run(["git"] + cmd, cwd=self.pgnls, env=env, check=True)

    def run_cli(self, *argv):
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        p = subprocess.run([sys.executable, os.path.join(TOOLS_DIR, "term_lint.py")] + list(argv), env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")

    def test_c_pair_and_json(self):
        old = os.path.join(self.tmp.name, "o.c")
        new = os.path.join(self.tmp.name, "n.c")
        with open(old, "w", encoding="utf-8") as f:
            f.write("/* could not read the cluster */\nint a; /* WAL */\n")
        with open(new, "w", encoding="utf-8") as f:
            f.write("/* 不能读取集群 */\nint a; /* 预写日志 */\n")
        rep = os.path.join(self.tmp.name, "r.json")
        rc, out, err = self.run_cli("--c-pair", old, new, "--pgnls", self.pgnls, "--no-check-pgnls-sha",
                                    "--json", rep)
        self.assertEqual(rc, 1, out + err)
        with open(rep, encoding="utf-8") as f:
            r = json.load(f)
        self.assertEqual(sorted(h["rule"] for h in r["hits"]), ["P01", "PT_WAL", "TL01"])
        self.assertEqual({h["rule"]: h["severity"] for h in r["hits"]},
                         {"P01": "warning", "PT_WAL": "error", "TL01": "error"})
        self.assertEqual(r["n_errors"], 2)
        self.assertEqual(r["n_warnings"], 1)
        self.assertEqual([h["line"] for h in r["hits"] if h["rule"] == "PT_WAL"], [2])
        # --phrasebook-severity error turns P01 into an error for comments
        rc, out, _ = self.run_cli("--c-pair", old, new, "--pgnls", self.pgnls, "--no-check-pgnls-sha",
                                  "--phrasebook-severity", "error", "--json", rep)
        with open(rep, encoding="utf-8") as f:
            self.assertEqual(json.load(f)["n_errors"], 3)
        # allowlist suppresses
        al = os.path.join(self.tmp.name, "allow.json")
        with open(al, "w", encoding="utf-8") as f:
            json.dump([{"rule": "TL01", "path": "**", "reason": "test"},
                       {"rule": "P01", "path": "**", "reason": "test"},
                       {"rule": "PT_WAL", "path": "**", "reason": "test"}], f)
        rc, out, _ = self.run_cli("--c-pair", old, new, "--pgnls", self.pgnls, "--no-check-pgnls-sha",
                                  "--allowlist", al)
        self.assertEqual(rc, 0, out)
        self.assertIn("3 allowlisted", out)

    def test_warning_only_hits_exit_zero(self):
        old = os.path.join(self.tmp.name, "o.c")
        new = os.path.join(self.tmp.name, "n.c")
        with open(old, "w", encoding="utf-8") as f:
            f.write("/* A primary key requires a lock; this is a requirement. */\n")
        with open(new, "w", encoding="utf-8") as f:
            f.write("/* 主键需要一个锁；这是一个要求。 */\n")
        rc, out, err = self.run_cli("--c-pair", old, new, "--pgnls", self.pgnls, "--no-check-pgnls-sha")
        self.assertEqual(rc, 0, out + err)
        self.assertIn("WARNING TL08", out)
        self.assertIn("0 error hits, 1 warnings", out)

    def test_pgnls_sha_mismatch_is_unverified(self):
        old = os.path.join(self.tmp.name, "o.c")
        with open(old, "w") as f:
            f.write("/* x */\n")
        rc, _, err = self.run_cli("--c-pair", old, old, "--pgnls", self.pgnls)
        self.assertEqual(rc, 2)
        self.assertIn("UNVERIFIED", err)

    def test_po_mode(self):
        po = os.path.join(self.tmp.name, "x.po")
        with open(po, "w", encoding="utf-8") as f:
            f.write('msgid ""\nmsgstr ""\n\nmsgid "could not open file"\nmsgstr "不能打开文件"\n')
        rc, out, _ = self.run_cli("--po", po, "--pgnls", self.pgnls, "--no-check-pgnls-sha")
        self.assertEqual(rc, 1)
        self.assertIn("ERROR P01", out)

    def test_no_pycache(self):
        for dp, dns, _ in os.walk(TOOLS_DIR):
            self.assertNotIn("__pycache__", dns)


if __name__ == "__main__":
    unittest.main()
