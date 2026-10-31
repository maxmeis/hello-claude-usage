# hello-box browser installer

A Vercel-hosted, browser-local installer for the SenseCAP Indicator D1 with the GX display, ESP32-S3, and 8 MB flash. Use desktop Chrome or Edge over HTTPS for USB flashing. Other browsers can edit settings and view the preview.

The website configures Wi-Fi, German or English clock text, timezone, word-clock screensaver or persistent Home, backlight brightness, dashboard timeout, refresh cadence, mascot motion, and refresh animation. All user settings are encoded in memory and sent directly over USB. There is no cloud configuration API, account database, analytics, or credential upload. Wi-Fi setup, connection checks, Claude authorization, and display configuration are available in the web app over USB.

**New setup** writes the verified bootloader, partition table, application, and a one-shot configuration sector. **Update firmware** writes only the application. Both preserve the hello-box NVS/account/identity and reserved data regions. USB identifies chip family and flash size; it cannot prove the board is a SenseCAP. Connect only the supported hardware.

The firmware stores validated display settings separately. New Wi-Fi credentials go through the existing APSTA connection trial and replace saved credentials only after a matching connection receives an IP address and saves successfully. A failed candidate leaves phone setup available. The one-shot sector is consumed after a terminal result and is also cleared during factory reset.

## Connect and configure

Choose **Connect box** first. Firmware with USB management reports its saved display settings and separate Wi-Fi and Claude states. Saved credentials are never read back. Existing installations default to a firmware-only update; changing display settings writes the one-shot configuration sector while preserving account and Wi-Fi storage.

A box that does not answer may still be starting or running older firmware. Retry first; if needed, explicitly upgrade the firmware while preserving its data, then reconnect. A missing response is not evidence that the box is unconfigured.

Use **Connect and test** to submit Wi-Fi credentials directly to the device. Success requires this specific trial to receive an IP address and save the credentials. A failed USB trial preserves the previous saved network and reconnects it when available. The separate network check reports association, IP, time, DNS and certificate-verified HTTPS; HTTPS reachability does not prove Claude account authorization.

Start Claude sign-in in the web app, open the device-generated authorization link in a **new tab**, then paste the complete `code#state` back into the app. PKCE exchange and credential storage happen on the box. Cancel and failed exchanges preserve an existing account. The app distinguishes a saved account from an account validated by an authenticated usage response.

The production UART0 protocol uses bounded `HELLO_BOX_RPC` JSON lines at 115200 baud. It is available to a locally connected USB host and returns settings and status, never Wi-Fi passwords or Claude tokens. All device HTTP work stays serialized in the existing workers. Routine flashing preserves NVS; a full chip erase is a separate destructive operation.

## Claude, Codex, and the device preview

Choose Claude, Codex, or Both in display settings. Existing settings and older installer frames default to Claude; disabling a provider does not erase its account. Home shows separate Claude and Codex widgets. A widget opens its app; disabled or unconfigured apps show a setup action without cached quotas. Reset countdowns come from the reported reset timestamps. **Reset tokens** means the provider-reported count of earned/manual resets; an unavailable count is different from zero. The app and firmware never redeem these resets.

Codex authorization starts on the device. Open the exact OpenAI device-sign-in link in a new tab and enter the displayed one-time code there; the box polls for completion. Tokens remain in the dedicated `quota_accounts` partition, and only public login status and usage cross USB. Factory reset erases this account partition; Forget Wi-Fi retains it. Account refresh/usage HTTPS is serialized across providers, with separate retry deadlines. Saved accounts are distinguished from accounts validated by an authenticated request.

The Home/Claude/Codex/word-clock preview is independent of the saved default screen. It follows provider selection, language and brightness. With a connected box it uses reported usage; missing or stale data stays unavailable. Without a connection it is explicitly labelled as an example. It is a representation of device data, not a captured display framebuffer.

