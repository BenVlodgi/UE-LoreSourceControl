# Changelog

Notable changes to LoreSourceControl, the Unreal Editor Revision Control provider for Lore.

## 0.4

### Added
- Recovery from an interrupted working-tree rewrite. Lore has no rewrite transaction of its own, so a crash mid-sync leaves a half-updated tree. The plugin records each rewrite in a marker and, on the next connect or when a rewrite fails in session, offers a notification: Recover resets the half-updated files to the pre-operation revision and re-runs the operation, and Dismiss leaves them as ordinary local modifications to inspect by hand.
- The revision data layer for the history browser: list a branch's revisions newest first with paging, and read one revision's commit metadata and per-file delta, through the native library with command-line mirrors that parse the same `--json` event stream.
- The branch data layer for the branches window: enumerate the repository's branches merged into one row per name (local and remote latest, current, archived), read each branch's protection state from the `protect` branch metadata key, and dispatch the branch and history reads as source control operations the window can run through the provider. The reads are local, so they work offline.
- The branches window and the repository history browser, on UE 5.1 and later. The Revision Control status-bar menu gains View Branches and View History entries while the Lore provider is active. The branches window lists every branch with the current one bold, its category, creator, created time, location (local, remote, or both, with a divergence marker), latest hash, and a protection indicator, with search, per-column sort, and an archived toggle; the stash branches stay hidden. The history browser is a dual pane keyed by revision: a paged revision list per branch above (newest first, one call per page, Load more until the first revision), and the selected revision's changed files below, with File History opening the engine's per-file history window.
- The guarded branch write actions in the branches window's context menu. Switch to a branch saves dirty packages first, unstages, enumerates the delta with a dry-run switch, clears the read-only attribute on the delta files (Lore's rewrite aborts on a read-only file), switches, reapplies read-only, then reloads the rewritten packages, all bracketed by the crash-protection marker so an interrupted switch is recoverable. Create a branch names a new branch and switches to it at the current revision (rewriting no files). Archive a branch removes it from the default listing (there is no unarchive). Protect and unprotect set and clear the branch's advisory protect flag, whose enforcement is the server rejecting a push. The switch never passes force, reset, or bare, so a conflicting dirty file refuses it rather than being overwritten. The create-at-revision entry in the history browser stays disabled until the guarded move between revisions lands, since creating at a historical revision needs that move.
- The changelist write workers, on UE 5.3 and later: create a changelist with a description, edit the description, move files between changelists (including out of Default), delete a changelist (the Default changelist refuses), and refresh the pending changelists. Each is a pure edit of the plugin-owned changelist store, so editor behavior is unchanged until the changelist surface turns on.
- The editor changelist surface, on UE 5.3 and later. The Changes view lists the plugin's changelists: a changed file not assigned elsewhere is collected into the Default changelist when the view reads state, and submitting a changelist commits exactly its member files. A named changelist emptied by its submit is removed, matching the Plastic provider; the Default changelist only empties. The uncontrolled changelist feature stays available alongside, matching the Perforce and Plastic providers.

### Fixed
- Sync completes through read-only assets. With locking on, a not-checked-out binary asset is read-only on disk, and Lore's working-tree rewrite aborts on a read-only file inside its delta, so a sync that touched one stalled and aborted midway, leaving the tree half-updated. Sync now runs behind a guard that clears the read-only attribute on the files it will rewrite, syncs, then reapplies read-only per the locking policy, so it finishes and the assets read read-only again.
- A changelist edit no longer disappears when the Changes view refreshes while the edit lands. The changelist workers write the store from pool threads while the view's reconciliation writes it from the game thread, so an unguarded load-edit-save could lose whichever write landed in between; every compound store access now runs under one process-wide lock.
- A changelist-keyed submit commits exactly the membership the Changes view rendered. The member files expand into the command at issue time on the game thread, instead of the worker re-reading the store on the pool thread, where a concurrent edit could change the set mid-flight.

