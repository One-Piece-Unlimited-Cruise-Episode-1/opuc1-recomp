# Contributing to One Piece: Unlimited Recompilation Episode 1

Thanks for wanting to help. A few ground rules.

## The short version

- Code is judged on quality, not on where it came from.
- You must understand and be able to explain every line you submit.
- PR descriptions and replies are written by you, not generated.
- Accuracy is the bar for anything that touches game behaviour.

## Code quality

What counts is whether a change meets or improves the project's patterns, and the only way to measure that is by
reading it. Low-quality code is not merged, whatever its origin.

Project rules that every change follows:

- Never edit `generated/`. Change the function map, the rebind overrides, the runtime or the translator instead.
- Every game address in runtime code comes from `projects/opuc1/MAP.txt` or `projects/opuc1/rebind_overrides.tsv`,
  never typed in by hand without a note saying where it was read.
- Read the game's code (Ghidra) before guessing at runtime behaviour, and compare against Dolphin when the two disagree.
- Run `node tools/check-bindings.mjs` after touching HLE bindings.

## If you use AI tools

That's fine, with two rules:

1. **You must be able to explain your changes.** If a reviewer asks why a line exists and you can't answer, the PR is
   closed. "The AI wrote it" is not an explanation.
2. **Write your own PR description**: what you changed and why, in your words.

## Pull requests

- Keep PRs focused, ideally one change per PR.
- Explain **what** and **why**, and reference the issue if there is one.
- For anything that affects game behaviour, identical behaviour to the original game is the goal. Show that your change
  does not diverge from it (Dolphin comparison, logs, screenshots, whatever fits).

## Bug reports

Use the issue forms and attach the run log. See the FAQ in the [README](README.md).

## Legal

- Never include game code, assets, translated output (`generated/`), disc files or save data in a PR, an issue or
  anywhere else in this project. No exceptions.
- By contributing you agree that your contributions are licensed under [GPL v3.0](LICENSE), like the rest of the project.
