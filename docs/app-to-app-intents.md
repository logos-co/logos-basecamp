# App-to-app intents in Logos Basecamp

*Ships with this release. Read this if you are writing a Logos app, reviewing the design, or deciding whether to depend on it yet.*

---

## 1. What this is

An app can ask for something to be done without knowing who will do it.

```js
logos.request("wallet.send", { to: "0xabc", amount: 12.5 }, function (res) {
    if (res.ok) console.log("sent", res.data.txHash);
    else        console.log("did not happen:", res.error);
});
```

The calling app never names a wallet, never links against one, and never learns which wallets are installed. Basecamp resolves the capability to an app that declared it, asks the user, brings that app forward, and routes the answer back to the caller alone.

What that buys is apps that compose without knowing about each other. A chat app gains the ability to send funds the day a wallet is installed — no release, no dependency, no agreement between the two teams beyond the name `wallet.send`. Remove the wallet and the chat app degrades to a clear `unavailable` rather than a broken build.

The mechanism is narrow on purpose, and every restriction is load-bearing:

- **A request names a capability, never a provider**, so an app cannot come to depend on one, nor use the request to discover what you have installed.
- **Exactly one provider services a request**, chosen by the user. No broadcast.
- **Every request terminates exactly once**, asynchronously, with a result.
- **The user is in the loop for every dispatch that crosses an app boundary**, shown only shell-drawn strings. Three dispatches skip the chooser by construction: an app requesting a capability it declares itself (internal navigation), the shell as provider, and the shell as requester with a single candidate.
- **Nothing crosses but plain data** — no object handles, no callbacks, no live references into another app's engine.

Two apps never talk directly; each talks to the shell. The caller's request id never leaves the caller's side — the shell mints a *separate* dispatch id and gives only that to the provider. So a provider cannot forge a result for a request it was not given, cannot enumerate other requests, and cannot discover that a request exists unless the user hands it one.

Nothing is signed yet, so a name is a claim rather than an identity. §6 sets out what that costs you.

---

## 2. What it covers

| Scenario | What happens |
|---|---|
| One provider installed | Still confirmed by the user — unless the shell is one of the two parties — then raised and handed the request |
| Several providers | Chooser lists them, sorted; only the chosen one ever hears about it |
| Nothing installed, catalog has one | Answers `unavailable`, and separately suggests installing it |
| Nothing anywhere | `unavailable`, on a timing floor so the speed reveals nothing |
| Caller never declared the intent | `not_declared`, immediately |
| Payload is the wrong shape | `bad_request` — fix what you sent rather than retrying |
| User dismisses the chooser | `cancelled`, distinct from "nobody was there" |
| Provider declares but ships no handler | `timeout` after 20s, with a log warning naming it |
| The shell is the provider | `basecamp.repositories.manage`, `basecamp.settings.open`, `basecamp.apps.open`, `basecamp.apps.launch` and the three `basecamp.packages.confirm_*` are serviced by Basecamp itself |
| A `basecamp://` link is clicked | Becomes an intent from a requester the shell mints — never the shell itself, so the chooser still applies (§7) |
| Requester is not on a restricted intent's list | `unavailable`, floored — indistinguishable from "nothing provides it" |

**Providers are `ui_qml` apps, by design.** Intents exist for *user-mediated* actions: the caller does not know who will service the request, and a human chooses. Core modules have no such problem — they already call each other directly by name through `LogosAPI`, with no chooser and no consent step, because nothing is being decided. A backend that needs another backend should make that call, not raise an intent.

**Not yet:** one provider per request; `cardinality: "all"` is reserved but unimplemented (§7). There is no "always use this app" — every ambiguous request raises the chooser (§7). Nothing crosses devices or process trees.

---

## 3. How it works

The system is split in two, and the split is the point:

- **`logos-view-module-runtime` — frozen.** `LogosIntent.h` (error codes, name grammar, payload rules, result envelope) plus the QML bridge. This is what apps compile against.
- **`logos-basecamp` — disposable.** Registry, broker, dialogs. All policy lives here and none in the frozen half, so when the core runtime takes over provider selection this can be thrown away and apps written today keep working.

The broker reaches the world through four seams — endpoint, presenter, chooser, installer — and knows nothing about widgets or app-name special cases. Each is faked in tests, so the whole policy layer is testable without a UI. The endpoint and chooser seams return *how many handlers received the prompt*; zero means nothing is on screen, and the broker fails closed rather than parking a request behind a dialog nobody mounted. The installer seam returns nothing: the request that prompted the suggestion was already answered `unavailable`, so a prompt that fails to mount leaves nothing in flight.