### Changed
- Offline responsiveness. When the remote is unreachable the editor no longer blocks on a lock check that cannot succeed. Each status pass reads reachability fresh from its own probe and skips the server-side lock query when the remote is down; check out then skips the lock acquire and makes the file writable, and the check-out action grays out so the save prompt offers Make Writable. The native library reuses a cached connection, so it detects an unreachable server when it connects, and a reconnect re-checks; the command-line backend, a fresh process each call, also detects a server that drops mid-session or returns. Before this, a save against a down server hung for minutes on the lock query.

### Testing
- Crash-protection coverage: the dry-run sync enumeration names exactly the changed file with the right action token and touches nothing (the anchor stays put), on both backends; the status revision anchors reader returns the known branch, current revision, and remote latest; the guarded rewrite helper syncs through a read-only file and reapplies read-only on success, keeps the marker on a failed rewrite, and drives a full FSync through the provider over a read-only delta file; and detection plus recovery restores a torn file to the anchor and clears the marker, clears a marker over a clean tree, and leaves a marker from another instance alone.
- Offline coverage: with the remote unreachable the status pass skips the lock query (a held lock reads as not held) and check out reads unavailable; a check out while offline makes the asset writable without a lock; and a status pass updates the provider's reachability in both directions. The drop-detection tests run on the command-line backend, since the native library's cached connection masks a mid-session drop.
- Changelist surface coverage: the changelist state query keeps an explicit assignment, collects an unassigned changed file into Default, resolves member files to their cached states, persists the reconciled membership, and refuses to submit an empty changelist, all against a bare temporary root; a live round trip submits a changelist by identifier and asserts exactly its member files commit and the emptied record goes away.
- Store concurrency coverage: a pool thread and the test thread interleave a few hundred changelist store mutates and no edit is lost; a live changelist-keyed submit issued under the store lock, with its membership emptied before release, still commits the member files, pinning the expansion to issue time.
- Branch and history read coverage: the `protect` metadata key reads absent on a fresh branch, true after a protect, and false after an unprotect, with the native path and the command-line mirror in agreement; the branch item merge folds local-only, remote-only, agreeing, diverging, and archived branches to the right rows as a pure function; a live branch read enumerates the branches with the current branch flagged and the protected branch read; and the three read operations dispatch through the provider and carry their rows on the operation object (branches with the current flag, revisions newest first, and a revision's changed files filtered to files).
- Guarded branch write coverage: a dry-run switch to another branch names exactly the changed file with the keep action token and leaves the tree on its branch, on both backends; a live round trip creates a branch, switches back over a read-only asset and asserts the tree lands on target with content and read-only restored, then protects and reads the flag back, unprotects, and archives; and an interrupted switch, recorded as a branchSwitch marker with a target branch, drives the switch re-dispatch arm of recovery, which resets the torn file and lands the tree on the recorded branch.

## 0.3.1

Fix a submit that left a file checked out and the save failure it caused on the next edit, fix a diff that compared an asset against itself, log every operation's outcome, and add automated coverage for every operation.

### Fixed
- Submit releases the lock on the committed files. Against a server with authentication off the released lock read back as still held, so the file stayed checked out and read-only and the next edit failed to save. Submit now asserts the released state the way revert does.
- Diff against depot extracts the server revision instead of silently extracting the working copy, so an asset no longer diffs against itself.
- The settings panel hides the token sign-in row when the server is unreachable or has authentication off, where pasting a token does nothing.

### Changed
- Every operation logs its outcome under `LogLoreSourceControl`, with the reasons on failure, so a problem is visible in the log rather than only as a generic editor dialog. A revision extraction that fails for diff names the revision and points at server reachability, the case behind a historical diff reporting unsupported content.
- File history marks a revision whose content has left the local store with an `[Unavailable]` prefix, and a diff of such a revision raises a notification naming the asset and the cause instead of the engine's generic "content unsupported" dialog. A revision is marked only on a definitive not-local answer from the native store probe, so freshly committed local content never reads as unavailable.

### Testing
- The in-plugin automation suite covers every operation and the four reported regressions (submit releases the lock and leaves the file writable, re-edit then resubmit succeeds, diff against depot yields the server revision, historical revision extraction is loadable), run unattended against the managed local server.
- The local-store content probe is covered directly: a present address reads as local, a bogus address reads as absent, history captures each revision's content address, and an obliterated revision reads as unavailable and refuses extraction.

## 0.3

Drive the editor's source control operations through one client with a selectable backend, native by default on Windows, add sign-in from the settings panel, and make the plugin's text translatable.

### Added
- Native by default, with a selectable backend. Every operation, including connect and discovery, runs through one client with a `Backend` setting: `Native`, the default on Windows, calls the Lore native library (liblore) in process and never launches the `lore` command-line client; `Command line` runs the subprocess; `Automatic` tries native and falls back to the command line on an error. The native library links when present in the build, so a release package runs native while a source tree without it builds the command-line path.
- Sign in and sign out from the settings panel. Paste a Lore token, sign in through a browser, or sign out, all through the active backend. With the native backend this resolves the identity and stores or clears the credential in process, with no command-line client.

### Changed
- The backend selector and the command-line client controls (the binary path and the downloader) sit under an Advanced section of the settings panel that stays folded under the native default, where there is no command-line client to point at. The command-line path is offered for a platform without the library, a performance comparison, and crash isolation, since liblore runs in process while the command-line client is a separate process.
- The submit dialog title names the current Lore branch.
- User-facing text is translatable. The submit-blocker warnings, the over-lock dialog, the file history action labels, and the operation errors build from localizable text.

### Fixed
- A submit no longer offers engine or plugin content for add. A file outside the working tree reads as unknown rather than addable, so pressing Submit gathers only the project's own assets.
- A submit with nothing changed makes no commit, instead of reporting an error.
- The editor stays responsive during a synchronous operation, showing a cancellable progress dialog only once the operation runs past half a second.

### Engine support
- Unreal Engine 4.27 through 5.8. The native client is Windows only; other platforms run the command-line client.

## 0.2

Use the Lore client's JSON output, improve lock handling and submit, and extend engine coverage down to Unreal Engine 4.26.

### Added
- Check In Over Lock: a Content Browser action that pushes a revision over another user's lock, after a dialog warning that two binary commits collide.
- Authentication row in the Revision Control settings panel, showing the server's state ("Not required", "Required, not signed in" with a sign-in button, or "Server not reachable"). The sign-in button appears only when it would do something.
- Improved asset source control icon handling, including the Content Browser list view, with a modified-locally icon and a not-at-head icon.
- A combined asset tooltip carrying lock, conflict, and out-of-date state.
- Tests for the authentication-state parse.

### Changed
- Read the Lore client's JSON output instead of scraping text for status, locks, and history. This gives typed actions, staged and dirty flags, the conflict-flag set, move sources, and exact ahead and behind state.
- Refuse a submit that would be rejected before it stages anything, so a failed submit never strands a local commit.
- Submit a file that is behind the server: the commit stays local, and a rejected push is reported so you can sync and retry.
- Show a file as locked by another user even when it also has local changes.
- File history shows readable action labels (Add, Keep (Edit), Delete, Move, Copy) instead of the raw client tokens.
- Keep transient folders (Binaries, Intermediate, Saved, and the caches) out of the status and submit lists.

### Fixed
- A synchronous operation no longer shows a second, duplicate progress dialog.
- Revert releases only the locks you hold, instead of releasing locks you never had.
- Read-only state is re-applied from the lock on every status refresh, keeping an unlocked binary read-only.

### Engine support
- Unreal Engine 4.26 through 5.8.

### Known limitations
- Authentication has no public Lore server yet. Epic's UCS authentication is private, and OAuth support is on Lore's roadmap. On an unauthenticated server every lock owner reads as `<unknown>`, so a real owner name and the sign-in flow appear only once a server is authenticated.

## 0.1

Lore integration set up.

### Added
- Unreal Engine 4.27 through 5.8.
- Automatic discovery of the Lore client, with a download button.
- Connect to a locally configured Lore repository.
- File status query.
- Check out with a lock, mark for add, delete, and revert.
- Submit and check in, with an option to keep the lock.
- Sync.
- History, and diff of the local file and of historical revisions.
- Move and rename tracking.
- Regression tests.
