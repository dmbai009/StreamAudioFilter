# Updates and compatibility

StreamAudioFilter currently ships a complete Vencord build with a custom plugin.
Vencord's stock updater is disabled in this build because it replaces the whole
distribution with official Vencord, which does not include StreamAudioFilter.
Install a newer StreamAudioFilter release to update both together. Settings and
application rules are preserved.

For a newer Vencord version before another prebuilt bundle is available, follow
[Rebuilding with a newer Vencord](REBUILDING.md).

Discord updates independently. Its internal JavaScript and native screen-share
interfaces can change while the custom Vencord bundle stays fixed. Updating
Vencord can fix startup incompatibilities, but the plugin's screen-share patches
also need checking. A successful build alone does not prove either startup or
stream audio works.

## What 0.3.2 fixes

The previous bundle's Vencord v1.15.11 stopped Discord Stable 1.0.9268 (web build
673869) at startup with `i.A is not a constructor`. The replacement based on
Vencord v1.15.17 reached a connected Discord session and started StreamAudioFilter
on the affected machine. This verifies startup; it is not a new live-stream audio
verification.

## Reducing the risk

- Keep the tested Vencord version pinned in releases. Automatically shipping an
  untested upstream build can replace one incompatibility with another.
- Check new Vencord revisions regularly, rebuild the plugin, and run the existing
  compiler, rule, protocol, installer and panel checks before publishing.
- Add a separate check against current Discord code for the real patch targets,
  plus a real-client startup and screen-share check. The existing regex tests use
  fixtures; they do not prove that today's Discord still matches.
- A future installer/launcher could verify that a new build finishes startup and
  offer to restore the saved working build if it fails. The current installer has
  transactional file rollback and uninstall backups, but no startup watchdog.
- Inclusion in official Vencord would remove the need to distribute a separate
  Vencord build. This depends on upstream acceptance and would not eliminate
  breakage caused by future Discord changes.

These are maintenance directions, not automatic protections implemented in 0.3.2.
Custom plugins currently need a source build; see the official
[custom-plugin instructions](https://docs.vencord.dev/installing/custom-plugins/)
and [plugin setup documentation](https://docs.vencord.dev/plugins/).

## Recovery

First try the latest StreamAudioFilter installer. If Discord still cannot start,
quit Discord completely and uninstall StreamAudioFilter with settings preserved.
When `dist.saf-backup` exists, uninstall restores the Vencord build saved before
the plugin was installed. That saved build may itself need an official Vencord
update if Discord has changed since it was saved. No Discord account data or
shared database needs deleting for this recovery.