```
  submit ──► payload not canonical? ─────────────────► bad_request  (floored)
     │
     ├────► not declared? ────────────────────────────► not_declared (instant)
     ▼
  resolve ─► nobody installed ──────────────────────► unavailable  (floored)
     │                                                 └─ and, separately, the
     │                                                    shell may suggest an
     ▼                                                    install to the user
  Accepted ─► AwaitingChoice ─► Activating ─► Dispatched ─► result to caller
                   │                              │
                   └─ dismissed ► cancelled       └─ spec mismatch ► bad_request
```

A response is accepted only when the dispatch id is pending, the phase is `Dispatched`, **and** the responding endpoint is pointer-identical to the recorded provider. Pointer, not name — a reloaded app is a different object and must not inherit the old one's in-flight requests. A failed guard drops silently, so a wrong guess teaches an attacker nothing, not even that it was wrong.

**Bounds.** Broker-minted failures are held to a 400 ms floor, so "instant" cannot mean "nothing is installed". Activation times out at 45s. A dispatched request that reached no handler times out at 20s; one a handler accepted is bounded only by a 10-minute backstop, so someone reading the provider’s own confirmation is not being clocked.

**Payloads** must survive crossing between two QML engines, so they are plain data only: ≤8 levels deep, ≤1000 nodes, strings ≤64 KB, keys ≤64 chars, integers within ±(2⁵³−1). No `QObject*` and no functions — that is what stops one app handing another a live handle into its engine. The same check runs on the result.

**Names** are `namespace.verb`: 2–4 dot-separated segments, 3–64 chars, lowercase with single underscores. Two namespaces are reserved and refused from any on-disk record: **`logos.*` is the platform's** — liblogos, logoscore, anything below the shell — and **`basecamp.*` is this shell's**. The split matters because Basecamp is one frontend among several possible ones: a capability of *this shell* has no business claiming the name of the platform every frontend sits on. Nothing claims `logos.*` yet; it is reserved now because reserving it later, once apps have declared `uses` against it, is not possible.

The two reservations live in different places, and that is the same split the rest of this document turns on. `logos.*` is enforced by the frozen surface (`LogosIntent.h`), because the platform's namespace outlives any particular shell. `basecamp.*` is enforced in `IntentRegistry`, because *which shell owns which prefix* is policy, and policy belongs in the disposable half — another frontend reserves its own prefix without touching the frozen header.

Matched byte-exactly — no case folding, no Unicode normalisation — because a name is a contract between independently shipped apps, and "looks the same" is not good enough.

---

## 4. Providing a capability

```json
"provides": [
  {
    "intent": "wallet.send",
    "params": [
      { "name": "to",     "type": "string", "required": true },
      { "name": "amount", "type": "number", "required": true },
      { "name": "memo",   "type": "string", "required": false }
    ]
  }
]
```

```qml
Connections {
    target: logos
    function onIntentRequested(requestId, intent, params, requesterName) {
        if (intent !== "wallet.send") return;
        logos.respond(requestId, true, { txHash: "0x…" }, "");
    }
}
```

`respond` takes **all four arguments**, with no defaults: a provider that omits `error` on a failure path must not fall into reporting success. `requesterName` is attested by the host, so unlike a name inside the payload it can be trusted.

- **`provides` travels into the signed `.lgx` manifest** (0.5.0+) and from there into a repository's `index.json`. That is what lets the shell offer to *install* a provider for something nothing on disk can do. `uses` is deliberately not carried — a catalog needs to know what a package can *do*, not what it wants to *call*.
- **`params` is enforced.** The broker checks the payload against the chosen provider's declaration just before dispatch and refuses with `bad_request`; the handler never sees it. Undeclared extra fields pass, so a caller written against a newer provider is not broken by an older description. No `params` at all means *undescribed*, not *takes nothing*.
- **Declaring without handling gives `timeout`, not silence** — the shell counts receivers and logs a warning naming your module.

The check runs only once a provider is chosen, never at submit: two providers may describe one intent differently, and testing all their specs would reveal how many exist. It describes what *that app* wants, not what the name means — there is no schema for `wallet.send` itself to appeal to yet (§7).

