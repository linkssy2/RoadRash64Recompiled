# Editing the local race additions

- `native/src/rr64_local_race_options.cpp`: persisted local options, menu layout and roster/bike choices. Keep local/online gates and human-count bounds.
- `native/src/rr64_custom_cop.cpp`: selection, roles, busts, equipment and race completion. Do not alter unlock tables or race eligibility to fix recovery.
- `native/src/rr64_custom_cop_runtime.cpp`: roadside placement, tap/hold input and recovery bridges. Guest a1 is newly pressed and a2 is held; preserve one toggle per hold.
- `native/src/rr64_custom_cop_ui.cpp`: original-font hints and HUD text.
- `config/roadrash64.us.toml`: original function hook sites; regenerate guest output after edits.
- `native/tests/rr64_local_race_options_smoke.cpp`: bounded roster, cop, control and recovery fixtures.
- `native/tests/rr64_action_bindings_smoke.cpp`: stable action IDs, defaults and named binding serialization. Append actions; do not renumber existing ones.

The release omits the temporary body recorder. Investigation source and captures stay in private analysis folders. Preserve frame ownership and matching guards when changing FPS admission; diagnostics are not a substitute for visual acceptance.
