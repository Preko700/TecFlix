# SPEC.md — Tecflix

> Este documento es la especificación viva del proyecto **Tecflix** (CE 2103 —
> Algoritmos y Estructuras de Datos II, TEC, II Semestre 2026). Sirve como
> input principal para cualquier agente de IA que colabore en el código, y
> como referencia única de arquitectura y alcance para el equipo.
>
> Metodología: **spec-driven development**. Cualquier cambio de arquitectura
> o de alcance debe reflejarse aquí ANTES o EN el mismo PR que lo implementa.
> Un agente de IA que trabaje en este repo debe leer este archivo completo
> antes de generar código.

## 1. Objetivo del proyecto

Diseñar e implementar un sistema de streaming de video, aplicando el
paradigma de orientación a objetos, dividido en componentes independientes
que se comunican entre sí.

## 2. Estado del proyecto

- [ ] Versión 1.0 (single-server, funcionalidad completa)
- [ ] Versión 2.0 (alta disponibilidad, performance, load-testing)

La rama `main` contiene el desarrollo activo. Al cerrar cada versión se crea
un tag (`v1.0`, `v2.0`) y opcionalmente una rama de mantenimiento
(`release/v1.0`, `release/v2.0`).

## 3. Arquitectura general

```
                        ┌─────────────────────┐
                        │   Aplicación Web     │
                        │  (React + TypeScript)│
                        └──────────┬───────────┘
                                   │ HTTPS/REST
           ┌───────────────────────┼───────────────────────┐
           │                       │                       │
┌──────────▼─────────┐  ┌──────────▼─────────┐  ┌──────────▼─────────┐
│    API Usuarios     │  │   API Metadata      │  │   API Streaming     │
│   (C++, REST)       │  │   (C++, REST)       │  │   (C++, REST)       │
│  Árbol B en disco    │  │   MongoDB           │  │   FFMPEG transcode  │
│  (archivo binario)   │  │                     │  │                     │
└──────────┬──────────┘  └──────────┬──────────┘  └──────────┬──────────┘
           │                       │                       │
           └───────────────────────┼───────────────────────┘
                                   │ OpenTelemetry (traces/métricas)
                        ┌──────────▼───────────┐
                        │  Dashboard / Observ.  │
                        │ (Prometheus/Aspire)  │
                        └───────────────────────┘
```

Cada componente es independiente, con su propio ciclo de build, pruebas y
(en V2.0) su propio par de instancias redundantes.

## 4. Componentes

### 4.1 Aplicación Web (`webapp/`)

- Stack: **ReactJS + TypeScript**.
- Flujo: pantalla de registro/login → lista de videos (propios + públicos de
  otros) con orden por título/autor/fecha de carga (delegado al API de
  metadata) y **paginación obligatoria**.
- Reproducción de video con reproductor open-source compatible con el API de
  streaming (Shaka Player o Dash.js).
- Gestión de metadata y de acceso (público/privado) por video.
- Cada usuario tiene una cuota de almacenamiento fija (no editable); al
  alcanzarla, se bloquea la subida de nuevos videos.

### 4.2 API de Usuarios (`services/user-api/`)

- Stack: **C++**, REST API.
- Operaciones: registrar, actualizar, eliminar, consultar usuarios.
- Persistencia: archivo binario propio en disco.
- Índice de búsqueda: **árbol B en disco** (implementación propia, no usar
  una librería que lo resuelva — es contenido del curso).

### 4.3 API de Metadata (`services/metadata-api/`)

- Stack: **C++**, REST API.
- Gestiona metadata de videos: autor, duración, fecha de carga, tema,
  título, etiquetas, etc.
- Persistencia: **MongoDB**.
- Responsable del ordenamiento/listado que consume la webapp.

### 4.4 API de Streaming (`services/streaming-api/`)

- Stack: **C++**, REST API.
- Sirve bloques/frames de video al reproductor (protocolo compatible con
  Shaka/Dash.js — DASH o HLS).
- Transcodifica video al momento de la subida usando **FFMPEG**.

