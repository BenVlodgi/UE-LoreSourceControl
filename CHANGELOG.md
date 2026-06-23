# Changelog

Notable changes to LoreSourceControl, the Unreal Editor Revision Control provider for Lore.

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
