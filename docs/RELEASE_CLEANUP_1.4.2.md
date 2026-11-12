# 1.4.2 source cleanup

Reviewed new item, audio and campaign modules against their production callers.
Removed draw_hud's obsolete scalar-size wrapper and its unused fixture stub;
tests now exercise draw_hud_rectangle directly. The runtime already used the
rectangle API, so this removes no supported gameplay path. Retained bounded
render statistics because regression tests consume them, and retained optional
diagnostic producers for future bug reports. Generated guest temporaries are
not handwritten dead code and were not edited.

Formatted the new modules with the project's clang-format rules; all formatting
changes were checked for identical non-whitespace bytes. No dependency or
generated-code rewrite. Added a responsibility/ownership guide and replaced
private candidate chronology with current feature documentation. Logs, test
launchers, profiles, generated game output and obsolete source mirrors are
excluded from published packages. Historical private artifacts remain intact.

No game was launched for release preparation. Final live MK64 item acceptance
remains outstanding and is explicitly stated in the release notes.