### What the shell provides

Registered in code (`registerShellProvider`), because the shell has no `metadata.json`. Two groups, and the line between them is load-bearing:

| Intent | Kind | Hand-off |
|---|---|---|
| `basecamp.repositories.manage` | navigation | yes |
| `basecamp.settings.open` | navigation | yes |
| `basecamp.apps.open` | navigation | yes |
| `basecamp.apps.launch` | navigation, takes `{ "app": … }` | yes |
| `basecamp.packages.confirm_install` | dialog | no |
| `basecamp.packages.confirm_uninstall` | dialog, restricted | no |
| `basecamp.packages.confirm_upgrade` | dialog, restricted | no |

The navigation group is all hand-offs: `ok` means "you are there", not "we are done", so returning the user would undo the request.

**The navigation list must stay navigation-only, and that is a security property, not a taste one.** The broker skips the chooser when the shell is the only provider (§2) — so everything in that group is something a requester can make the shell do *with no consent dialog at all*. That is correct for moving between the shell's own sections: nothing crosses a boundary, nothing is destructive, and confirming a navigation the user just asked for is a dialog answering itself. It stops being correct the instant an entry mutates state. Adding `basecamp.packages.uninstall` to that list would be a silent, unconsented removal, reachable by anything that can raise an intent.

**`basecamp.apps.launch` carries the app name as a parameter, and answers the same way whether or not that app exists.** Both halves are load-bearing.

The name is a parameter because `basecamp.launch.<appName>` — the obvious alternative — fails three ways. App names are not bound by the intent-name grammar, so anything with a hyphen or a capital produces a name that is silently dropped. `provides` would have to be re-registered on every install and uninstall. And a caller's `uses` would have to name each app it might launch, which is exactly the "a request names a capability, never a provider" line in §1. `packages.show` already carries a package name this way.

The answer is constant because "launch X, tell me if it worked" is otherwise an installed-app enumeration oracle: iterate plausible names, read the answers, recover the user's whole app list. `unavailable` merging "absent" with "denied", the timing floor, and install offers that never report back all exist to prevent precisely that, and a differential answer here would undo all three at once. So the requester always gets `ok`, on the floor, and learns nothing; when the app is absent the shell may offer an install, which is between the shell and the user. A malformed payload takes the same path for the same reason — `bad_request` would confirm the name was well-formed but absent, which is half the oracle back.

Note the asymmetry that makes this workable: from a link there is no oracle at all, because nothing returns to the browser and the user clicked it themselves. The constant answer is what makes the app-to-app case safe as well.

**These were `logos.*` before the namespace split, and no alias is carried.** `package_manager_ui` is the only consumer and was never released declaring the old names, so no installed copy asks for them. That makes a compatibility window pure cost: restrictions are keyed on the name as submitted and checked before delivery, so a surviving `logos.packages.confirm_uninstall` would be a second live path to a restricted, destructive capability — and one nothing on disk would ever have used.

---

## 5. Calling one

```json
"uses": [ { "intent": "wallet.send" } ]
```

An undeclared request fails `not_declared` before anything is resolved — the broker's first gate, and the only thing `uses` does today.

> ⚠ **A bare string array is silently ignored.** `"uses": ["wallet.send"]` parses, declares nothing, and every request fails `not_declared` with no obvious cause. Entries must be objects; the registry logs a diagnostic for each rejected entry.

`res` always has all three keys (`ok`, `data`, `error`), and the callback always fires exactly once. `res.error` is one of six values:

| Code | Meaning | Who can send it |
|---|---|---|
| `not_declared` | you did not list this intent in your own `uses` | shell |
| `unavailable` | no provider could service it, **or you were not allowed** | shell |
| `bad_request` | your `params` were rejected | shell **and** provider |
| `cancelled` | the user backed out, or the provider cancelled | both |
| `timeout` | a provider was reached but never answered | both |
| `failed` | the provider reported a failure | both |

Anything else a provider returns is coerced to `failed` — free text in the caller's error path is both a leak and an un-switchable API.

**`unavailable` merges "nothing installed" with "denied" on purpose.** An app that could tell those apart would have an oracle for your installed-app list. Same reason the install offer never reports back: decline it and the caller gets the identical `unavailable` it would have got had no such package existed.

**Some intents restrict who may ask.** A provider-side allow-list, declared in code beside the shell's own `provides` (`IntentRegistry::restrictIntentToRequesters`). Absent = unrestricted, which is every intent except two.

