# Boot intro skipping

A or Start advances one post-logo card per press. The first logo keeps its
original timing; either button on the title page opens the main menu and marks
the player session started. Holding a button does not repeatedly advance.

This replaces our two earlier hooks with heyitsjames's merged PR #27
(`2d3a3cc77563ab094ed0c4bb417eb1543fe96768`, including the session fix).
The single entry hook in `80021B0C` calls `rr64_intro_skip`.
`RR64BootIntroSmoke` exercises the generated routines with rendering stubbed:
logo boundaries, page stepping, held/unrelated/no input, session activation,
and the unchanged native title handler.
Live boot presentation still needs the next authorized test.
