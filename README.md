# Tecflix

Sistema de streaming de video — Proyecto de **Algoritmos y Estructuras de
Datos II (CE 2103)**, Instituto Tecnológico de Costa Rica, II Semestre 2026.

> La especificación completa y viva del proyecto está en [`SPEC.md`](./SPEC.md).
> Cualquier agente de IA o persona nueva en el equipo debe leerla primero.

## Componentes

| Carpeta                  | Componente          | Stack                        |
|---------------------------|----------------------|-------------------------------|
| [`webapp/`](./webapp)               | Aplicación Web       | React + TypeScript           |
| [`services/user-api/`](./services/user-api)     | API de Usuarios      | C++ (árbol B en disco)       |
| [`services/metadata-api/`](./services/metadata-api) | API de Metadata      | C++ + MongoDB                |
| [`services/streaming-api/`](./services/streaming-api)| API de Streaming     | C++ + FFMPEG                 |
| [`dashboard/`](./dashboard)             | Dashboard / Observabilidad | OpenTelemetry + Prometheus/Aspire |
| [`infra/`](./infra)                | Docker, CI, load-testing | —                        |
| [`docs/`](./docs)                 | Documentación adicional / paper | Markdown              |

## Equipo

| Nombre | Carné | Rol / componente principal |
|--------|-------|------------------------------|
| Preko  | 2023800088 | _por definir_ |
|        |            |                |
|        |            |                |
|        |            |                |

## Cómo levantar el proyecto en local

Cada componente documenta su propio setup en su `README.md`. El resumen
rápido (una vez cada componente tenga su Dockerfile):

```bash
docker compose -f infra/docker/docker-compose.yml up --build
```

## Ramas y flujo de trabajo

- `main`: rama protegida, siempre desplegable.
- `feature/<componente>-<tema>`: trabajo en progreso, un PR por feature.
- `v1.0`, `v2.0`: tags de entrega de cada versión del proyecto.

Ver la sección 6 de [`SPEC.md`](./SPEC.md) para las convenciones completas.

## CI/CD

Los workflows de GitHub Actions viven en [`.github/workflows/`](./.github/workflows)
y compilan/prueban cada componente por separado (ver `docs/repo-structure.md`).

## Licencia / uso académico

Proyecto académico para CE 2103, TEC. Uso interno del equipo del curso.
