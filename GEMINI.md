# GEMINI.md / CLAUDE.md

This file defines the workflow, conventions, and rules for LLM coding assistants (Gemini and Claude) working in this repository. It is intentionally limited to *how to work here* — the project overview, architecture, and command references live in the documents linked under [Where Things Live](#where-things-live).

## Where Things Live

GEMINI.md / CLAUDE.md holds the workflow, conventions, and rules. It deliberately carries no architecture, addresses, or command listings; those live in:

- **[README.md](README.md)** — the plain-language project overview and the build/flash command reference.
- **[DESIGN.md](DESIGN.md)** — the engineering reference: memory map (§1–§2), OFW patching (§3), recovery hook (§4), boot flow & module map (§5), bank swap (§6), asset/sprite details (§7), OSPI init (§8), boot magic values (§9), and debugging & inspection scripts/commands (§10).
- **[ACTIVE_WORK.md](ACTIVE_WORK.md)** — the live overarching goals, granular tasks, and debugging log.
- **[CHANGELOG.md](CHANGELOG.md)** — a terse bullet-point log of completed fixes and tasks.

Whenever a rule below touches something technical, it points to those docs rather than restating it.

## Active Work Tracking

[ACTIVE_WORK.md](ACTIVE_WORK.md) is the live working document: two tiers of checklist (**Overarching Goals** → **Tasks** grouped under them) plus a **Debugging** log and long-lived reference notes. The goals mirror the **Project Goals** in DESIGN.md.

**Workflow for AI assistants:**
1. **Start of every session:** Read `ACTIVE_WORK.md` first to orient yourself before touching any code or running any commands.
2. **During work:** Update the Tasks checklist as items are completed. **NEVER mark a task as done (`[x]`) until you have verified the change with testing on physical hardware. Testing on device is as important as successfully building the binary, if not more; marking things as accomplished before verification on the hardware is a critical antipattern we must avoid.** Add new Debugging entries as issues are discovered, and log finished fixes/tasks in CHANGELOG.md (see Documentation Workflow).
3. **End of session:** Summarize any unresolved state in Debugging so the next session can pick up without re-investigation.
4. **When sections are empty:** Leave the headings in place — do not remove them. Use a single `-` or `*(none)*` as a placeholder.

## Documentation Workflow

These documents divide the labor; keep them aligned whenever a goal, feature, or task changes:

- **[README.md](README.md)** — the friendly, non-technical overview: plain-language descriptions of the key features for a general audience. No registers, addresses, jargon, or marketing tone — just what it does.
- **[DESIGN.md](DESIGN.md)** — the engineering reference. Opens with the **Project Goals** (the overarching backbone), then a section per subsystem, each pairing an **Overview** (the idea) with an **Implementation** (how the code works).
- **[ACTIVE_WORK.md](ACTIVE_WORK.md)** — the live working document: overarching goals, granular tasks grouped under them, and the debugging log.
- **[CHANGELOG.md](CHANGELOG.md)** — a terse, bullet-point record of completed bug fixes and tasks (newest first).

The overarching-goals list is shared by README, DESIGN, and ACTIVE_WORK — README in plain language, DESIGN with architecture, ACTIVE_WORK with live tasks. When one changes, update the others in the same pass.

**Log completed work in the CHANGELOG.** When a bug fix or task is finished and verified, add one concise bullet to CHANGELOG.md (newest first) and link the commit(s) that landed it. Keep each entry to a single line — the CHANGELOG is a scannable history, not a narrative.

**Single source of truth (DRY).** Each fact lives in exactly one place. Architecture and mechanism detail belongs in DESIGN.md; GEMINI.md and README.md point to it rather than restating it. When something moves or changes, fix the one authoritative copy and confirm the pointers still resolve.

**Both the AI and the human are responsible for upholding this.** The AI must keep the docs aligned as it works and flag drift; the human should do the same rather than letting them rot.

**Work in feature branches, merge clean.** Before any large or risky change — anything touching many files, the boot path, the patch pipeline, or several subsystems at once — create a dedicated git feature branch off `main`. This keeps `main` stable and makes a change that goes sideways painless to abandon. Commits *within* the branch can be granular and messy — that's fine. But before it reaches `main`, **rebase onto `main`, squash the branch's commits into a coherent minimal set, then merge.** Branch clutter should not pollute `main`'s history.

## Testing Workflow

Tests are kept aligned with the code exactly like the docs are: **every feature or change updates OR adds the appropriate test in the same pass; never leave tests to rot.** A menu/string/i18n change updates (or adds) the device test that covers it; a new subsystem ships with its test. A stale test is treated as seriously as a stale doc.

- **Verify UI on hardware through the OCR harness, not by eye.** `scripts/common/ocrnav.py` navigates the menu by on-screen text and OCR-asserts rendered strings in any language; prefer it over one-off `snap.py` plus eyeballing. Detect the active language by OCR (render a candidate's label, match it), never by SWD symbol reads, since the i18n state lives in the PIE language module, not the core.
- **Prefer modular, parameterized tests over bespoke one-offs.** A reusable verifier such as `verify_entry_translated_in_all_languages.py <STR_ID>` (cycle the live Language selector, assert a UI element renders translated in every language) beats a new hand-written test per check.
- **Both the AI and the human uphold this**, same as the docs: keep tests current as you work and flag drift.

## Communication & Control Rules

* **CRITICAL STOP & TALK SIGNAL (BROAD INTERPRETATION):** If the user sends any permutation of "We need to talk", "Stop", "Wait", "Hold on", "Hang on", "Pause", or any command to halt, you must **immediately halt all tool execution** (no compilation, files, shell, or search tools). You must not run any background commands or perform any actions, and must instead stop and interact directly with the user via text to align and get on the same page.
* **MANDATORY INTERACTIVE RESPONSE TO INQUIRIES:** If the user asks any question, raises a concern, or makes an inquiry, you **must respond to it in an interactive way and answer it directly in text before doing anything else**. Do not blow past user questions, do not execute any further code or tool operations, and do not continue working on tasks until the inquiry is fully addressed and you have confirmed with the user. The human user's questions and comments always take absolute priority.
* **Context Compaction Handling:** If a context compaction/truncation occurs, do not silently crawl the filesystem or log files to find your history. Explain to the user in text that compaction has happened, and state what you need to look up or ask before calling any tools.
* **Direct Dialog Preference:** Avoid silent background execution loops. Keep the user in the loop on your rationale at each step.
* **Explanatory Debugging:** For every hardware interaction (flashing, resetting, or running debug scripts), provide a brief explanation of *why* the step was performed and *what* the specific result was (e.g., 'Running memory.py to verify magic word; result was 0x00000000').
* **Debug Script Evolution:** If a debug script fails because you assumed it supported certain parameters or logic (e.g., math in address arguments), consider implementing that missing functionality if it would benefit future debugging sessions.
* **Patch Stage Insights:** Always run the firmware patching process with high verbosity to capture critical details about compressed asset relocation and memory offsets.


## Engineering Rules

* **STABILITY IS LAW — Boot Path is Inviolable:** The chainloader must always reach the launcher menu, regardless of the state of Bank 2, external flash, or any secondary firmware slot. This is the single non-negotiable invariant of this project. Stability is never traded against features. Features are always traded against stability.
  - **Never** add code to the boot-critical path (`stub_main.c`, `startup.s`) without explicitly tracing every possible failure mode to its conclusion. If a branch can exit the boot path without reaching `BOOT_ACTION_DECOMPRESS` or a valid jump target, that is a critical bug.
  - **Never** perform a destructive operation (erase, overwrite) on internal flash from within the chainloader without first excluding Bank 1 from the target range and confirming the bank-swap state (`bank.py status`). If banks are swapped, Bank 2 is mapped to `0x08000000` — erasing it destroys the boot firmware.
  - **Any new feature, menu option, or diagnostic tool that can modify flash must display an explicit confirmation prompt with a clear description of what region will be erased before proceeding.** Silent or single-button destructive operations are not acceptable.
  - **Iterate on failure modes.** Before shipping any code that touches the boot path or flash, walk through: "What if Bank 2 is empty? What if external flash is absent? What if a reset fires mid-operation?" The menu must remain reachable in every case.
  - **RETRO-GO RETURN-TO-MENU IS INVIOLABLE.** When Retro-Go signals "Return to Main Menu" — the `BOOT_MAGIC_RETROGO` (`"CORE"`) marker, or the `BOOT_MAGIC_RESET` warm-reset trace it leaves in the **RG magic cell** (`RG_MAGIC_ADDR` = `0x20000000`, *not* the `0x2001FFF8` SRAM cell); see `src/common/boot_magic.h`, `src/common/memory_map.h`, and DESIGN §5/§9 — the chainloader MUST **re-launch Retro-Go so its OWN launcher (game list) reloads**, by jumping to `RETROGO_BASE`. It must **never** fall through to the chainloader's own menu, boot an OFW, or hang. `RESET` is consumed by the **stub** (`stub_main.c`); `CORE` by `main.c` `app_early_logic()` §2.1 — both jump to `RETROGO_BASE`, and always via the `RETROGO_BASE` / `RG_MAGIC_ADDR` defines, **never a literal address** (a hardcoded `0x08008000` here went stale when the flash ceiling rose 32K→40K and silently dropped to the menu — that was the recurring regression). Two *different* signals legitimately land on the chainloader menu and must keep working: Retro-Go's "Quit to Bootloader" (`BOOT` at `0x2001FFF8` + target, §2.3) jumps to the chainloader, and holding **START/PAUSE** at boot (§1.3, which runs before §2.1) forces the menu — that button override is the only escape from a stuck Retro-Go reset loop, so `CORE`/`RESET` themselves must re-launch unconditionally. **This exact round trip has regressed at least five separate times after working; it is not allowed to break again.** Treat it like the boot path itself: NEVER change the stub/boot magic handling (`stub_main.c`, `startup.s`, `boot_magic.h`, the magic-cell checks, their addresses, or the *order* of the magic checks) without (a) tracing every magic value's path to its conclusion and confirming the Retro-Go case **re-launches Retro-Go**, and (b) **re-verifying the in-game "Return to Main Menu" → Retro-Go launcher round trip on physical hardware before claiming the change is done** (`scripts/tests/retrogo_return_test.py` injects the magics and asserts Retro-Go re-launches — a fast proxy, but the real in-game flow is the authoritative check). A green build is never sufficient.

* **Documentation Discipline:** NEVER edit documentation files (`.md`, `.txt`, etc.) using shell commands like `cat`, `sed`, `echo >>`, or `redirect`. Always treat documentation changes exactly like code changes: use the `replace` or `write_file` tools to ensure accuracy and allow for proper review. Do NOT delete information that does not pertain to what you are currently doing. Deleting or removing documentation is not to be taken lightly and should only be proposed whenever the new information conflicts with or differs from information already present.

* **Plain, Literal Documentation Language:** Do not carry conversational shorthand, nicknames, or euphemisms into the docs. Name the actual component, behavior, or value. Documentation is written for a reader who was not present for the conversation.

* **Extracted Assets Go in `build/`:** All extracted assets (tilesets, sprites, graphics) must be saved into the `build/` directory (e.g. `build/extracted_zelda/`). Do not copy or output files to external artifact/brain workspaces.

* **Script Creation & DRY Policy:** When asked to create a new script, or when creating one to accomplish a task, you MUST:
  1. Thoroughly scan the existing scripts in the project to ensure the new script aligns with established workflows, naming conventions, and shared library usage.
  2. Follow the DRY (Don't Repeat Yourself) principle rigorously. If existing scripts perform similar tasks, incorporate that logic into a shared utility or extend existing scripts rather than creating duplicates.
  3. Periodically suggest opportunities to refactor and combine existing scripts to consolidate functionality and improve maintainability.
* **Always `make clean` First:** Never run a bare `make`/`make -j` to rebuild. Always run `make clean` before building so stale objects (especially under `-flto`) can't mask source changes or produce a misleading binary size. The full incantation is `make clean && make -j16`.
* **No Python/Shell One-Liners:** Do not run insane Python/Shell/Bash one-liners. Always save Python code as a script under `scripts/debug/`.
* **Never Blindly Delete File Contents:** Never use a command line tool like sed to blindly delete parts of a file. Always use the `replace` or `write_file` tools to ensure you don't remove uncommitted work.
* **Iterate on Examples:** When given examples, do not only look at them once and then go off on a tangent: continuously reiterate over them until you have truly distilled all there is to learn from them. Refer to example specifications or other parts of this repository first when stuck on hardware/chainloader issues.
* **OpenOCD Error Handling:** The `Unable to parse read_uint32 response: ""` hang is now rare (largely fixed alongside the gnwmanager openocd-stderr drain, to which it was partially connected). Do not treat the probe as not-ready by default and do not prefix flashes with a reset as a matter of course. If the error does still appear, `python3 scripts/debug/trace.py reset-halt` forces a hardware reset of the MCU and clears it; reach for that only as a fallback when the error actually occurs.
* **Check Debug Scripts First:** Always check the scripts under `scripts/debug/` first when debugging, resetting, or checking the state of the device. Avoid running generic shell tools like `gnwmanager info` unless absolutely necessary as a fallback.
