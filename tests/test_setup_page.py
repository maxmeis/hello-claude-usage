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
    pattern = re.escape(call_prefix) + r"\s*,\s*(?:sizeof\([^)]*\)|[A-Z_][A-Z_0-9]*)\s*,\s*((?:\s*" + part + r")+)"
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
    states = ["Ready", *re.findall(r'set_page_state\("([^"]+)"\)', source)]
    page = page_format % ("hello-box-4827", auth_url, max(states, key=len))
    capacity_match = re.search(r"#define SETUP_PAGE_CAPACITY (\d+)", source)
    auth_capacity_match = re.search(r"char s_auth_url\[(\d+)\]", source)
    if not capacity_match or not auth_capacity_match:
        raise AssertionError("Could not read heap page/auth URL buffer capacities from C source")
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

    def test_settings_page_has_labeled_sections_and_accessible_feedback(self):
        tags = self.parsed.tags
        for landmark in ("head", "body", "main", "header", "footer"):
            self.assertTrue(any(tag == landmark for tag, _ in tags),
                            f"missing {landmark} landmark")
        self.assertIn("<title>Settings</title>", self.page)
        self.assertIn("<h1>Settings</h1>", self.page)
        sections = [attrs.get("aria-labelledby") for tag, attrs in tags if tag == "section"]
        self.assertEqual(sections, ["account-title", "wifi-title", "reset-section-title"])
        ids = {attrs["id"] for _, attrs in tags if "id" in attrs}
        self.assertTrue(set(sections).issubset(ids))
        labels = [attrs for tag, attrs in tags if tag == "label"]
        self.assertEqual([attrs.get("for") for attrs in labels], ["code"])
        self.assertEqual(self.parsed.textareas[0].get("aria-describedby"), "code-help")
        self.assertIn("code-help", ids)
        result = next(attrs for tag, attrs in tags if attrs.get("id") == "result")
        self.assertEqual(result.get("role"), "status")
        self.assertEqual(result.get("aria-live"), "polite")
        self.assertIn("Get a sign-in code", self.page)
        self.assertIn("Paste the full code, including #state", self.page)
        self.assertLess(self.page.index("account-title"), self.page.index("wifi-title"))
        self.assertLess(self.page.index("wifi-title"), self.page.index("reset-section-title"))

    def test_feedback_moves_beside_each_action_and_wraps(self):
        script = self.parsed.scripts[0]
        self.assertIn("insertAdjacentElement('afterend',r)", script)
        self.assertIn("resultAfter('send')", script)
        self.assertIn("resultAfter('forget')", script)
        self.assertIn("resultAfter('prepare-reset')", script)
        self.assertIn("overflow-wrap:anywhere", self.page)
        if not shutil.which("node"):
            return
        harness = r"""
const script = require('fs').readFileSync(0, 'utf8');
const nodes = Object.fromEntries(['send','forget','prepare-reset','result','code',
  'reset-dialog','cancel-reset','confirm-reset'].map(id => [id, {
    id, parent: null, value: 'code', textContent: '',
    insertAdjacentElement(where, element) { if (where !== 'afterend') throw Error(where); element.parent = id },
    focus() {}, addEventListener() {}, showModal() { this.open = true }, close() { this.open = false }
  }]));
global.document = {getElementById: id => nodes[id]};
global.location = {pathname:'/setup'};
global.setInterval = () => 1;
global.fetch = async (_path, options) => ({ok:true, text:async () =>
  options.headers['X-Setup-Action'] === 'prepare-reset' ? 'nonce' : 'Done'});
eval(script);
(async () => {
  await nodes.send.onclick(); if (nodes.result.parent !== 'send') throw Error('send result');
  await nodes.forget.onclick(); if (nodes.result.parent !== 'forget') throw Error('forget result');
  await nodes['prepare-reset'].onclick();
  if (nodes.result.parent !== 'prepare-reset' || !nodes['reset-dialog'].open) throw Error('prepare result');
  await nodes['confirm-reset'].onclick();
  if (nodes.result.parent !== 'prepare-reset' || nodes['reset-dialog'].open) throw Error('confirm result');
})().catch(e => { console.error(e); process.exitCode = 1 });
"""
        checked = subprocess.run(["node", "-e", harness], input=script, text=True,
                                 capture_output=True, check=False)
        self.assertEqual(checked.returncode, 0, checked.stderr)

    def test_button_boundaries_have_visible_contrast(self):
        def luminance(color):
            values = [int(color[i:i + 2], 16) / 255 for i in (1, 3, 5)]
            values = [value / 12.92 if value <= 0.04045
                      else ((value + 0.055) / 1.055) ** 2.4 for value in values]
            return sum(value * weight for value, weight in zip(values, (.2126, .7152, .0722)))

        def contrast(first, second):
            a, b = luminance(first), luminance(second)
            return (max(a, b) + .05) / (min(a, b) + .05)

        ordinary = re.search(r"button\{background:(#[0-9a-f]{6});color:[^}]+border:1px solid (#[0-9a-f]{6})", self.page)
        danger = re.search(r"\.danger-zone button\{[^}]*border-color:(#[0-9a-f]{6});background:transparent", self.page)
        self.assertIsNotNone(ordinary)
        self.assertIsNotNone(danger)
        for fill in (ordinary.group(1), "#343435"):
            self.assertGreaterEqual(contrast(ordinary.group(2), fill), 3)
        self.assertGreaterEqual(contrast(danger.group(1), "#18191b"), 3)

    def test_page_shows_only_validated_device_identity(self):
        self.assertIn("<p class='device-name'>hello-box-4827</p>", self.page)
        get = self.source.split("static esp_err_t setup_get", 1)[1].split("static esp_err_t setup_post", 1)[0]
        self.assertIn("device_identity_name()", get)
        self.assertIn('strlen(device_name) != 14', get)
        self.assertIn('strncmp(device_name, "hello-box-", 10)', get)
        self.assertIn("device_name[i] < '0' || device_name[i] > '9'", get)
        self.assertLess(get.index("device_name = \"Device\""), get.index("snprintf(page"))
        self.assertIn("malloc(SETUP_PAGE_CAPACITY)", get)
        self.assertIn("free(page)", get)
        self.assertNotIn("s_cap", self.page)
        self.assertNotIn("refresh_token", self.page)

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

    def test_forget_wifi_is_a_separate_explicit_action(self):
        buttons = [attrs for tag, attrs in self.parsed.tags if tag == "button"]
        self.assertTrue(any(attrs.get("id") == "forget" for attrs in buttons))
        self.assertIn("Forget Wi-Fi and reconnect", self.page)
        self.assertIn("Your Claude sign-in stays saved", self.page)
        self.assertIn("setup QR code", self.page)
        script = self.parsed.scripts[0]
        self.assertIn("getElementById('forget').onclick", script)
        self.assertIn("'X-Setup-Action':'forget-wifi'", script)
        self.assertNotIn("body:b.value", script.split("getElementById('forget').onclick", 1)[1],
                         "Wi-Fi reset must not carry the OAuth code")
        post = self.source.split("static esp_err_t setup_post", 1)[1].split("static void stop_setup", 1)[0]
        self.assertIn('strcmp(action, "forget-wifi") != 0', post)
        self.assertIn("req->content_len != 0", post)
        self.assertIn("xQueueSend(s_forget_commands, &session, 0)", post)
        self.assertNotIn("wifi_manager_request_forget", post,
                         "the HTTP handler must only queue the reset")
        reset = self.source.split("static void process_forget", 1)[1].split("static void worker", 1)[0]
        self.assertLess(reset.index("stop_setup();"), reset.index("wifi_manager_request_forget()"),
                        "release the account HTTP port before starting Wi-Fi setup")
        self.assertNotIn("clear_token", reset, "Wi-Fi reset must retain Claude sign-in")

    def test_factory_reset_requires_dialog_and_second_post(self):
        buttons = [attrs for tag, attrs in self.parsed.tags if tag == "button"]
        for button_id in ("prepare-reset", "cancel-reset", "confirm-reset"):
            self.assertTrue(any(attrs.get("id") == button_id for attrs in buttons))
        self.assertIn("autofocus", next(attrs for attrs in buttons
                                        if attrs.get("id") == "cancel-reset"))
        self.assertTrue(any(tag == "dialog" and attrs.get("id") == "reset-dialog"
                            for tag, attrs in self.parsed.tags))
        for detail in ("Claude sign-in", "saved Wi-Fi", "device settings",
                       "new name generated", "cannot be undone"):
            self.assertIn(detail, self.page)
        script = self.parsed.scripts[0]
        self.assertIn("'X-Setup-Action':'prepare-reset'", script)
        self.assertIn("resetDialog.showModal()", script)
        self.assertIn("getElementById('cancel-reset').focus()", script)
        self.assertIn("'X-Setup-Action':'cancel-reset'", script)
        self.assertIn("'X-Setup-Action':'factory-reset'", script)
        self.assertIn("'X-Reset-Confirmation':nonce", script)
        self.assertIn("resetDialog.addEventListener('cancel'", script)

    def test_portal_is_capability_scoped_and_does_not_replace_account_on_failure(self):
        get = self.source.split("static esp_err_t setup_get", 1)[1].split("static esp_err_t setup_post", 1)[0]
        self.assertIn('"Cache-Control", "no-store"', get)
        self.assertIn('"Content-Security-Policy"', get)
        self.assertIn("cap_matches(req->uri", get)
        worker = self.source.split("static void worker", 1)[1]
        manual = worker.split("if (s_manual_session)", 1)[1].split("int64_t now = now_s()", 1)[0]
        self.assertLess(manual.index("save_token(candidate)"), manual.index("*token = *candidate"),
                        "manual account replacement must follow durable save")
        self.assertIn("settings_wanted(s_manual_request_id)", manual)
        self.assertIn("stop_setup();", manual)


if __name__ == "__main__":
    unittest.main()
