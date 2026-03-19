---
name: comment-new-functions
description: Enforce comment quality for generated code. Use when creating or modifying code that introduces new functions or complex logic, especially when users request readable/maintainable code or explicitly ask for comments (e.g., “为新函数加注释”, “add comments to new functions”). Ensure each new function has a concise purpose comment and key implementation steps are annotated.
---

# Comment New Functions

Apply a strict commenting standard while generating or editing code:
- Add a concise comment for every newly created function.
- Add brief intent comments before important implementation steps.

## Commenting Rules

1. Detect language and local comment style first.
2. Follow existing project conventions for syntax and tone.
3. Write function-level comments for every new function.
4. Place step-level comments before non-trivial logic blocks.
5. Keep comments synchronized with code changes.

## Function-Level Comments

For each new function, add a short comment that states:
- What the function does.
- Why it exists when intent is not obvious.
- Critical side effects or constraints when relevant.

Keep it concise:
- Prefer 1-3 lines.
- Avoid repeating the function name literally.
- Do not document trivial assignments.

## Step-Level Comments

Add comments at important steps, such as:
- Branches with business rules or risk controls.
- Data transformations with non-obvious assumptions.
- External I/O, IPC, database, network, or file interactions.
- Error-handling paths and fallback logic.
- Performance-sensitive or concurrency-critical sections.

Comment intent, not mechanics:
- Good: explain why this step is needed.
- Avoid: narrating each line.

## Quality Bar

- Ensure comments are accurate after every code edit.
- Remove stale comments if logic changes.
- Keep wording clear and specific.
- Prefer fewer high-value comments over many low-value comments.

## Output Checklist

Before finishing any coding task:
1. Confirm every new function has a comment.
2. Confirm important logic steps are annotated.
3. Confirm comments follow repo style and language syntax.
4. Confirm no misleading or redundant comments remain.
