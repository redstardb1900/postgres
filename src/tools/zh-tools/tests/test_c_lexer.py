import sys
import unittest

sys.dont_write_bytecode = True

from tests import _path  # noqa: F401,E402  (sets sys.path)
from zhtools import c_lexer  # noqa: E402
from zhtools.c_lexer import BLOCK, LINE  # noqa: E402


def comments_of(data):
    r = c_lexer.lex(data)
    return [(c.kind, c.text) for c in r.comments]


class LexerBasics(unittest.TestCase):
    def test_block_and_line(self):
        src = b"int a; /* one */ int b; // two\nint c;\n"
        self.assertEqual(comments_of(src), [(BLOCK, b"/* one */"), (LINE, b"// two")])
        r = c_lexer.lex(src)
        self.assertEqual(r.skeleton, b"int a; \x00C0\x00 int b; \x00C1\x00\nint c;\n")
        self.assertEqual(c_lexer.reassemble(r.skeleton, r.comments), src)

    def test_line_numbers(self):
        src = b"a\n\n/* x\n y */\n// z"
        r = c_lexer.lex(src)
        self.assertEqual((r.comments[0].line, r.comments[0].end_line), (3, 4))
        self.assertEqual(r.comments[1].line, 5)

    def test_comment_markers_in_strings_and_chars(self):
        src = b'char *s = "/* not */ // no"; char c = \'/\'; char d = \'"\'; /* real */\n'
        self.assertEqual(comments_of(src), [(BLOCK, b"/* real */")])

    def test_escaped_quotes(self):
        src = b'const char *s = "a\\"b/*c*/"; char q = \'\\\'\'; // tail\n'
        self.assertEqual(comments_of(src), [(LINE, b"// tail")])

    def test_encoding_prefixes(self):
        src = b'L"/*" u8"//" u"x" U"/*"; /* ok */'
        self.assertEqual(comments_of(src), [(BLOCK, b"/* ok */")])

    def test_raw_string(self):
        src = b'auto s = R"xy(a /* b */ // c\n)xy"; /* d */\nauto t = u8R"(/*)"; // e\n'
        self.assertEqual(comments_of(src), [(BLOCK, b"/* d */"), (LINE, b"// e")])

    def test_raw_string_undoes_splice(self):
        # backslash-newline inside a raw string must stay inside the string
        src = b'R"(abc\\\n/* not a comment */)"; /* yes */\n'
        self.assertEqual(comments_of(src), [(BLOCK, b"/* yes */")])

    def test_include_header_name(self):
        src = b"#include <sys/types.h> /* c1 */\n#include \"a//b.h\" // c2\n"
        self.assertEqual(comments_of(src), [(BLOCK, b"/* c1 */"), (LINE, b"// c2")])

    def test_include_header_with_apostrophe_not_a_char(self):
        src = b"#include <don't.h>\n/* c */\n"
        self.assertEqual(comments_of(src), [(BLOCK, b"/* c */")])

    def test_error_directive_apostrophe(self):
        src = b"#error don't do this\n/* c */\n#warning it's fine\nint x; // d\n"
        r = c_lexer.lex(src)
        self.assertEqual([(c.kind, c.text) for c in r.comments], [(BLOCK, b"/* c */"), (LINE, b"// d")])
        self.assertEqual(r.warnings, [])

    def test_unterminated_literal_in_code_closes_at_newline(self):
        src = b"#if 0\nthis isn't code /* hidden */\n#endif\n/* c */\n"
        r = c_lexer.lex(src)
        # conservative: the apostrophe swallows the rest of the line
        self.assertEqual([(c.kind, c.text) for c in r.comments], [(BLOCK, b"/* c */")])
        self.assertEqual([w[0] for w in r.warnings], ["unterminated_literal"])

    def test_no_trailing_newline(self):
        src = b"int a; // tail"
        r = c_lexer.lex(src)
        self.assertEqual([(c.kind, c.text) for c in r.comments], [(LINE, b"// tail")])
        self.assertEqual(c_lexer.reassemble(r.skeleton, r.comments), src)
        src2 = b"int a; /* tail */"
        r2 = c_lexer.lex(src2)
        self.assertEqual(r2.comments[0].end, len(src2))

    def test_unterminated_block_comment_is_warning(self):
        r = c_lexer.lex(b"int a; /* never closed\n int b;")
        self.assertEqual(r.warnings[0][0], "unterminated_block_comment")
        self.assertEqual(r.comments[0].end, len(r.data))

    def test_directive_after_block_comment_on_same_logical_line(self):
        src = b"/* a */ #define X 1 // d\n"
        self.assertEqual(comments_of(src), [(BLOCK, b"/* a */"), (LINE, b"// d")])

    def test_hash_not_at_line_start(self):
        src = b"int x = 1 ; x # <y> /* c */\n"
        self.assertEqual(comments_of(src), [(BLOCK, b"/* c */")])

    def test_less_than_outside_include(self):
        src = b"#define A (x < y) /* c */\nif (a < b) /* d */ > e;\n"
        self.assertEqual(comments_of(src), [(BLOCK, b"/* c */"), (BLOCK, b"/* d */")])

    def test_crlf_preserved(self):
        src = b"int a;\r\n/* c\r\n d */\r\n// e\r\n"
        r = c_lexer.lex(src)
        self.assertEqual([c.text for c in r.comments], [b"/* c\r\n d */", b"// e\r"])
        self.assertEqual(c_lexer.reassemble(r.skeleton, r.comments), src)


