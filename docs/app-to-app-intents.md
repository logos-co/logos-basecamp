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
| The shell is the provider | `logos.repositories.manage` and the three `logos.packages.confirm_*` are serviced by Basecamp itself |
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

**Names** are `namespace.verb`: 2–4 dot-separated segments, 3–64 chars, lowercase with single underscores. `logos.*` is reserved for the shell. Matched byte-exactly — no case folding, no Unicode normalisation — because a name is a contract between independently shipped apps, and "looks the same" is not good enough.

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

It exists because attribution is not always enough. Showing who asked works when the user has context to judge against — they clicked something, and "Chat App wants to send funds" is a question they can answer. An *unsolicited* prompt to remove or downgrade one of your packages has no such context, and its correct answer is always no. A dialog whose right answer is unconditional can only cost you: it trains dismissal, and one mis-click is destructive and not undoable. So `logos.packages.confirm_uninstall` and `logos.packages.confirm_upgrade` are restricted to `package_manager_ui`, while `confirm_install` stays open — an app saying "you need X" is legitimate, and the shell already offers catalog installs an app's request provoked.

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

**Shell-defined intents with third-party providers.** `logos.repositories.manage` is serviced by Basecamp today. The interesting inverse is intents Basecamp *defines* and any app may *implement* — `logos.share`, `logos.open` — so a user can replace a built-in without the shell knowing. The plumbing supports it; missing are the definitions and the rule for when the shell's own implementation should lose to an installed one.

**Opening an app from a link outside Basecamp.** Today the only thing that can raise an intent is an installed `ui_qml` app. The obvious extension is a `logos://` URL: a user clicks a link in a browser and Basecamp comes up with the request already in flight. Nothing in the design resists it — the URL becomes an intent submitted on behalf of a requester the shell mints itself, and resolution, consent, dispatch and the spoofing guard all work unchanged. What is missing is everything around that one line.

*Registration and delivery, which is plumbing.* Nothing registers a scheme today: `app/macos/Info.plist.in` has no `CFBundleURLTypes`, and `assets/logos-basecamp.desktop` has no `x-scheme-handler/logos` and takes no `%u`.

| Platform | What it needs | Note |
|---|---|---|
| macOS | `CFBundleURLTypes` in the plist | LaunchServices registers on first launch and delivers the URL to the *running* instance as a `QFileOpenEvent`, so there is nothing else to build |
| Linux | `MimeType=x-scheme-handler/logos;`, `Exec=… %u` | Only takes effect once the desktop file is in the user's database. The shipped artifact is an AppImage, whose embedded desktop file the system never sees unless the user runs AppImageLauncher — so this needs first-run self-registration into `~/.local/share/applications` |
| Windows | `HKCU\Software\Classes\logos` | No installer exists, so the same first-run self-registration |

On Linux and Windows the OS launches a *new process* per click, and Basecamp has no single-instance guard — a second click would start a second app rather than hand the URL to the first. That guard must be keyed on the resolved user directory rather than being global: `--user-dir` exists precisely so instances can run side by side, and a naive lock would break it.

*The part that is not plumbing.* A web page is a requester with no identity the shell can check, and that is a different problem from an unsigned app rather than a worse version of one. §6's answer to "who is asking" is to show the package name beneath the display name — self-declared, but stable, and attached to something the user chose to install. A link has no equivalent. The browser does not tell us the originating page, and any origin carried *in the URL* is written by whoever wrote the URL, so the chooser cannot honestly name the requester at all: it can say that a link was clicked, and nothing more. That is thinner consent than the app case, and it must not be papered over by displaying an attacker-supplied string.

Three consequences worth settling before any of this is built. `uses` cannot gate it, there being no manifest on the calling side — so reachability has to be declared by the *provider*, an explicit per-intent opt-in, rather than every entry in `provides` silently becoming a web entry point. Web-originated dispatches must never be rememberable, for the same reason "always allow" must not apply to signing: the *(requester, intent)* pair a default is keyed on has no meaningful requester here. And `params` arrive fully attacker-controlled, which turns `specViolation` from a developer-convenience check into a security one.

*What the web side cannot do.* No browser will tell a page whether Basecamp is installed — it is a fingerprinting vector, and it is blocked deliberately. The usual fallback, navigate and then show a download link if nothing happens within a second or two, is a heuristic that misfires whenever the app is merely slow to start. Worth saying plainly to anyone who asks for this: "open it if they have it, otherwise offer the download" sounds like one feature, and is two, of which the second is guesswork.

**Consent that scales, starting with "always use this app".** This shipped once and was pulled before release: remembering a pick is only half a feature without a way to see and undo it, and a preference you cannot revoke from the UI is worse than one you never made. Landing it properly needs a settings screen listing every remembered choice with a revoke button; intents that can never be remembered, since "always allow" must not apply to signing a transaction — a property of the intent, so it belongs in the registry above; and a rule that a remembered pick is only ever a shortcut past the chooser, never a different call shape, so the whole thing stays removable. Beyond that: a read/mutate distinction, which the vocabulary currently cannot express, and scoped grants — "this app, this intent, up to this amount, for 24 hours" — the shape that reduces prompts without reducing safety.

**Getting back to whoever asked.** Dispatching brings the provider forward; nothing brings the user back — and after installing a suggested package you must return to the original app and repeat the action yourself. `IntentPresenter` deliberately has no "put it back", because a shell that navigates on its own fights whoever is driving — but the current state is the opposite extreme, where you approve something in a wallet and are left there with no thread home. What is missing is *offered* return: an affordance naming the app that asked, present while a request is in flight and briefly after, surviving navigation within the provider. Shell navigation only — no error code, no manifest field, no change to the frozen surface.

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
| Dialogs | `logos-basecamp/src/Basecamp/Shell/Intent*Dialog.qml` |
| App-author guide | `logos-tutorial/guide-intents-for-app-developers.md` |
| Full reference | `logos-tutorial/logos-developer-guide.md` §8.5 |