It exists because attribution is not always enough. Showing who asked works when the user has context to judge against — they clicked something, and "Chat App wants to send funds" is a question they can answer. An *unsolicited* prompt to remove or downgrade one of your packages has no such context, and its correct answer is always no. A dialog whose right answer is unconditional can only cost you: it trains dismissal, and one mis-click is destructive and not undoable. So `basecamp.packages.confirm_uninstall` and `basecamp.packages.confirm_upgrade` are restricted to `package_manager_ui`, while `confirm_install` stays open — an app saying "you need X" is legitimate, and the shell already offers catalog installs an app's request provoked.

Three properties are load-bearing. Denial answers `unavailable` **on the same floor** as "nothing provides it", so a refused app cannot learn the capability exists. An empty requester list is **refused**, not stored — it reads as "restricted to nobody" but would behave as unrestricted, so a typo must not silently open a destructive capability. And the list survives `rebuild()`, because it is code-declared policy rather than something read off disk.

Its limit is the same one §6 sets out: an allow-list keyed on a self-declared module name is only as strong as the name, and nothing is signed. It raises the bar from "any installed app" to "an app that can successfully claim the name `package_manager_ui`". That is meaningfully better and it is not a proof.

**`bad_request` vs `failed`** is "you sent the wrong thing" vs "the world didn't cooperate" — only the first is worth fixing on your side. Both the shell and the provider can send it, and you cannot tell which did: if only the shell could, the code itself would prove no provider was consulted. The reason goes to the log, not to the caller — the envelope carries a code and nothing else.

---

## 6. Known limitations

**Nothing is signed.** The official catalog has zero signatures across all published versions and an empty `trustedSigners` list. Every guarantee above is about *routing* — that a result reaches the right caller, that a provider cannot forge or enumerate requests. None of it is about *identity*. A module name is a string a package chose for itself: two packages can claim the same `provides` and the same display name, and the chooser cannot tell you which is the wallet you installed last week. What the chooser does about that is partial and worth knowing exactly. Every row shows the package name under the display name, and so does the line naming the requester — a package called `evil_ui` shipping `"display_name": "Wallet"` cannot hide behind the label. "Details" expands in place (it does not navigate away, and does not end the request) to show version, originating repository, install type, and an explicit **"Unsigned — the shell cannot confirm who published this"**. Install offers likewise show the repository hostname. All of that is provenance for the *channel*, never the *publisher*, and none of it is proof.

The content hash is deliberately **not** shown. It is available, but a hash is only evidence against an independent trusted reference, and with nothing signed the catalog that serves the package serves the hash too. Beside the word "Unsigned" a 64-character digest reads as rigour and weakens the honest statement next to it.

**The timing floor is a speed bump, not a proof.** 400 ms defeats the naive probe. It does not survive statistical analysis, and it does not cover timing once the user is involved.

**A provider decides what you see.** Ask a wallet to sign and the wallet renders the confirmation. The shell cannot verify that what it displays matches what the caller actually sent.

**The catalog → registry hop is not covered end-to-end in CI.** Building a `.lgx` with `provides`, publishing it, and having the shell offer it has been verified by hand against a local repository; no automated test crosses all three.

**Prompt fatigue is a real cost.** Confirming every cross-app dispatch, including the single-provider case, is right for a first release — the silent case was the dangerous one — but the answer at scale is scoped, revocable grants, not more dialogs.

---

## 7. What should come next

**Signatures, and everything they unlock — the priority.** Signed manifests turn every "the user must judge" above into something the shell can check: a stable publisher identity, so the chooser can say "by the same publisher as the app you installed" instead of showing a self-declared string; namespace ownership, so `wallet.*` is restricted to signers entitled to it, which is also what would make `uses` meaningful rather than decorative; trust on first use and then pinning, so an upgrade signed by a different key is a prompt rather than a silent swap. The groundwork exists — the manifest is versioned, `provides` sits inside the signed region, `trustedSigners` has a place in the repository descriptor. Missing are key management, a signing step in the release pipeline, and verification at install.

*Where that verification belongs — recorded because the current arrangement looks like an oversight and is not.* The registry reads each installed app's **`metadata.json`**, which is unsigned, while a signed copy of `provides` sits unread beside it in `manifest.json`. Deliberate, for three reasons that hold independently: nothing is signed today, so reading the manifest would change no threat; not every installed app has a manifest (dev builds, `DEV_QML_PATH`, hand-placed plugins), so the registry needs the `metadata.json` path regardless — and a control you fall back from is not a control; and the manifest is a bundle-time snapshot that can drift, so trusting it means the shell can believe something the module no longer says.

