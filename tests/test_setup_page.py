"""Regression checks for the setup page embedded in oauth_probe.c."""

import ast
from html.parser import HTMLParser
from pathlib import Path
import re
import shutil
import subprocess
import unittest


SOURCE = Path(__file__).resolve().parents[1] / "main" / "oauth_probe.c"


def c_string_sequence(text):
    """Decode adjacent ordinary C string literals with Python's C-compatible escapes."""
    literals = re.findall(r'"(?:\\.|[^"\\])*"', text)
    return "".join(ast.literal_eval(literal) for literal in literals)


def source_template(source, function, call_prefix):
    body = source.split(function, 1)[1]
    literal = r'"(?:\\.|[^"\\])*"'
    part = r"(?:" + literal + r"|[A-Z_][A-Z_0-9]*)"
    pattern = re.escape(call_prefix) + r"\s*,\s*sizeof\([^)]*\)\s*,\s*((?:\s*" + part + r")+)"
    match = re.search(pattern, body, re.DOTALL)
    if not match:
        raise AssertionError(f"Could not extract C string template for {call_prefix}")
    macros = dict(re.findall(r'^#define\s+([A-Z_][A-Z_0-9]*)\s+("(?:\\.|[^"\\])*")\s*$',
                             source, re.MULTILINE))
    expression = match.group(1)
    expanded = []
    for token in re.findall(literal + r"|[A-Z_][A-Z_0-9]*", expression):
        if token.startswith('"'):
            expanded.append(token)
        elif token in macros:
            expanded.append(macros[token])
        else:
            raise AssertionError(f"Unsupported token {token} in {call_prefix} C string template")
    return c_string_sequence(" ".join(expanded))


class SetupPageParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self.tags = []
        self.textareas = []
        self.scripts = []
        self.current_script = None

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        self.tags.append((tag, attrs))
        if tag == "textarea":
            self.textareas.append(attrs)
        if tag == "script":
            self.current_script = []

    def handle_data(self, data):
        if self.current_script is not None:
            self.current_script.append(data)

    def handle_endtag(self, tag):
        if tag == "script" and self.current_script is not None:
            self.scripts.append("".join(self.current_script))
            self.current_script = None


def rendered_page(source):
    auth_format = source_template(source, "static bool start_setup", "snprintf(s_auth_url")
    auth_url = auth_format % ("A" * 43, "S" * 32)  # SHA-256 PKCE and 24-byte state, base64url.
    page_format = source_template(source, "static esp_err_t setup_get", "snprintf(page")
    page = page_format % (auth_url, "Ready")
    capacity_match = re.search(r"char page\[(\d+)\]", source.split("static esp_err_t setup_get", 1)[1])
    auth_capacity_match = re.search(r"char s_auth_url\[(\d+)\]", source)
    if not capacity_match or not auth_capacity_match:
        raise AssertionError("Could not read setup page/auth URL buffer capacities from C source")
    return page, int(capacity_match.group(1)), auth_url, int(auth_capacity_match.group(1))


class SetupPageRegressionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = SOURCE.read_text(encoding="utf-8")
        cls.page, cls.capacity, cls.auth_url, cls.auth_capacity = rendered_page(cls.source)
        cls.parsed = SetupPageParser()
        cls.parsed.feed(cls.page)

    def test_maximum_source_auth_url_fits_page_buffer_including_nul(self):
        self.assertLessEqual(len(self.auth_url), self.auth_capacity - 1,
                             "maximum source-derived OAuth URL exceeds s_auth_url")
        self.assertLessEqual(len(self.page.encode("utf-8")) + 1, self.capacity,
                             f"rendered setup page needs {len(self.page.encode('utf-8')) + 1} bytes, "
                             f"page buffer has {self.capacity}")

    def test_html_exposes_local_code_submission_without_external_assets(self):
        self.assertIn(("html", {"lang": "en"}), self.parsed.tags)
        links = [attrs for tag, attrs in self.parsed.tags if tag == "a"]
        self.assertEqual(len(links), 1, "expected one Claude authorization link")
        self.assertEqual(links[0].get("href"), self.auth_url)
        self.assertIn("noreferrer", links[0].get("rel", "").split())
        self.assertIn("noopener", links[0].get("rel", "").split())
        self.assertEqual(len(self.parsed.textareas), 1, "OAuth code input textarea is missing")
        self.assertTrue(any(tag == "button" and attrs.get("id") == "send"
                            for tag, attrs in self.parsed.tags), "Submit code button is missing")
        self.assertTrue(any(tag == "meta" and attrs.get("name") == "referrer"
                            and attrs.get("content") == "no-referrer"
                            for tag, attrs in self.parsed.tags), "no-referrer meta is missing")
        external_assets = [f"<{tag} {attrs}>" for tag, attrs in self.parsed.tags
                           if (tag == "script" and attrs.get("src"))
                           or (tag == "link" and attrs.get("href", "").startswith(("http:", "https:")))]
        self.assertEqual(external_assets, [], "setup page must not load external scripts/styles")

    def test_submit_posts_raw_code_to_current_path_and_shows_result(self):
        self.assertEqual(len(self.parsed.scripts), 1, "expected one inline submit script")
        script = self.parsed.scripts[0]
        for behavior, description in (
            ("getElementById('send').onclick", "send button handler"),
            ("fetch(location.pathname", "POST to the current setup path"),
            ("method:'POST'", "POST method"),
            ("'Content-Type':'text/plain'", "plain-text code payload"),
            ("body:b.value", "textarea code submitted as raw body"),
            ("r.textContent=await x.text()", "server response shown in the page"),
            ("b.value=''", "code input cleared after submission"),
        ):
            self.assertIn(behavior, script, f"submit flow is missing {description}")
        if shutil.which("node"):
            checked = subprocess.run(["node", "--check"], input=script, text=True,
                                     capture_output=True, check=False)
            self.assertEqual(checked.returncode, 0,
                             f"inline setup JavaScript failed node --check: {checked.stderr}")


if __name__ == "__main__":
    unittest.main()