Codex's direct HTTP interoperability follows OpenAI's client source at `67f0994b532b8740f14b5b738af7b7b2a915b1d6`. These account endpoints are internal and can change; the documented [app-server authentication and rate-limit fields](https://learn.chatgpt.com/docs/app-server) describe the corresponding login and usage concepts. No hosted credential service or local Codex credential import is used. Full account usage needs fresh user authorization.

The preceding multi-provider release passed 94 Python tests, 36 web tests, and 47 native ASan/UBSan display scenes, along with the firmware build and focused independent reviews. A live account's Codex authorization and quotas are separate from the stub-backed tests.

## App and screensaver shell

Firmware `0.18.0-app-framework` uses app and screensaver registries with a shared navigation policy. Home combines the app widgets in two full-width rows; app details retain their own refresh targets. Codex shows only a valid reported seven-day window, selected by duration from either response position. A missing week remains unavailable; another duration is never relabelled. The clock is a screensaver, not a provider view. Tap the word-clock screensaver to return to Home. Home and apps return to it after the configured idle timeout.

After manual Home/app navigation, the configured idle timeout (30 seconds by default) returns to the screensaver. A new successful background measurement may interrupt it for eight seconds; multiple active apps share one Home interruption. New results do not extend that deadline. Face animation finishes before the attention window starts. Manual navigation and Settings keep focus, and manual screensaver navigation suppresses the current request's later interruption. Selecting persistent Home disables automatic screensaver return.

The existing configuration encoding remains compatible. USB status includes static app IDs, names and capabilities plus available screensavers; it does not invoke display callbacks from the serial task. The local **Preview refresh** control simulates animation and timed return without contacting the device or requesting provider data.

## Build a public release

From the firmware repository root:

```sh
python3 scripts/build_public_firmware.py
cd web-installer
pnpm install --frozen-lockfile
pnpm test
pnpm build
```

The public firmware script refuses `main/provision.h` or enabled diagnostic helpers, starts from a fresh `build-web-release` directory, and records source/configuration/image hashes. Packaging rejects stale receipts or changed images. Only the three explicitly named public `.bin` assets are allowed by Git; private diagnostic binaries remain ignored. Keep the generated `public/firmware` bundle with the site source for Vercel's static build.

Vercel uses the uploaded, checksum-verified bundle. It does not need ESP-IDF and never compiles credentials. Run deployment from the repository root after linking the website to the Vercel project:

```sh
node web-installer/scripts/deploy-vercel.mjs
```

The helper uploads only the website directory. It temporarily removes the Git monorepo root prefix for this already-scoped CLI upload and restores it afterward. Production headers limit resources to the same origin, enable Web Serial for the top-level site, and disable framing. Browser settings and Wi-Fi values never become Vercel environment variables.

## Validation

Node tests exercise configuration encoding, checksum/layout validation, and the actual install/update orchestration with stubbed serial/flash transports: wrong chip, small flash, corrupt assets, cancellation, unplug, cleanup, and exact write offsets. Python tests decode JavaScript-generated sectors using the production C decoder. Sanitized native renders cover customized display behavior.

These checks do not replace a physical Web Serial run. Browser port selection, USB reset timing, real backlight output, and a complete browser-driven install should be checked on the supported SenseCAP hardware. “Flash contents verified” describes chip readback, not Wi-Fi association or Claude sign-in.

See [flashing details](README-flashing.md) and the [firmware README](../README.md).

## Latest physical check

On 31 October 2026, firmware `0.16.1-usb-setup` was installed on the second SenseCAP with its saved data preserved. Direct production USB commands successfully read settings, connected and saved a Wi-Fi candidate, checked DNS and verified HTTPS, and began/cancelled Claude authorization without replacing the existing account. A reboot retained settings and Wi-Fi and reached authenticated Claude usage again. The temporary Wi-Fi secret file was removed afterward.

The full Python suite passes 82 tests and the website suite passes 27 tests. The Vercel production bundle was fetched and all three image SHA-256 values matched. The browser USB chooser could not be controlled in this machine session, so a full chip erase and real browser-driven fresh installation remain unverified. No full erase was performed.

## Multi-provider hardware check

Firmware `0.17.1-multi-provider` passed the native OpenAI device-code start/cancel handshake on the second SenseCAP. The Both selection was imported and persisted, then the original Claude settings were restored; Wi-Fi and authenticated Claude usage recovered after reboot. No OpenAI account was authorized during this test, so authenticated Codex usage and reset-token values are validated against source schemas and host tests rather than a live signed-in account.

The first live handshake exposed internal-heap exhaustion during RSA certificate verification (`RSA_PUBLIC_FAILED + MPI_ALLOC_FAILED`). The supported MbedTLS external-PSRAM allocator fixed the handshake while retaining the existing certificate bundle, hostname checks, and certificate verification. No trust bypass or server-leaf pinning was introduced.

## Framework hardware check

Firmware `0.18.0-app-framework` was installed on the second SenseCAP by writing only the application partition. The image hash was verified, and a subsequent reboot retained the original Wi-Fi, device identity, German clock settings, 30-second idle timeout, five-minute polling interval, and Claude account. Authenticated Claude usage recovered, and the device passed DNS and certificate-verified HTTPS checks without an observed panic. USB reported both app descriptors and the word-clock screensaver descriptor. Codex remains signed out on this device; these checks do not claim live authenticated Codex usage.

The Vercel production web bundle and all three firmware images were fetched and matched the local release hashes. No full chip erase or browser-controlled USB flashing was performed for this update.

## Weekly-only UI revision

`0.18.1-app-framework` replaces the narrow Home columns with two full-width provider rows. Claude retains its five-hour and weekly quotas. Codex has one weekly row and one weekly detail panel, selected only from a valid reported 604800-second window in either response slot. Unsupported short windows never become invented weekly values or screensaver attention. The web preview uses the same validity rules and proportional type scale.

Validation: 99 Python tests, 41 web tests, all 27 current native ASan/UBSan scenes, a clean firmware build, and focused independent review passed. The renderer's historical `face-refresh` and `forecast-preclock` routes are excluded from defaults; refresh admission/cycles and face rendering remain covered, but the complete old manual-refresh timing sequence is not currently replayed. Device reboot retained Wi-Fi, settings and the authenticated Claude account. The live website and three firmware files match the local release hashes. Authenticated Codex usage still requires user sign-in.

## Polished device UI

Firmware `0.19.0-polished-ui` replaces the crowded text toolbar with 24px navigation symbols, hides redundant Home navigation on Home, and keeps the Clawd and clock actions. Borderless provider rows use 48px percentages and 16px labels/countdowns. Codex has one 80px weekly detail. Unknown reset-token filler is hidden; a known zero remains visible. Missing weekly data has one clear message, while real errors and stale states retain priority.

Validation passed: 99 host tests, 41 web tests, all 27 default native ASan/UBSan scenes (201 C sources), native/device-configuration render comparisons, the firmware build, and independent review. The application-only update and reboot preserved the second box’s settings, Wi-Fi and authenticated Claude account. The live web bundle and all three firmware hashes match the validated release. The embedded preview uses example values; no live Codex account was authorized during these checks.

## Clock as screensaver only

`0.19.1-screensaver-clock` removes the digital clock and its touch target from Home, app headers, boot and refresh animations. The word-clock screensaver still appears after idle, and tapping it returns to Home. Time synchronization, reset countdowns and forecasts remain unchanged.

Validation: 99 host tests, 42 web tests and all 28 current native ASan/UBSan scenes passed, including absence of a header clock, an inert former clock location and idle entry into the word clock. Firmware build, render configuration comparison and focused review passed.

## Quiet idle footer

`0.19.2-quiet-footer` removes the decorative rotating thinking words from the device. Healthy idle leaves the center footer blank; last-update age and actual update, stale, waiting, retry and error messages remain. A native framebuffer regression checks idle after the former rotation interval, active refresh, error recovery and clearing old status pixels. All 29 native sanitizer scenes, 99 host tests and 42 web tests pass.

### Codex sign-in persistence

`0.19.3-codex-signin` saves a successful OAuth token exchange before fetching usage. The separate usage request can retry without losing the saved account. A failed save or cancelled generation preserves the previous account; the commit reservation remains held until the pending authorization is cleared. Late sign-in failures update the matching USB operation without overwriting newer work. Diagnostic logs expose numeric HTTP, parse and save results only. All 100 host tests and 42 web tests pass; the host tests include production-C authentication and quota-retry scenarios under ASan/UBSan.

`0.19.4-codex-headers` gives authenticated Codex requests a bounded 4,224-byte transmit buffer. The pinned ESP-IDF header writer cannot split one header across buffers: its 512-byte default can leave a long Bearer header unfinished and cause `ESP_ERR_HTTP_FETCH_HEADER`. The SDK-backed sanitizer regression reproduces that failure and verifies complete requests with synthetic token lengths of 600, 2,000 and 4,095 bytes. Requests without an account retain the default buffer. Saved accounts and TLS verification are preserved.

Validation: 101 host tests and 42 web tests passed. On the connected SenseCAP, the updated firmware reused its saved Codex account after restart and returned HTTP 200 with a valid weekly usage sample and reset-credit count. Claude also validated, Wi-Fi reconnected, and both apps were enabled while the other saved display settings stayed unchanged. The published firmware bundle and web app were checked against the local build hashes.