### 4.5 Dashboard del sistema (`dashboard/`)

- Todos los componentes instrumentados con **OpenTelemetry** (traces +
  métricas).
- Visualización mediante Prometheus, Aspire o similar.
- Debe permitir ver: errores, salud por componente, desempeño por
  componente, estadísticas generales del sistema.

## 5. Requerimientos V2.0

1. **Eliminar single points of failure**: todo excepto la webapp corre en al
   menos 2 máquinas/instancias separadas por componente. Implica
   coordinación/balanceo de carga y consistencia de datos — documentar
   decisiones en el paper.
2. **Mejorar performance**: identificar ≥5 oportunidades de optimización a
   partir de las métricas del dashboard de V1.0, implementarlas y medir el
   antes/después.
3. **Load-test**: pruebas de carga/stress con usuarios concurrentes,
   comparando 1 servidor vs. 2, documentando dónde el sistema degrada o
   empieza a rechazar peticiones. Scripts en `infra/loadtest/`.
4. **Integración continua**: pipelines de GitHub Actions que compilan y
   empaquetan cada componente, con **cobertura de pruebas unitarias ≥90%**.
   Los ejecutables de revisión se descargan desde GitHub Releases/Artifacts.
5. **Pruebas funcionales**: con Playwright (o similar), integradas al
   pipeline de CI.

## 6. Convenciones del repositorio

- **Monorepo**: un solo repositorio con carpetas por componente (ver
  `docs/repo-structure.md`).
- **Ramas**: `main` protegida, trabajo en `feature/<componente>-<tema>`,
  PRs obligatorios con al menos 1 revisión de otro miembro del equipo.
- **Tags**: `v1.0` y `v2.0` marcan las entregas de cada versión.
- **Commits**: mensajes descriptivos en español o inglés (consistente
  dentro de cada componente), preferiblemente Conventional Commits
  (`feat:`, `fix:`, `test:`, `docs:`, `ci:`, `refactor:`).
- **Code review obligatorio**: nadie hace merge a `main` de su propio PR sin
  aprobación.
- Cada mala práctica de programación detectada en revisión se penaliza con
  5 puntos — usar linters/formatters en CI para atraparlas antes.

## 7. Entregables y evaluación (referencia rápida)

| Aspecto                                    | Valor |
|---------------------------------------------|-------|
| Aplicación Web                              | 10    |
| API de usuarios                             | 15    |
| API de metadata                             | 5     |
| API de streaming                            | 15    |
| Dashboard del sistema                       | 10    |
| Eliminar single-point of failures (V2.0)    | 20    |
| Load-test                                   | 15    |
| Integración continua                        | 5     |
| Pruebas funcionales                         | 5     |
| **Total código**                            | **100 (80% de la nota del curso)** |
| Paper (sin AI, foco en V2.0)                | 20%   |

Reglas duras a recordar:
- Si los componentes "sirven por aparte pero no en conjunto" → nota total = 0.
- Si en la defensa alguien del equipo no puede explicar cualquier parte de
  la arquitectura o decisión de diseño → nota del proyecto = 0.
- El paper se escribe **sin ayuda de AI**; si se detecta, 0 puntos en esa
  parte.
- Uso de AI para código es libre, pero el equipo debe poder justificar y
  explicar TODO lo generado.

## 8. Cómo debe trabajar un agente de IA en este repo

1. Leer este `SPEC.md` completo antes de proponer o generar código.
2. Ubicar el cambio en el componente correcto (`webapp/`, `services/*`,
   `dashboard/`, `infra/`).
3. No introducir dependencias que resuelvan de fábrica algo que es objetivo
   de aprendizaje del curso (p. ej. el árbol B del API de usuarios).
4. Actualizar este archivo cuando el cambio afecte arquitectura, contratos
   entre componentes, o alcance.
5. Todo código nuevo en `services/*` debe venir acompañado de pruebas
   unitarias (meta: 90% cobertura) y pasar el pipeline de CI correspondiente.
