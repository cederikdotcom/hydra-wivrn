# Issue #739: WiVRn client distribution for headsets

> ## SUPERSEDED IN PART, 2026-09-16
>
> The owner ruled after this was written: **we will not release through Meta**,
> the target platform will probably be **Pico**, and **side-loading is
> acceptable for now**.
>
> That removes the spine of section 3 and most of the rollout: no Meta
> organisation, no Horizon Managed Services, no private release channel, and no
> issue split (its justification was a Meta verification call nobody owned).
>
> What still stands: the signing and keystore work in section 2, the version and
> capability reporting in section 5, and the coexistence question in section 4,
> where the incumbent becomes Pico Business Streaming rather than
> hydraheadquest. The session-handoff experiment is still the gate.
>
> Verified the same day: the WiVRn client already supports Pico as a first-class
> target (`client/application.cpp` carries pico_neo3, pico4 and pico4s
> interaction profiles; `client/hmd_traits.cpp` has a Pico branch). The platform
> change invalidates the delivery channel, not the client.
>
> Read this document for sections 2, 4 and 5. Treat section 3 and the rollout as
> historical. The current position is on issue #739.


> **Verified after writing, 2026-09-16.** Two claims in this document were
> re-checked against the live estate and both held:
>
> - `releases.experiencenet.com/hydrawivrn/production/latest/hydra-wivrn-server.pkg.tar.zst`
>   returns **404**. The constant-filename convention this design depends on has
>   never been exercised here; only the versioned name resolves through
>   `/latest/`. That is why rollout step 0 exists.
> - The `/latest/` redirect target is
>   `bxl1.hydramirror.experiencenet.com`, a district-named host that also fronts
>   the issue tracker. Any permanent install URL handed to Meta dereferences
>   through it.
>
> See `739-critique.md` for the full adversarial review this design was hardened
> against.


Design document. Written 2026-09-16. Implement from this, not from the proposals it supersedes.

---

## 1. Decision

Build a Hydra-signed WiVRn client as its own APK from `cederikdotcom/hydra-wivrn`, publish it to a new `hydrawivrnclient` project on `releases.experiencenet.com` under a constant filename, and install it on headsets through Meta, either Meta Horizon Managed Services installing from that URL or a Meta private release channel, with `hydraadb` demoted to a lab-only lane. hydraheadquest stays the kiosk and the launcher: it gains a driver table, a package-visibility entry for the new client, and it reports which XR clients a head actually has, so the cluster can match heads to bodies on the XR driver the same way it already matches bodies.