When signing lands, the fix is **not** to move the runtime read. Verify at install: check the manifest signature, confirm `metadata.json`'s `provides` matches it, refuse the install on mismatch. That puts the check at the trust boundary that already exists — where the user consents — rather than re-verifying a signature on every registry rebuild, which happens on every install, uninstall and load. It is a change to `IntentRegistry::rebuild()` alone: no broker change, no frozen-surface change, nothing for apps.

The gap that leaves is post-install tampering: editing `metadata.json` after the fact still works. Judged not worth closing while neither file is integrity-checked at load, since anyone who can write to the plugin directory can replace the `.so` outright and arbitrary code is the larger prize. Revisit if that asymmetry ever appears.

Note what the manifest does **not** carry: the author's `params`. Only intent names travel into the manifest and the catalog, because the only question that copy exists to answer is "which installable package provides X?". The payload shape is read from — and enforced against — the installed `metadata.json`; a second copy in the manifest would be a bundle-time snapshot nothing reads and that can drift from the file actually enforced. If signing later makes an attested parameter shape worth having, adding it back is a field on an existing object: no shape change, no version bump.

**Well-known intents defined somewhere public.** `wallet.sign` currently means whatever two developers independently decided. A caller learns the shape by reading a provider's `metadata.json`, which describes *that provider*, not *the intent* — two wallets can describe the same name differently and both be "correct". What is needed is a published, versioned registry: name, parameter schema, result schema, semantics, compatibility policy. It should be where names are *published*, not where they are *permitted* — anyone should be able to define `myapp.thing` and have others implement it, with reserved namespaces the exception.

**Shell-defined intents with third-party providers.** `basecamp.repositories.manage` is serviced by Basecamp today. The interesting inverse is intents Basecamp *defines* and any app may *implement* — `basecamp.share`, `basecamp.open` — so a user can replace a built-in without the shell knowing. The plumbing supports it; missing are the definitions and the rule for when the shell's own implementation should lose to an installed one.

**Opening an app from a link outside Basecamp — shipped.** A `basecamp://` URL becomes an intent submitted on behalf of a requester the shell mints itself, and resolution, consent, dispatch and the spoofing guard all work unchanged. Three forms: `basecamp://` raises the window and submits nothing; `basecamp://app/<name>` is sugar for `basecamp.apps.launch`; `basecamp://intent/<name>?p=<base64url JSON>` is the general case. base64url-of-JSON rather than query pairs because query values are strings, and the broker type-checks a payload against the provider's declared `params` — `?amount=12.5` would arrive as the string and be refused with no way to say otherwise.

*Registration and delivery.* `CFBundleURLTypes` on macOS, where LaunchServices also delivers to the running instance as a `QFileOpenEvent` and nothing else is needed. On Linux and Windows the OS launches a *new process* per click, so there is a single-instance guard: a `QLocalServer` named from a hash of the **resolved** user directory, because `--user-dir` exists so instances can run side by side and a global lock would break exactly that. A secondary forwards its URL and exits before creating a runtime. The primary reads that socket asynchronously — `waitForReadyRead()` on the GUI thread freezes the running app for its whole timeout whenever something connects and sends nothing. Linux registration is written to `~/.local/share/applications` on first run using `$APPIMAGE`, not `applicationFilePath()`, which points into a mount that disappears at exit; Windows uses `HKCU\Software\Classes\basecamp`, which needs no installer and no elevation.

*The cold-start gate is the first registry rebuild*, not the window appearing. A URL that launched the app arrives seconds before `IntentRegistry` knows what any app provides; resolving in that window answers `unavailable` and may offer to install a package that is already there. So it is parked, with a deadline — a URL waiting on a rebuild that never comes reports failure rather than evaporating.

*A link is not the shell.* It submits under its own requester name, never `main_ui`. The broker skips the chooser when the shell is either party (§2), so a link that inherited that identity would dispatch into an installed app with no consent step at all — and nothing else would look wrong. There is a test whose entire purpose is that a link faces the chooser.

