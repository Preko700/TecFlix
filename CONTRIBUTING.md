# Guía de contribución — Tecflix

## Antes de escribir código

1. Leé [`SPEC.md`](./SPEC.md) completo — es la fuente de verdad de
   arquitectura y alcance.
2. Si vas a usar un agente de IA, dale `SPEC.md` como contexto y el README
   del componente específico donde vas a trabajar.

## Flujo de trabajo

1. Crear rama desde `main`: `feature/<componente>-<tema>`
   (ej. `feature/user-api-btree-insert`).
2. Commits descriptivos, idealmente Conventional Commits
   (`feat:`, `fix:`, `test:`, `docs:`, `ci:`, `refactor:`).
3. Abrir Pull Request hacia `main` usando la plantilla del repo.
4. **Al menos 1 aprobación de otro miembro del equipo** antes de merge.
5. El CI del componente correspondiente debe estar en verde.
6. Si el cambio afecta arquitectura o contratos entre componentes,
   actualizar `SPEC.md` en el mismo PR.

## Reglas duras del proyecto (no negociables)

- Todos deben poder explicar en la defensa cualquier parte del código,
  incluyendo lo generado por AI. Si usás AI, entendé lo que genera antes de
  hacer commit.
- El paper de V2.0 se escribe **sin AI**.
- Evitar malas prácticas de programación — cada una detectada en revisión
  resta 5 puntos. Usá los linters/formatters configurados en cada
  componente antes de pedir review.
- Los componentes deben integrar entre sí — no basta con que cada uno
  funcione por separado.

## Estructura del repo

Ver [`docs/repo-structure.md`](./docs/repo-structure.md).
