# TD-Sop-Anim-Bridge

## Project Idea

This project is about saving and exporting animation from TouchDesigner into 3D exchange formats that support frame sequences and animated geometry.

The main focus is direct export from the SOP / Surface Operators context into formats such as Alembic (`.abc`) or USD (`.usd`, `.usda`, `.usdc`) so the resulting files can be imported into Blender, Houdini, and other 3D tools.

## Motivation

The current workflow is too indirect:

1. TouchDesigner geometry has to be saved frame by frame.
2. A temporary format or workaround such as `bhclassic` is often used.
3. Houdini is then used to combine those frames into one animated `.abc` or `.usd` file.
4. Only after that can the file be imported cleanly into Blender or another DCC.

The goal is to remove this intermediate step and build a more direct bridge from TouchDesigner SOP animation to standard animated 3D exchange files.

## Agent Context

- This is not just a static geometry exporter.
- Account for frame sequences, topology changes, vertex attributes, point attributes, normals, UVs, colors, and transform/geometry animation.
- Alembic and USD are the priority target formats.
- Blender and Houdini are important target applications for validation.
- TouchDesigner SOP context is the primary data source.
- Solutions should fit real production workflows, not only demo proof-of-concepts.

## Possible Directions

- Investigate TouchDesigner Python API support for reading SOP geometry per frame.
- Build an exporter that samples a SOP across a frame range and writes one animated file.
- Evaluate Python libraries and CLI tools for writing Alembic/USD.
- Treat a Houdini bridge as a fallback, not the main goal.
- Document limitations: changing topology, large caches, attributes, FPS, frame range, and scale/orientation differences between TD, Houdini, and Blender.

## Language

Documentation and external project communication are English-only.

- Keep repository docs, changelog entries, issues, release notes, public comments, and user-facing project text in English.
- Translate existing non-English project documentation when touching it.
- Internal reasoning can happen silently in any language, but the output committed to the project should be English.

## Changelog - Document All Changes

ALWAYS update `docs/changelog.md` when making changes.

- Bug fixes, new features, refactors, documentation updates, project rule changes, and other repository changes all go in the changelog.
- Format: `## [YYYY-MM-DD] Brief Title` followed by bullet points.
- Include affected files and migrations. If there are no migrations, write `Migrations: none`.

## Investigate Before Answering

ALWAYS read and understand relevant files before proposing or making code edits.

- If the user references a specific file or path, MUST open and inspect it before explaining it or proposing fixes.
- Be rigorous in searching the codebase for key facts before making claims about behavior.
- Thoroughly review the style, conventions, and existing abstractions of the codebase before implementing new features.
- Never speculate about code, project behavior, or implementation details that have not been inspected.
- If the relevant files are not obvious, search the repository first and inspect the files that define the behavior being changed.
- State assumptions explicitly when local evidence is incomplete.

## Challenge Before Agreeing

When the user proposes a change in strategy, positioning, architecture, or UX, DO NOT agree immediately.

1. Defend the current solution first. Recall why it was chosen, what problem it solved, and what is lost by abandoning it.
2. Attack the proposal. Identify weak points, risks, and non-obvious consequences.
3. Only then give a position: which side is stronger and why. `I do not know, need data` is a valid answer.

If you catch yourself simply repackaging the user's words into an argument, say so directly.

## No Hardcoding - 100% Data-Driven

For model/provider-specific behavior, prefer data-driven configuration over hardcoded branches.

- NEVER encode model or provider behavior with checks like `if model.startswith('midjourney')` or `if provider == 'kie_ai'`.
- ALWAYS represent model/provider capabilities and parsing rules in YAML config, using fields such as `response_parser`, `requires_image`, and `asset_type`.
- Test every model/provider decision with this question: `Will this break when we add 100 new models?`
- If the answer is yes, refactor the behavior into YAML-driven configuration.

## Brevity - Hard Requirement

The user's attention context window is narrow. Long walls of text are pain, not service.

- Default response length: 3-7 lines.
- Use long breakdowns only when the user explicitly asks for detailed analysis, for example `ULTRATHINK`, `detailed`, or `break it down`.
- Do not use tables or numbered lists for routine communication. Add structure only when comparing options or when it materially improves clarity.
- Do not use preambles like `I will investigate`, `let's think`, or `so`. Go straight to the point.
- Do not add final summaries just to restate visible diffs.
- When asking for a decision, ask one question. If there is a recommendation, state it in one line and wait for ack/nack.
- If the answer is getting long, reduce it to the core thesis. Length is not depth.