*Reachability is opt-in, declared by the provider.* `uses` cannot gate a link, there being no manifest on the calling side, so an app publishes a capability to the web with `"web": true` on its `provides` entry; the link requester's `uses` is derived from those plus the shell's own web-reachable set on every rebuild. Without it every entry in every `provides` would silently have become a web entry point. A non-boolean is refused rather than coerced, for a sharper reason than `handoff`: `"web": "false"` read as true publishes something its author was declining to publish. The shell's own set is navigation only — the package confirmation intents are deliberately absent.

*What is still true about the requester.* A web page has no identity the shell can check, and that is a different problem from an unsigned app rather than a worse version of one. The browser does not tell us the originating page, and any origin carried *in* the URL is written by whoever wrote it, so the chooser can say a link was clicked and nothing more. It must not render an attacker-supplied string as if it were an identity. Web-originated dispatches must never be rememberable if "always use this app" lands, since the *(requester, intent)* pair a default keys on has no meaningful requester here. And `params` arrive fully attacker-controlled, which is what turns `specViolation` from a developer convenience into a security check.

*What the web side still cannot do.* No browser will tell a page whether Basecamp is installed — a fingerprinting vector, blocked deliberately. The navigate-then-fall-back-after-a-timeout heuristic misfires whenever the app is merely slow to start, which is exactly the cold path. Worth saying plainly: "open it if they have it, otherwise offer the download" sounds like one feature and is two, of which the second is guesswork. The honest arrangement is one link — `https://…/i/<intent>` and `basecamp://intent/<intent>` naming the same thing — so "not installed" lands on a real page.

*Nothing goes back to the browser*, by construction. Every failure surfaces in the shell and the log, which is also why no error code here can leak anything: there is nobody on the far side to read it.

**Consent that scales, starting with "always use this app".** This shipped once and was pulled before release: remembering a pick is only half a feature without a way to see and undo it, and a preference you cannot revoke from the UI is worse than one you never made. Landing it properly needs a settings screen listing every remembered choice with a revoke button; intents that can never be remembered, since "always allow" must not apply to signing a transaction — a property of the intent, so it belongs in the registry above; and a rule that a remembered pick is only ever a shortcut past the chooser, never a different call shape, so the whole thing stays removable. Beyond that: a read/mutate distinction, which the vocabulary currently cannot express, and scoped grants — "this app, this intent, up to this amount, for 24 hours" — the shape that reduces prompts without reducing safety.

**Getting back to whoever asked — partly answered.** A provider answering now returns the user to the requester, on either outcome; see §8. What is still missing is the *install* case: after installing a suggested package you must find the original app and repeat the action yourself. Nothing dispatched, so there is nothing to return from, and reviving a request the user was already told was `unavailable` would mean an app's action firing without them asking twice.

**`cardinality: "all"` needs rethinking before it is built.** It is reserved in the grammar, but broadcast conflicts with the rest of the design in two ways: it tells providers the user never chose that a request happened, leaking the requester's activity to bystanders; and returning N results tells the caller how many providers exist, which is precisely the enumeration oracle that merging `unavailable` and flooring the timing exist to prevent. It needs a different consent shape — the user picking a *set* — and probably must not report a count. It is not simply pending implementation.

**Robustness.** Progress reporting, since 20s is either far too long or far too short depending on the intent — though this means adding a signal to the frozen surface, whose whole value is that it does not change. A dispatch audit log the user can inspect, which needs durable storage and is privacy-sensitive in its own right. Per-requester rate limiting, and closing the CI gap above; those two are simply not done.

**Developer experience.** `params` is enforced but surfaced nowhere a developer looks: there is no `lm intents` command, and the App Manager does not show what an app provides or expects. Finding out how to call an intent means reading someone's `metadata.json`.

---

## 8. Reference

| | |
|---|---|
| Frozen vocabulary | `logos-view-module-runtime/include/LogosIntent.h` |
| QML surface | `logos-view-module-runtime/src/LogosQmlBridge.cpp` |
| Registry / broker | `logos-basecamp/app/IntentRegistry.{h,cpp}`, `IntentBroker.{h,cpp}` |
| Shell's own capabilities | `logos-basecamp/app/ShellIntents.{h,cpp}` |
| Deep links | `logos-basecamp/app/links/` — `LinkUrl` (parser), `SingleInstanceGuard`, `LinkUrlInbox`, `LinkRequestCoordinator`, `SchemeRegistrar` |
| Dialogs | `logos-basecamp/src/Basecamp/Shell/Intent*Dialog.qml` |
| App-author guide | `logos-tutorial/guide-intents-for-app-developers.md` |
| Full reference | `logos-tutorial/logos-developer-guide.md` §8.5 |