class LineSplicing(unittest.TestCase):
    def test_spliced_block_opener(self):
        src = b"int a; /\\\n* spliced */ int b;\n"
        r = c_lexer.lex(src)
        self.assertEqual([(c.kind, c.text) for c in r.comments], [(BLOCK, b"/\\\n* spliced */")])
        self.assertEqual(r.splices, 1)
        self.assertEqual(c_lexer.reassemble(r.skeleton, r.comments), src)

    def test_spliced_line_comment_continues(self):
        src = b"// first \\\nsecond line is comment\nint b;\n"
        r = c_lexer.lex(src)
        self.assertEqual([c.text for c in r.comments], [b"// first \\\nsecond line is comment"])
        self.assertEqual(r.skeleton, b"\x00C0\x00\nint b;\n")

    def test_spliced_line_comment_crlf(self):
        src = b"// a \\\r\nb\r\nint c;\r\n"
        r = c_lexer.lex(src)
        self.assertEqual([c.text for c in r.comments], [b"// a \\\r\nb\r"])

    def test_spliced_slash_slash(self):
        src = b"/\\\n/ comment\nint x;\n"
        r = c_lexer.lex(src)
        self.assertEqual([(c.kind, c.text) for c in r.comments], [(LINE, b"/\\\n/ comment")])

    def test_spliced_directive_include(self):
        src = b"#inc\\\nlude <a'b.h>\n/* c */\n"
        self.assertEqual(comments_of(src), [(BLOCK, b"/* c */")])

    def test_spliced_string(self):
        src = b'char *s = "abc\\\ndef /* no */";\n/* yes */\n'
        self.assertEqual(comments_of(src), [(BLOCK, b"/* yes */")])

    def test_macro_with_continuation_lines(self):
        src = (b"#define FOO(x) \\\n"
               b"\tdo { /* c1 */ \\\n"
               b"\t\t(x)++; /* c2 */ \\\n"
               b"\t} while (0)\n")
        self.assertEqual(comments_of(src), [(BLOCK, b"/* c1 */"), (BLOCK, b"/* c2 */")])

    def test_offsets_are_original(self):
        src = b"\\\n\\\n/* c */"
        r = c_lexer.lex(src)
        self.assertEqual((r.comments[0].start, r.comments[0].end), (4, 11))


class Trigraphs(unittest.TestCase):
    def test_find(self):
        hits = c_lexer.find_trigraphs(b"a ??= b ??/ ??( ??) x")
        self.assertEqual([t for _, t in hits], [b"??=", b"??/", b"??(", b"??)"])
        self.assertEqual(c_lexer.find_trigraphs(b"?? ?x? ???"), [])

    def test_all_nine(self):
        self.assertEqual(len(c_lexer.TRIGRAPHS), 9)
        for t in c_lexer.TRIGRAPHS:
            self.assertEqual(c_lexer.find_trigraphs(b"x" + t + b"y"), [(1, t)])


if __name__ == "__main__":
    unittest.main()