We are explicitly **not** doing these things: not absorbing the WiVRn client into the hydraheadquest APK (kept as a documented escape hatch, see 4.6); not building or registering our own Quest MDM (Horizon OS enrols only Meta-approved MDMs, and `dpm set-device-owner` fails with accounts present, per #544); not buying ManageXR or ArborXR yet; not shipping a session-blocking version gate; and not keeping `adb`, a cable or a laptop anywhere on the venue path.

We are also **splitting the issue**, because three of the four candidate designs had a rollout whose fourth step blocked on a Meta business verification call:

- **#739 (this document, sections 2, 3-track-A, 5)** builds and publishes the signed client. No external dependency. Shippable this month.
- **New issue, "Quest fleet distribution"** (device- and app-agnostic): Meta organisation, HMS or release channel, one enrolled headset, the `ACTIVATE_VPN` question, the hydramdm Quest runbook. Owned separately, started this week, measured in weeks. It blocks hydraheadquest too, not only WiVRn.
- **New issue, "XR driver launch and arbitration in hydraheadquest"** (section 4): gated on the hardware experiment E-1.

---

## 2. Package and signing

### 2.1 Package id

`com.experiencenet.hydraxr`, launcher label "Hydra XR".

The Java package and the JNI symbols stay `org.meumeu.wivrn`. The launch component is therefore:

```
com.experiencenet.hydraxr/org.meumeu.wivrn.MainActivity
```

hydraheadquest already proves this split (`namespace com.limelight`, `applicationId com.experiencenet.hydraheadquest`). No Java is renamed, no JNI symbol changes, `client/java/MainActivity.java` is untouched.

Getting there needs **one line** in `/home/claude-user/hydra-wivrn/build.gradle`, replacing the hardcoded applicationId:

```groovy
applicationId project.findProperty("wivrn_application_id") ?: "org.meumeu.wivrn"
```

That line is semantically null upstream, so offer it upstream; if they take it, our rebase surface on that file returns to zero.

**Why not `-Psuffix='.hydra'`** (which two proposals preferred because it needs no source edit at all): it leaves us squatting in `org.meumeu.*`, a namespace we do not own. A Meta app id is bound to a package name, so a namespace we do not own closes the Meta release channel, which section 3 argues is the best delivery path available. One semantically-null line now is cheaper than a second forced reinstall later plus a blocked route.

The build still passes `-Psuffix=""` explicitly. Every WiVRn build type appends a suffix unless told otherwise (`release` and `debug` default to `.local`), so a build that passes nothing ships as `com.experiencenet.hydraxr.local`.

Build properties for the release build, all of them already parameterised upstream:

| Property | Value |
|---|---|
| `-Pwivrn_application_id` | `com.experiencenet.hydraxr` |
| `-Psuffix` | `""` |
| `-Pwivrn_app_name` | `Hydra XR` |
| `-Pwivrn_css` | `images/wivrn-release.css` |
| `-Pwivrn_version` | tag minus the `client-v` prefix, e.g. `0.1.0` |
| `-Pwivrn_versionCode` | `MAJOR*10000 + MINOR*100 + PATCH` |

### 2.2 versionCode

Upstream hardcodes `VERSION_CODE=1` for every non-Oculus build (`/home/claude-user/hydra-wivrn/.github/workflows/Build.yml:254`) and `build.gradle` defaults it to 1. Two different builds are then indistinguishable to Android and to any MDM, and the second install silently does nothing. The formula above fixes it and leaves deliberate gaps, because Android refuses a versionCode downgrade: an on-device rollback is always a roll-forward rebuild of the older tree under a higher tag.

### 2.3 Flavors

None. WiVRn has no product flavors, only build types (`release`, `releaseDebuggable`, `oculus`, `debug`, `debugWithValidationLayer`). We ship `assembleRelease` only. Note that `/home/claude-user/hydra-wivrn/docs/runbooks/runbook.md:41` says `./gradlew assembleStandardRelease`, which is a task that does not exist; fix that line in the same PR.

The `oculus` build type gets built only if and when the Meta release channel lane in 3.2 goes ahead. It is the one that sets `com.oculus.supportedDevices` to `quest2|quest3|questpro` instead of `all`.

### 2.4 Signing

Sign with `apksigner` **after** the build, in CI, the way `/home/claude-user/hydraheadquest/.github/workflows/release.yml:81-100` does. Do not use WiVRn's Gradle `signingConfigs` block.

Reasons: WiVRn's signing config fixes `storeFile` to `ks.keystore` at the repo root, `keyAlias` to `default_key`, and forces one password for both store and key. `apksigner` keeps all of that configurable from secrets, writes no `gradle.properties` to disk, and matches the house precedent.

New GitHub secrets on `cederikdotcom/hydra-wivrn`:

```
HYDRAXR_KEYSTORE_BASE64
HYDRAXR_KEYSTORE_PASSWORD
HYDRAXR_KEY_ALIAS
HYDRAXR_KEY_PASSWORD
```

Deliberately **not** upstream's names `APK_KEYSTORE_BASE64` and `APK_SIGNINGKEYPASSWORD`. Upstream's `Build.yml` runs on every push to every branch; reusing those names would sign every branch build with the production key and park it in a public-ish artifact for 30 days.

Unlike hydraheadquest, **the job fails when the keystore secret is absent**. Use the same `has_keystore` step-output pattern (the secrets context is not allowed in step-level `if`), but the false branch exits 1 instead of renaming an unsigned APK. An unsigned artifact must never be able to reach the publish step.

Add a pre-publish assertion, cheap and load-bearing:

```bash
$ANDROID_HOME/build-tools/34.0.0/aapt2 dump packagename dist/hydraxr.apk \
  | grep -qx 'com.experiencenet.hydraxr' || exit 1
```

That makes shipping a `.local` or `.github.testing` build to production structurally impossible, not merely unlikely.

### 2.5 Keystore custody

New keystore, not the hydraheadquest one. The argument is blast radius: one leaked key must not force an uninstall and reinstall of both the kiosk and the streaming client.

Create it the way the hydraheadquest key was created (`/home/claude-user/hydraheadquest/docs/runbooks/runbook.md:82-96`): PKCS12, RSA 4096, self-signed, 30 year validity, alias `hydraxr`.

- Master copy: `~/.hydraxr/release.p12`, mode 600, with `~/.hydraxr/release-keystore-password.txt`, mode 600.
- Backup: Hetzner Storage Box u645590 (SSH port 23), `hydramdm-backups/hydraxr-signing/`, alongside the existing `hydraheadquest-signing/`.
- **Restore drill, mandatory before the first signed build ships.** Restore the backup into a scratch directory, and check that `apksigner verify --print-certs` on the published APK reports the same signer SHA-256 as the restored keystore's certificate. A backup that has never been restored is a claim, not a control. Do the same drill for the existing hydraheadquest key while you are there; it has never been restored either.

Record the signer SHA-256 in the hydra-wivrn runbook. Section 5 puts it in the per-head reported inventory, which is how we answer "does this headset still carry the debug build" without a site visit, on a platform where `adb screencap` returns 0 bytes.

### 2.6 Rotation: there is none, and a changed key is a wipe

**An Android app signed with a different key cannot upgrade in place.** Changing the signing identity of an installed package means uninstall then reinstall. Uninstall destroys app data, and WiVRn's app data is its pairing state (the PIN-derived keypair, `/home/claude-user/hydra-wivrn/common/secrets.cpp:24-27`).

Two consequences, both deliberate:

1. The first Hydra-signed install on the lab Quest is a destructive replacement of the debug-signed `org.meumeu.wivrn.github.testing` build. That is unavoidable and it is also a different package id, so both will be present until we remove the old one explicitly (rollout step 6).
2. **We make pairing disposable, so that a lost key costs no human input.** Every launch hands the client a fresh `wivrn://:PIN@host:port` URI with a server-minted PIN (section 4.4). The client therefore keeps no state we care about. A forced uninstall and reinstall of every headset then costs one MDM push per device and zero re-pairing at a venue. This is the single best mitigation anyone proposed for the lost-key scenario, and it is why the key custody rules above are a control and not a panic.

---

## 3. Distribution channel

### 3.1 The honest position

Nothing in this estate has ever installed an app on a Quest by any means other than `adb` sideload, and nothing has ever been published to `releases.experiencenet.com` for any Android project (`curl https://releases.experiencenet.com/hydraheadquest/production/latest.json` returns 404 today). Both the hosting half and the install half are unproven. The hosting half is a day of work. The install half needs a **Meta organisation that does not exist and that nobody has started**, and that is the long pole for the whole Quest fleet, not only for WiVRn.

So: hosting is settled here, install is pursued on two tracks off one Meta organisation application, and the lab lane keeps us moving in the meantime.

### 3.2 Hosting: hydrarelease, new project, constant filename

Publish to a **new** project `hydrawivrnclient`. Do not reuse `hydrawivrn`: `/home/claude-user/hydracluster/recipes/wivrn-arch.yaml:11-25` reads `hydrawivrn/production/latest.json` to pick the **server** pacman version, and there is one `latest.json` per project and channel. An APK published there would hand an Android version string to pacman on every body and break provisioning silently.

Publish two filenames per release, from the same CI step:

- `hydraxr.apk` (constant)
- `hydraxr-$VERSION.apk` (provenance)

The constant name is what makes the permanent install URL work. `handleFileRedirect` in `/home/claude-user/hydrarelease/internal/api/server.go:190-201` resolves a `latest` **version** segment server-side, but it does not resolve the filename. So:

```
https://releases.experiencenet.com/hydrawivrnclient/production/latest/hydraxr.apk
```

is a URL an MDM is configured with exactly once. Note that the field's earlier "verified live" claim about this was wrong: the only artifact ever published to `hydrawivrn` embeds its version in the filename, so `/latest/<constant-name>` has never once been exercised in this estate. Rollout step 0 exercises it with hydraheadquest before we depend on it.

Publish with the existing contract, same as `/home/claude-user/hydraheadquest/.github/workflows/release.yml:145-175`:

```
POST https://releases.experiencenet.com/api/v1/publish/hydrawivrnclient/<channel>/<version>/<file>
Authorization: Bearer $HYDRARELEASE_PUBLISH_TOKEN
POST .../finalize
```

Channel is `staging` when the tag contains `-rc` or `-beta`, otherwise `production`. Only `production` and `staging` exist; `handlers_publish.go:23` hard-limits them.

**Use the two channels as a canary ring.** The lab headset is configured permanently against `staging/latest`, venue headsets against `production/latest`. Promotion is a deliberate tag, not a flag. This is free and it is the only rollback we have, because Android refuses a versionCode downgrade.

Downloads are unauthenticated GETs (`/home/claude-user/hydrarelease/internal/api/server.go:130-139` registers the download routes with no auth wrapper), which is what makes them usable by an MDM.

One thing to check before handing the URL to Meta: hydrarelease **302s** to `bxl1.hydramirror.experiencenet.com`, which resolves to 141.227.136.199, the same edge box that runs the issue tracker. Two questions nobody asked: does Meta's installer follow the redirect, and is a single `bxl1.`-named host the right dereference target for a venue outside bxl1. See O-4.

### 3.3 Track A: Meta Horizon Managed Services, install from URL

What #544 records: HMS is free since 2026-02-20, enrols in Shared Mode with no per-device Meta accounts, its Device Manager "deploys private apps to managed devices from a URL to a self-hosted APK", and it can block the public store and restrict which apps users see. That last capability is also how we hide the app tiles a visitor must not tap.

What we do not have: a Meta organisation, an HMS account, an enrolled headset, and any evidence about whether enrolment wipes a device or whether developer mode survives it.

What has to be true for this to work, in priority order (all in the new Quest fleet distribution issue, not here):

1. A verified Meta business organisation exists.
2. HMS enrolment does not require wiping the one Quest 2 we own, or we accept the wipe.
3. An HMS-deployed hydraheadquest can hold the WireGuard tunnel. The tunnel today exists because somebody ran `adb appops set <pkg> ACTIVATE_VPN allow`, which needs developer mode, which needs adb. If HMS excludes or resets developer mode and cannot pre-grant `ACTIVATE_VPN`, the head never heartbeats and every design in this field collapses. **This is a gate, not a risk item.** Note that the WiVRn client itself needs no VPN permission; it inherits the device-wide tunnel the kiosk holds. The gate is on hydraheadquest.
4. HMS's fetcher follows our 302.

### 3.4 Track B: our own Meta app id and a private release channel

The whole field missed this and it has working code in the tree. `/home/claude-user/hydra-wivrn/.github/workflows/Build.yml:317-334` already runs:

```
ovr-platform-util upload-quest-build --app-id <id> --token $OCULUS_TOKEN \
  --apk apk/WiVRn-oculus.apk --channel $CHANNEL
```

Everyone's only conclusion was defensive, that we must never set `OCULUS_TOKEN` or we push to upstream's app id 7959676140827574. Invert it. Create a **Hydra** Meta app id under `com.experiencenet.hydraxr`, set our own token, and push the `oculus` build type to a non-LIVE **release channel**. A release channel is an invite list of Meta accounts. Builds arrive through the normal store UI with normal background auto-update: no developer mode, no unknown sources, no cable, no MDM, and no store review, because review gates LIVE and not channels.

That is the only path in this whole field that gives silent updates on a stock consumer Quest without an MDM. Its prerequisite is the same Meta organisation HMS needs, so it costs nothing extra to pursue. It composes with HMS rather than competing: HMS for device policy and tile hiding, the channel for app delivery.

Pursue **both** off one organisation application. Expect the channel to land first, because it has fewer moving parts.

This is also why section 2.1 spends a line of rebase surface on a Hydra-owned package namespace. A Meta app id cannot be bound to `org.meumeu.wivrn.hydra`.

### 3.5 Lab lane: hydraadb, and what it is not

`hydraadb` is an Incus scale on pi-node-004-nvme reaching the Quest at 10.10.200.7:5555 over the mesh. It stays, for developer headsets only. Write these disqualifying facts into `/home/claude-user/hydraadb/docs/runbooks/runbook.md` under a heading that says **lab lane, never a venue**:

- `adb tcpip 5555` does not survive a Quest reboot, and re-arming needs USB plus an in-headset "Allow USB debugging" tap. There is no network-only recovery.
- The headset answers only while awake or worn.
- `adb ... screencap` returns a 0-byte file on Horizon OS, so an install cannot be verified visually from here.
- The container image has no `curl` or `wget` (`/home/claude-user/hydraadb/Dockerfile` installs `adb`, `ca-certificates`, `iputils-ping`). Stage APKs with `incus file push`, not by fetching inside the container.

Developer headset install and upgrade, for the record:

```bash
incus file push hydraxr.apk hydraadb/tmp/hydraxr.apk
incus exec hydraadb -- adb -s 10.10.200.7:5555 install -r /tmp/hydraxr.apk
incus exec hydraadb -- adb -s 10.10.200.7:5555 shell pm list packages | grep hydraxr
incus exec hydraadb -- adb -s 10.10.200.7:5555 shell dumpsys package com.experiencenet.hydraxr \
  | grep -E 'versionCode|versionName'
```

### 3.6 Security cleanup, not hygiene

Delete the `pyrowave-test-1` prerelease on `cederikdotcom/hydra-wivrn`. It is a live, public, unauthenticated 32.8 MB download of a **debug-signed** APK of our fork. Debug signing means the private half of the signing key ships inside every Android SDK installation, so anyone who can reach that headset over adb can build something Android considers the same app and upgrade it in place. The build is also `debuggable`, which makes `run-as` work and the app's private data readable, including the PIN-derived pairing material. The public download advertises the target. Treat this as a finding, and do it in rollout step 3 rather than at the end.

---

## 4. Coexistence with hydraheadquest

### 4.1 What runs on a headset

Three packages, at most:

| Package | Role | Holds an OpenXR session |
|---|---|---|
| `com.experiencenet.hydraheadquest` | kiosk, enrolment, catalog, Moonlight | Only inside `GameXR`, its own immersive streaming activity |
| `alvr.client.stable` | ALVR XR driver client | Yes, while streaming |
| `com.experiencenet.hydraxr` | WiVRn XR driver client | Yes, while streaming |

**The policy is: at most one XR driver client is ever launched, and hydraheadquest decides which one.** Both may be installed.

We explicitly reject the competing policy of "exactly one XR client installed per headset". It contradicts master #734, which frames WiVRn as a driver *alongside* ALVR; it statically partitions the body pool by driver per district and venue, which works against discovery-based body selection; and it makes the head-side driver matching in section 5 vestigial, because a head that can only speak one driver has nothing to match. It also has no answer during any migration window.

The kiosk's own `GameXR` is not a contender in practice. Every Hydra kiosk activity is declared `pvr.app.type=2d` with an explicit `<layout defaultWidth="1024dp" defaultHeight="640dp">` Horizon panel, and `GameXR` is `android:exported="false"`. The activity that performs the launch is a 2D panel and does not contend. A driver session and a Moonlight XR session are different `HydraState` paths and are never both live.

### 4.2 The launcher becomes data, not a constant

Today `/home/claude-user/hydraheadquest/app/src/main/java/com/limelight/hydra/AlvrLauncher.kt` hardcodes `ALVR_PACKAGE = "alvr.client.stable"` and `ALVR_ACTIVITY = "android.app.NativeActivity"`. Replace it with a driver table implementing the existing `HydraState.XrHooks` seam (`isXrClientInstalled()`, `launchXrClient()`), keyed on the driver string the cluster session carries:

```kotlin
private val XR_CLIENTS = mapOf(
  "alvr"  to XrClientSpec("alvr.client.stable",        "android.app.NativeActivity"),
  "wivrn" to XrClientSpec("com.experiencenet.hydraxr", "org.meumeu.wivrn.MainActivity"),
)
```

Three launch rules, all hardware-verified and all still binding (`AlvrLauncher.kt:16-23`):

1. The intent must carry the category `com.oculus.intent.category.VR` or vrshell rejects the placement.
2. The launch must come from a **live foreground activity**. A background or application-context start is refused by SWMS, and vrshell retries it about 23 s later.
3. Horizon OS silently drops activity starts while the headset is asleep. Launch only when worn, or send `KEYCODE_WAKEUP` first.

Package visibility. `/home/claude-user/hydraheadquest/app/src/main/AndroidManifest.xml:74-76` must list **three** packages:

```xml
<queries>
  <package android:name="alvr.client.stable" />
  <package android:name="com.experiencenet.hydraxr" />
  <!-- legacy debug build: visible so the fleet can REPORT it, never launched -->
  <package android:name="org.meumeu.wivrn.github.testing" />
</queries>
```

The third entry is what makes the migration auditable instead of hopeful. The driver table never maps to it, and section 5 reports it when present.

### 4.3 How a headset ends up in the right lane

1. The head reports which XR clients it has and which drivers it can speak (section 5), every 30 s tick.
2. The head asks `GET /api/v1/bodies/eligible?xr_driver=<driver>` and claims with `POST /api/v1/heads/{id}/xr-session` carrying `driver`. Today it sends neither and every Quest request silently resolves to `alvr` (`HydraState.kt:884` re-filters on `it.xrDrivers.contains("alvr")`, and `postXrSession` omits the key entirely). Fix both.
3. The cluster matches on the driver, which it already does (`/home/claude-user/hydracluster/pkg/api/handlers_xr.go:161`).
4. The kiosk launches the client named by the driver in the session it created, and no other.

The exclusivity record stays where it is: one `NodeXRSession` per body. The head-side guarantee is that the kiosk is the only thing that launches a client, and it launches exactly the one it asked for.

### 4.4 The handoff, and the PIN

mDNS `_wivrn._tcp.local.` does not cross the WireGuard mesh, so the deep link is the **only** way to aim the client at a body:

```
wivrn://:<PIN>@<body-mesh-ip>:<port>
```

`/home/claude-user/hydra-wivrn/client/application.cpp:1530-1548` parses host, port, `tcp_only` (scheme `wivrn+tcp`) and `pin` from the URI password field. `/home/claude-user/hydra-wivrn/client/scenes/lobby.cpp:911-921` honours the intent only when idle, and line 916 pre-fills `pin_request` so the visitor never sees a pairing prompt.

**Always cold-start. Never re-aim a running client.** An intent delivered while streaming is buffered and ignored until the lobby is current again, and re-aiming currently risks a crash: `Java_org_meumeu_wivrn_MainActivity_onNewIntent` (`application.cpp:1508-1518`) calls `set_server_uri` with no try/catch, while the cold-start path at `:1565-1577` wraps the identical throwing call. **Fix that in the fork and offer it upstream** (rollout step 3), before anything depends on `onNewIntent`.

Where the PIN comes from, which no proposal specified and which sits on the critical path:

- The WiVRn server generates and publishes the PIN over D-Bus (`/home/claude-user/hydra-wivrn/server/main.cpp:584-621`, `wivrn_server_set_pin`) and **refuses to enable pairing while a session is active** (`main.cpp:683`).
- hydrabody sets a cluster-supplied PIN over that D-Bus interface while the XR session is in `arming`, which is exactly the window before the client connects.
- The cluster mints the PIN, stores it on `NodeXRSession`, relays it to the body in the existing `clusterapi.XRSessionDirective` (`/home/claude-user/hydraclusterapi/types.go:58-67`, new field `xr_pin`), and returns it to the head in the XR session response so the kiosk can build the URI.

O-5 confirms hydrabody can drive that D-Bus call. If it cannot, the fallback is that hydrabody **reads** the server-generated PIN and reports it upward in the body status, which is strictly easier and needs no setter.

### 4.5 Getting the visitor back to the kiosk

This is the weakest part of every proposal in the field and it needs an explicit answer.

The WiVRn client contains no code that launches another app (the only `startActivityForResult` is the file picker at `client/android/intent.cpp:54`). When the runtime signals `EXITING` or `LOSS_PENDING`, `application::run()` unwinds and `/home/claude-user/hydra-wivrn/client/main.cpp:71-87` calls `ANativeActivity_finish` then `exit(0)`. The process dies, the visitor is left looking at whatever vrshell shows next, and the kiosk cannot legally start a foreground activity from the background to rescue them.

Primary mechanism, a fork-local patch (roughly 30 lines, upstreamable as "return to launcher"):

- `client/java/MainActivity.java` gains `public void launchReturnComponent()`, which reads a string extra `hydra_return_component` off the launch intent, and if present starts that `ComponentName` with `FLAG_ACTIVITY_NEW_TASK` and category `com.oculus.intent.category.VR`.
- `client/main.cpp` calls it through JNI immediately before `ANativeActivity_finish`, while the activity is still alive and still foreground, which is what satisfies the SWMS foreground rule.
- The kiosk passes `hydra_return_component = com.experiencenet.hydraheadquest/com.limelight.hydra.HydraLaunchActivity` on the launch intent. `HydraLaunchActivity` is already exported (`main AndroidManifest.xml:317-318`).

Fallback, which we keep because the patch is new code on hardware we have not tested: the kiosk notices the session ended on its next 30 s tick and starts its own `HydraLaunchActivity` anyway. That start is SWMS-rejected and lands via the vrshell retry about 23 s later. Worst case the visitor waits under a minute. State that in the testbook rather than pretending it is instant.

`HydraTunnelService` (the `specialUse` foreground service at `main AndroidManifest.xml:301-311`) must stay alive throughout. It exists because a fullscreen XR app gets the kiosk reaped by Android LMK, taking the mesh tunnel with it. That was the v0.7.2 fix and it still applies with a second client.

### 4.6 The escape hatch, if the hardware says otherwise

Nobody has ever observed what Horizon OS does when a second immersive app requests a session while a first holds one. E-1 settles it. Two outcomes:

- The incumbent receives `STOPPING` and is backgrounded. Handoff is free and everything above is more machinery than we need, which is fine, it is additive.
- The incumbent receives `EXITING` or `LOSS_PENDING`, dies, and the transition is ugly or unrecoverable.

If the second outcome makes cooperative handoff unworkable, fall back to the **one-APK absorption** design: ship the WiVRn client as a prebuilt lane bundle (`jniLibs/arm64-v8a/libwivrn.so` plus assets, res and the three `org.meumeu.wivrn` Java files, built by hydra-wivrn CI which already has NDK 29, CMake 3.31.5, glslang, spirv-tools, rsvg-convert, gettext, the KTX deb and gltf-transform) inside `com.experiencenet.hydraheadquest`, running under `android:process=":wivrn"`. Its payoff is real and is available to no other design: **an app cannot force-stop another app, but it can kill its own other process.** `android.os.Process.killProcess(pid)` within one UID turns "cooperative handoff we hope works" into a hard guarantee that the kiosk can reclaim the headset. Its cost is a build-system merge that AGP makes awkward (hydraheadquest is ndkBuild on NDK 27) and one shared versionCode series. We do not pay that cost unless E-1 says we must.

---

## 5. Version and capability reporting

Today the cluster knows nothing. `/home/claude-user/hydracluster/pkg/api/handlers_head.go:20-37` has no client or version field, `diagnostics["version"]` is the kiosk's own version, and `diagnostics["xr_client"]` is the hardcoded string literal `"alvr"` set by `HydraState.kt:1211-1216` whenever the state is `ArmingXr` or `StreamingXr`, whether or not any client is installed. The live record for `quest-head-36afcd` shows exactly this: the WiVRn client that is physically on that headset is invisible.

Split **reported inventory** from **declared capability**. They behave differently when an operator uninstalls a client, and they need different durability.

### 5.1 Reported inventory rides in diagnostics

No cluster wire change. The head probes `PackageManager` over its `<queries>` list every 30 s tick and puts one key in the existing free-form map:

```
xr_clients = alvr.client.stable=20.14.1/8f3a1c02;com.experiencenet.hydraxr=0.1.0/1c9d44b7
```

Format is `package=versionName/signerSha256First8`, semicolon separated, present packages only. The signer SHA-256 prefix is the point: it is how we answer "does this headset still carry the debug-signed test build" from a desk, on a platform where `screencap` returns 0 bytes. A head still carrying `org.meumeu.wivrn.github.testing` reports it, with a debug signer hash that stands out.

Diagnostics is safe for this because `SetHeadDiagnostics` (`/home/claude-user/hydracluster/pkg/store/store.go:567-574`) replaces the whole map and the head resends the whole map every tick. It is not safe for anything the cluster must be able to rely on between ticks, which is why capability is not stored here.

### 5.2 Declared capability is a typed field

Add a fourth field to `PUT /api/v1/heads/{id}` (`handlers_head.go:122-190`, which today reads only `status`, `diagnostics`, `body_id`):

```json
{ "xr_drivers": ["alvr", "wivrn"] }
```

- Store: `store.Node.HeadXRDrivers []string`, yaml `head_xr_drivers` (`/home/claude-user/hydracluster/pkg/store/store.go`).
- Echo: `xr_drivers` in `headConfigResponse` (`handlers_head.go:20-37`).
- **Absent must mean `["alvr"]`.** That matches the existing defaults at `handlers_xr.go:108-112` and `handlers_api.go:900-903`, and it is the house rule: every XR addition so far is additive with a default that reproduces the old behaviour. Unknown fields are already silently ignored by `encoding/json`, so an older head PUTting without the field keeps working unchanged.
- The head **derives** this list from the probe in 5.1, not from an admin field and not from an enrolment-time declaration. Either of those lies the moment an operator uninstalls a client.

### 5.3 Tie it to the driver matching that already exists

The cluster half is done; the head half is not.

- `GET /api/v1/bodies/eligible?xr_driver=<d>` exists, defaults to `alvr`. The head must start sending it. Delete the client-side re-filter at `HydraState.kt:884`.
- `POST /api/v1/heads/{id}/xr-session` accepts optional `driver`, defaults to `alvr`. The head must start sending it.
- Keep the two defaults in step. If discovery accepts a wider set than claim, a head finds a body and is then refused with `not_xr_capable` (`/home/claude-user/hydracluster/docs/runbooks/body-selection.md:100-105`).

Two cluster changes are required or a WiVRn session literally cannot be opened:

1. **`client_hostname` becomes driver-conditional.** `handlers_xr.go:168-177` returns 400 `client_hostname is required (set xr_client_hostname on the head)` unconditionally. That field is ALVR-shaped (`0529.client`). WiVRn has no client-hostname concept; it pairs by PIN. Require it for `driver == "alvr"` only. Also drop the pre-flight abort at `HydraState.kt:823-830` for non-ALVR drivers.
2. **New `xr_pin` on `NodeXRSession` and on `clusterapi.XRSessionDirective`**, per 4.4.

The head-type gate at `handlers_xr.go:139` (`headType(*head) != "hydraheadquest"`) stays as is.

### 5.4 Staleness is advisory, with one hard refusal

Add `hydraheadquest` and `hydrawivrnclient` to `pollableRoles` in `/home/claude-user/hydracluster/pkg/versions/poller.go:12-34`, and surface `client_update_available` on the head config response, following the `BodyUpdateAvailable` precedent for bodies.

Advisory only. Do **not** gate an XR session on a stale client: by the time that check runs a visitor is already wearing the headset, and a refused session is worse than an old one. There is no body-side precedent for gating either.

The one hard refusal: a session request for a driver the head did not report a client for. That session cannot succeed anyway, and failing at claim time is strictly better than failing in the headset.

Normalisation trap for whoever writes the URL builder: `latest.json` advertises `"1.2.3"` while the artifact path uses `"v1.2.3"`. `normalizeVersion` re-adds the `v` and deliberately leaves the literal `latest` alone.

### 5.5 Admin UI

`/home/claude-user/hydracluster/internal/web/templates/node_detail.html` renders neither head status nor head diagnostics today. Add `xr_drivers`, `xr_clients` and `client_update_available` to the head section. Otherwise all of the above is visible only through raw JSON.

### 5.6 Contract

`/home/claude-user/hydraheadquest/docs/hydra-api-contract.md` is authoritative for the Quest client. Every field above lands there in the same PR as the code, on both sides.

---

## 6. Rollout

Each step is independently verifiable and stands on its own. Steps 0, 1 and 2 can run in parallel and all three can start this week.

**Step 0 (this week, no design dependency, roughly one hour).** Set `HYDRARELEASE_PUBLISH_TOKEN` on `cederikdotcom/hydraheadquest`. It currently holds the four `ANDROID_*` secrets and not this one, which is why nothing has ever been published for that project. Add a constant-filename artifact `hydraheadquest.apk` alongside the versioned one in `release.yml`. Cut `v0.8.3-rc1`.
*Verify:* `curl https://releases.experiencenet.com/hydraheadquest/staging/latest.json` returns 200, and `curl -IL https://releases.experiencenet.com/hydraheadquest/staging/latest/hydraheadquest.apk` follows the 302 to `bxl1.hydramirror.experiencenet.com` and ends in a 200 of the right byte count. This de-risks every distribution design in the field, not just this one, and it exercises the `/latest/<constant-name>` convention that has never been used here.

**Step 1 (this week, roughly one hour on hardware).** The experiment that decides section 4. First check the Quest still answers: cluster exec on `node-50ab5309`, `incus exec hydraadb -- adb devices`. If it does not, that is a USB re-arm with a Meta account and a person, and it goes on the calendar. Then, with both APKs installed:

```
adb shell am start -a android.intent.action.VIEW \
  -d "wivrn://10.10.200.x" \
  -c com.oculus.intent.category.VR \
  org.meumeu.wivrn.github.testing
```

while an ALVR session holds the headset, and read logcat for the `XR_SESSION_STATE_*` the incumbent receives.
*Verify:* a logcat excerpt pasted into #739 naming `STOPPING` or `EXITING`. That excerpt decides whether section 4.5 or section 4.6 is the plan.

**Step 2 (this week, external clock starts).** File the two split issues. Start the Meta business organisation application. Read Meta's current managed services and release channel documentation to answer O-2 and O-3 from a browser, with no account. **Do not order Quest 3/3S until the business-channel SKU question (O-8) is answered**; ordering consumer units may cost a wipe per device, exactly the trap #518 documents for the three iPads.

**Step 3 (half a day). Fork hygiene.** Delete `/home/claude-user/hydra-wivrn/.github/workflows/APK-Release.yml` (it checks out `repository: 'WiVRn/WiVRn'` and can never build the pyrowave branch). Then set the repo variable `APK_ONLY=1`, which gates every `Build.yml` job off (`Build.yml` jobs run only when `vars.APK_ONLY == ''`). Order matters: delete first, then set. Fix the missing try/catch in `onNewIntent` (`client/application.cpp:1508-1518`) and open it upstream. Fix `docs/runbooks/runbook.md:41` (`assembleStandardRelease` does not exist). Delete the `pyrowave-test-1` prerelease.
*Verify:* the next push to `pyrowave` runs zero workflows. Today it runs Build, Format and Locales, and a master sync runs Build for 51 minutes. Also `gh release list` no longer shows `pyrowave-test-1`.

**Step 4 (two hours). Keystore.** Create it, back it up to Storage Box u645590 under `hydramdm-backups/hydraxr-signing/`, set the four `HYDRAXR_*` secrets, and run the restore drill for both the new key and the existing hydraheadquest key.
*Verify:* `apksigner verify --print-certs` against a throwaway signed APK reports the same SHA-256 as the restored backup, and the hash is written into the hydra-wivrn runbook.

**Step 5 (one day). `client-release.yml` in hydra-wivrn.** Tag trigger `client-v*` (never `v*`, which is upstream's namespace). ubuntu-24.04; install `glslang-tools spirv-tools librsvg2-bin gettext`, the KTX-Software `.deb`, and `npm install --global @gltf-transform/cli`; NDK 29.0.14206865, CMake 3.31.5. Build `assembleRelease` with the six `-P` properties from 2.1, `zipalign -f -p 4`, `apksigner sign`, the aapt2 applicationId assertion, then publish both filenames to `hydrawivrnclient` plus `/finalize`. Fail the job if the keystore secret is absent. Never set `OCULUS_TOKEN` on this repo until step 9, and write that down next to the secret list. Tag `client-v0.1.0-rc1`.
*Verify:* `https://releases.experiencenet.com/hydrawivrnclient/staging/latest/hydraxr.apk` returns 200; `aapt2 dump badging` on the download reports `com.experiencenet.hydraxr` and versionCode 100; `apksigner verify --print-certs` matches step 4. Budget about 6 minutes for the Android leg on a hosted runner; that number is measured, not guessed.

**Step 6 (one hour). Lab install.** Through hydraadb: uninstall `org.meumeu.wivrn.github.testing`, install `com.experiencenet.hydraxr` from the staging URL.
*Verify:* `pm list packages | grep -E 'wivrn|hydraxr'` shows only the new id, and a PyroWave stream from `spicy-cactus-76` still runs end to end from the new package.

**Step 7 (two to three days). Cluster and body.** `xr_pin` on `NodeXRSession` and the directive; hydrabody drives or reads the WiVRn D-Bus PIN during `arming`; `client_hostname` becomes driver-conditional; `HeadXRDrivers` field, store, PUT and response; `pollableRoles` additions and `client_update_available`; `node_detail.html`. Update `/home/claude-user/hydraheadquest/docs/hydra-api-contract.md`. hydracluster default branch is `main`.
*Verify:* `PUT /api/v1/heads/{id}` with `xr_drivers` round-trips through `GET`; a head record with no `xr_drivers` still resolves to `alvr` on both discovery and claim; `POST .../xr-session` with `driver=wivrn` and no `client_hostname` returns 200 and a `xr_pin`.

**Step 8 (two to three days). hydraheadquest v0.9.0.** The three-package `<queries>` block; the driver table launcher; send `driver` and `xr_driver`; probe and report `xr_clients` and `xr_drivers`; build the `wivrn://:PIN@host:port` URI; the return-to-kiosk path chosen by step 1. Derive versionCode from the tag (it is hand-maintained at `app/build.gradle:16`, value 14, today).
*Verify:* on the lab Quest, a full visitor journey: kiosk to WiVRn client to stream to end to kiosk, with no adb involved after the install, and the cluster head record showing both clients with their signer hashes.

**Step 9 (blocked on Meta, unbounded).** With the organisation verified: enrol one headset; test HMS install-from-URL against the production URL, watching whether the fetcher follows the 302; test a private release channel upload with our own app id and `OCULUS_TOKEN`; confirm an HMS-deployed hydraheadquest can hold the WireGuard tunnel. Then write `/home/claude-user/hydramdm/docs/runbooks/quest.md`, which both the hydraheadquest runbook and #544 already point at and which does not exist.

**Step 10.** Quest 3/3S on the right SKU, the dedicated 6 GHz AP with a route to the body and to the mesh, PyroWave validation at the venue. Lab device pinned to `staging`, venue devices to `production`.

---

## 7. Open questions

Answer O-1 through O-3 before step 1 commits anyone to a lane. The rest are cheap and can run alongside.

**O-1. What does Horizon OS do when a second immersive app requests a session?**
The brief asserts one session at a time, and no one has ever observed it. The distinction that matters is not "one session" but which event the incumbent receives: `STOPPING` (free handoff, section 4.5 is over-engineered but harmless) or `EXITING` (WiVRn calls `exit(0)`, section 4.5 is required, and section 4.6 may be).
*Cheapest answer:* rollout step 1. Twenty minutes, hardware we own, no money.

**O-2. Does HMS enrolment wipe the device, and does developer mode survive it?**
A wipe destroys developer mode, both sideloaded apps and pairing on the only headset we have.
*Cheapest answer:* read Meta's managed services documentation in a browser today. No account needed.

**O-3. Can an HMS-deployed hydraheadquest be granted `ACTIVATE_VPN`?**
The mesh tunnel today exists only because someone ran `adb appops set <pkg> ACTIVATE_VPN allow`, which needs developer mode, which needs adb. If the managed lane cannot pre-grant it, the head never heartbeats and every design here collapses.
*Cheapest answer:* same documentation read, then confirmed on the first enrolled headset. This is a gate on the Quest fleet distribution issue.

**O-4. Does Meta's installer follow our 302, and is `bxl1.hydramirror.experiencenet.com` the right dereference target?**
hydrarelease redirects rather than serving bytes, and that redirect resolves to 141.227.136.199, the same edge box as the issue tracker. Nobody asked what a `bxl1.`-named host means for a venue outside bxl1, or whether the redirect is chosen per client.
*Cheapest answer:* step 0 proves the redirect chain from a normal client today. The Meta-side half waits for step 9. If Meta will not follow it, add a direct-serve path to hydrarelease.

**O-5. Can hydrabody set or read the WiVRn server PIN?**
The PIN is the whole pairing-free visitor flow. `server/main.cpp:584-621` publishes it over D-Bus and `main.cpp:683` refuses pairing while a session is active.
*Cheapest answer:* one hour reading the hydrabody side of the WiVRn D-Bus interface. No hardware.

**O-6. Is a Meta developer organisation really needed for developer mode?**
This claim is sourced entirely to our own runbook written 2026-08-21 and never re-verified, and it is what disqualifies the lab lane.
*Cheapest answer:* ten minutes with a phone and the headset.

**O-7. Is the dedicated 6 GHz AP an isolated segment?**
A Quest has one radio and one association. Today the head reaches the cluster over WireGuard over the venue SSID (`wifi_ssid: Rotar`, `local_ip 192.168.68.50`). Moving it to the PyroWave AP moves the heartbeat, the command poll, the MDM check-in and every APK download onto that AP. If that AP is isolated, which is how most people build "dedicated AP, one client", the headset is off the mesh for the whole session.
*Cheapest answer:* state it as a venue network requirement now: the PyroWave segment must route to the body and to the WireGuard endpoint. Confirm on the first venue build.

**O-8. Does Meta sell headsets through a business channel with the managed-services entitlement attached?**
If yes, business-provisioned devices are enrolment-ready at first boot. If we buy consumer units, enrolment may need a wipe per device.
*Cheapest answer:* one call or one page on Meta's business site, before the Quest 3/3S order is placed. This expires the moment somebody clicks buy, and it is worth more than any single technical choice in this document.

Two things previously listed as unknown are settled and should not be re-investigated: the Android build costs about 6 minutes per leg on a hosted runner (the hour-long leg in our fork's CI is the flatpak server build, not Android), and the release download lane works end to end from the public internet.

---

## 8. Risks accepted

**A lost signing key means uninstall and reinstall on every headset.** Mitigated, not eliminated: pairing is disposable because every launch hands the client a fresh PIN, so a forced reinstall costs one push per device and zero human input at a venue. The restore drill in step 4 is the control.

**There is no rollback.** Android refuses a versionCode downgrade, so recovering from a bad client is a roll-forward rebuild of the older tree under a higher tag. The staging channel pinned to the lab headset is the canary that keeps a bad build off the fleet in the first place. versionCode gaps make the roll-forward possible.

**The return-to-kiosk fallback can cost about 50 seconds.** If the fork-local patch in 4.5 does not work on hardware, the visitor waits for the vrshell retry. Documented in the testbook rather than hidden.

**Updates land when the headset is awake and on the mesh.** Horizon OS drops activity starts while asleep and a headset in a locker is not reachable. There is no interlock preventing a head from entering self-service while a 32 MB APK is landing. Android applies the update on the next app start, so an in-session update does not interrupt a visitor; we accept the window.

**We are validating on hardware the payload cannot run well on.** The fleet owns one Quest 2, which the codec ruling excludes, and it has been `node_status: offline` since 2026-09-15. Everything up to step 8 is validated on it anyway, because the distribution and reporting mechanics are hardware-independent. PyroWave performance is not proven until step 10.

**Horizon OS SDK declaration is unresolved for the WiVRn client.** hydraheadquest declares `horizonos:uses-horizonos-sdk` 74/77 because v77+ refuses immersive frames without it, and because a floor of 77 blocks install on Quest 2s still on v76. The WiVRn manifest declares none at all. Our only device is on v76, so the claim is untestable here. Decide it explicitly when a Quest 3/3S arrives; expect to add the declaration.

**GPLv3 keeps the fork public.** WiVRn is GPLv3 and so is hydraheadquest. Distributing a Hydra-branded binary obliges us to offer the corresponding source for that exact build to every recipient, which is satisfied only while `cederikdotcom/hydra-wivrn` stays public. That becomes a live constraint the moment anyone proposes a private fork or a whitelabel, and a whitelabel is already on the books under #672. Flag it there.

**Arbitration is cooperative, not enforced.** An app cannot force-stop another app. The guarantee is that hydraheadquest is the only thing that launches an XR client and it launches only the one the cluster session names. If step 1 shows that is not enough, section 4.6 is the answer, and it is a bigger change than anything else here.

**The steady-state install path depends on Meta, and Meta has not started.** This is the single biggest schedule risk, it is external, and it is unbounded. It is also why the Quest fleet distribution work is its own issue with its own owner: #739 can close without it.