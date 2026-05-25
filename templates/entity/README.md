# Entity templates

Placeholders use Go `text/template` syntax directly (`{{.Title}}`, `{{.PlanID}}`, etc.); the loader renders them via `text/template` with `missingkey=error`.