---

## 8. Returning to the caller

Dispatching moves the user to the provider. Not moving them back was an asymmetry, not a policy: the shell was willing to take someone somewhere and unwilling to bring them home.

When a provider **answers** — `ok` either way; a cancel is the outcome that most wants a ride home — the shell returns the user to the requester. Navigation only: the result reaches the requester's callback exactly as before, and nothing about the frozen surface in `LogosIntent.h` changes.

**Only that path navigates.** `finish()` is reached six ways and only `submitResponse` is something the user did. The `sweep` deadlines, the 400 ms `failWithFloor`, `endpointDestroyed`, `abandon` and refused payloads all happen for reasons that never reach the screen — four of them never navigated in the first place, and the two that did would otherwise move the shell minutes later with nothing on it to explain why. The path is recorded as `CompletionPath` and carried into `finish()` for exactly this reason.

Five further guards, each keeping the motion explainable rather than merely safe:

| Guard | Prevents |
|---|---|
| `didNavigate` | "returning" from a request that displaced nobody — a shell provider, or anything that failed before dispatch |
| `isHandoff` | bouncing the user out of a destination they were sent to |
| `isAppFrontmost(provider)` | dragging back a user who already moved on |
| `isAppLoaded(requester)` | navigating to an app that unloaded meanwhile; a return must never become a load |
| `anyDialogOpen()` | navigating out from under a shell dialog |

A **400 ms dwell floor** holds the return so a fast provider produces visible motion rather than a flicker; it is a legibility bound, not a semantic one — `handoff` is the semantic test. A **circuit breaker** (3 returns in 10 s) disables auto-return for the session: nothing detects intent cycles and a provider may submit while handling one, so the user must never be trapped in a shell moving on its own.

**Hand-offs.** Not every intent is a transaction. Some exist to take the user somewhere and leave them: a page, a note, something they will watch on its own timescale. Their `ok` means "I have taken you there", not "we are done", and returning would undo the request. The provider declares it on the `provides` entry beside `params`:

```json
{ "intent": "some.intent", "handoff": true }
```

Absent means `false`. Per *(provider, intent)*, like `params` and for the same reason: two apps may implement one capability differently and there is no per-intent schema to appeal to. A non-boolean is refused into `diagnostics()` rather than coerced — `"handoff": "true"` would otherwise read as true and silently invert an author's intent. Two providers may disagree; the chosen one's declaration is the one that applies.

**`handoff` governs navigation only.** When a provider answers is a separate, independent decision: on arrival when there is no completion to wait for, or when the user marks the action done — at which point the caller learns it really happened and the user still is not sent back. The one constraint is the deadline a provider that accepted already lives under: **10 minutes** before the backstop reports `timeout`. Ample for a button press, not for work waiting on a network or a chain, where the provider should answer once the work is *started* and let the caller read the outcome from its own data source.

`basecamp.repositories.manage` is declared a hand-off in code, through `registerShellProvider`. It happens not to auto-return anyway — the broker never presents a shell provider, so `didNavigate` is false — but that is the right behaviour by an unrelated route, and it would stop holding the day a shell intent is transactional.

**Not carried into the manifest.** `handoff` stays in the installed `metadata.json`, exactly like `params`, and `bundle.sh` copies intent names alone. A second copy in the signed manifest would be a bundle-time snapshot nothing reads and that can drift from the file actually enforced.

**Why no return affordance.** An earlier draft had a floating "← Back to Chat" chip for every case auto-return declines. Dropped: every loaded app has a permanent sidebar icon, and plugin widgets are confined to the content stack, so a provider's own popup cannot cover the way back. The chip would have been a second escape hatch layered on one that cannot be blocked — at the cost of a component, a grace timer, bridge plumbing, and a placement constraint (`OverlayDialogs` must stay hidden when idle or it steals the Linux drag-and-drop pixmap). **That makes "app content cannot cover the sidebar" load-bearing**, and it is the kind of thing a later full-bleed workspace would quietly break.

Known limits: wander off before the provider answers and there is no automatic return; the install-offer flow has none, as above; and a return lands you on the app, not on where you were inside it — in-app position survives only because nothing here unloads anything.
