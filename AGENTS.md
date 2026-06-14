# TD-Sop-Anim-Bridge

## Идея проекта

Проект посвящен сохранению и экспорту анимации из TouchDesigner в обменные 3D-форматы, которые поддерживают последовательности кадров и анимированную геометрию.

Главный фокус: экспортировать анимацию напрямую из контекста SOP / Surface Operators в форматы вроде Alembic (`.abc`) или USD (`.usd`, `.usda`, `.usdc`), чтобы такие файлы можно было дальше импортировать в Blender, Houdini и другие 3D-пакеты.

## Зачем это нужно

Сейчас рабочий процесс неудобный:

1. В TouchDesigner приходится сохранять геометрию покадрово.
2. Часто используется промежуточный формат или обходной путь через `bhclassic`.
3. Затем в Houdini эти кадры нужно вручную или полуавтоматически объединять в один анимированный `.abc` или `.usd` файл.
4. Только после этого файл можно нормально импортировать, например, в Blender.

Цель проекта — убрать этот муторный промежуточный этап и сделать более прямой мост от TouchDesigner SOP-анимации к стандартным анимированным 3D exchange-файлам.

## Что важно учитывать агентам

- Это не просто экспорт статичной геометрии.
- Нужно думать о frame sequence, topology changes, vertex attributes, point attributes, normals, UV, colors и transform/geometry animation.
- Alembic и USD являются приоритетными целевыми форматами.
- Blender и Houdini — важные целевые приложения для проверки результата.
- TouchDesigner SOP-контекст — основной источник данных.
- Решения должны быть пригодны для реального production workflow, а не только для демонстрационного proof of concept.

## Возможные направления

- Исследовать существующие возможности TouchDesigner Python API для чтения SOP-геометрии по кадрам.
- Сделать экспортёр, который семплирует SOP на диапазоне кадров и пишет один анимированный файл.
- Проверить Python-библиотеки и CLI-инструменты для записи Alembic/USD.
- Рассмотреть промежуточный bridge через Houdini только как fallback, а не как основную цель.
- Документировать ограничения: меняющаяся топология, большие кэши, атрибуты, FPS, frame range, scale/orientation differences между TD, Houdini и Blender.

## Changelog — Document All Changes

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
