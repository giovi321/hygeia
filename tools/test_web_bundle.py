"""Tests for tools/web_bundle.py. Run from the repository root: python -m unittest tools/test_web_bundle.py"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import web_bundle  # noqa: E402


INDEX = """<!doctype html>
<html><head><link rel="stylesheet" href="/app.css"></head>
<body><script src="/app.js"></script></body></html>
"""


def write(root, rel, text):
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def make_src(root, css="body{color:#111}\n"):
    write(root, "index.html", INDEX)
    write(root, "app.css", css)
    write(root, "favicon.svg", "<svg xmlns='http://www.w3.org/2000/svg'/>\n")
    write(root, "js/boot.js", "console.log('boot');\n")
    write(root, "js/extra.js", "console.log('NOT_LISTED_MARKER');\n")
    write(root, "bundle.txt", "# order of the scripts in app.js\njs/boot.js\n")


class WebBundleTest(unittest.TestCase):
    def test_same_input_gives_identical_output(self):
        with tempfile.TemporaryDirectory() as d:
            make_src(d)
            a = web_bundle.render_c(web_bundle.build(d))
            b = web_bundle.render_c(web_bundle.build(d))
            self.assertEqual(a, b)

    def test_hashed_name_follows_css(self):
        with tempfile.TemporaryDirectory() as d1, tempfile.TemporaryDirectory() as d2:
            make_src(d1, css="body{color:#111}\n")
            make_src(d2, css="body{color:#222}\n")
            p1 = [a.path for a in web_bundle.build(d1)]
            p2 = [a.path for a in web_bundle.build(d2)]
            css1 = [p for p in p1 if p.endswith(".css")]
            css2 = [p for p in p2 if p.endswith(".css")]
            self.assertEqual(len(css1), 1)
            self.assertRegex(css1[0], r"^/app\.[0-9a-f]{8}\.css$")
            self.assertNotEqual(css1, css2)
            js1 = [p for p in p1 if p.endswith(".js")]
            js2 = [p for p in p2 if p.endswith(".js")]
            self.assertEqual(js1, js2)

    def test_index_references_hashed_names(self):
        with tempfile.TemporaryDirectory() as d:
            make_src(d)
            assets = web_bundle.build(d)
            index = [a for a in assets if a.path == "/index.html"][0]
            html = web_bundle.gunzip(index.gz).decode("utf-8")
            css = [a.path for a in assets if a.path.endswith(".css")][0]
            js = [a.path for a in assets if a.path.endswith(".js")][0]
            self.assertIn('href="%s"' % css, html)
            self.assertIn('src="%s"' % js, html)
            self.assertNotIn('"/app.css"', html)
            self.assertFalse(index.immutable)
            self.assertTrue([a for a in assets if a.path == css][0].immutable)

    def test_file_missing_from_bundle_list_is_not_served(self):
        with tempfile.TemporaryDirectory() as d:
            make_src(d)
            assets = web_bundle.build(d)
            paths = [a.path for a in assets]
            self.assertEqual(len(paths), 4)
            self.assertFalse(any("extra" in p for p in paths))
            js = [a for a in assets if a.path.endswith(".js")][0]
            text = web_bundle.gunzip(js.gz).decode("utf-8")
            self.assertIn("boot", text)
            self.assertNotIn("NOT_LISTED_MARKER", text)

    def test_size_limit(self):
        limit = 200 * 1024
        fake = [web_bundle.Asset("/a", "text/plain", b"x" * (limit - 10), False),
                web_bundle.Asset("/b", "text/plain", b"y" * 10, False)]
        self.assertEqual(web_bundle.check_size(fake, limit), limit)
        fake.append(web_bundle.Asset("/c", "text/plain", b"z", False))
        with self.assertRaises(web_bundle.BundleError):
            web_bundle.check_size(fake, limit)

    def test_missing_listed_file_fails(self):
        with tempfile.TemporaryDirectory() as d:
            make_src(d)
            write(d, "bundle.txt", "js/boot.js\njs/absent.js\n")
            with self.assertRaises(web_bundle.BundleError):
                web_bundle.build(d)


if __name__ == "__main__":
    unittest.main()
