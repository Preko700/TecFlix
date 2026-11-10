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

## 9. Decisiones técnicas (V1.0)

| Tema | Decisión | Por qué |
|------|----------|---------|
| Lenguaje servicios | C++20, CMake + Ninja, GoogleTest, gcovr | Estándar del curso; cobertura medible en CI |
| Servidor HTTP | `cpp-httplib` | Header-only, un hilo por petición: fácil de explicar y de medir |
| JSON | `nlohmann/json` | — |
| Auth | JWT HS256 con secreto compartido (`TECFLIX_JWT_SECRET`) | Los servicios validan el token sin llamar al API de usuarios (sin estado → facilita V2.0) |
| MongoDB | driver C oficial (`libmongoc`) envuelto en una clase C++ | Está en apt; evita compilar `mongocxx` |
| Streaming | MPEG-DASH (fMP4, segmentos de 4 s, 360p + 720p) generado con el CLI de `ffmpeg` | Compatible con Shaka Player |
| Reproductor | Shaka Player | — |
| Observabilidad | SDK `opentelemetry-cpp` → OTLP → `grafana/otel-lgtm` (Collector + Prometheus + Tempo + Grafana en un contenedor) | Un solo contenedor cubre traces, métricas y dashboards |
| Entorno | Linux (WSL2 / Docker / runners `ubuntu-latest`) | FFMPEG y toolchain C++ |

Puertos locales: user-api `8081`, metadata-api `8082`, streaming-api `8083`,
webapp `5173`, Grafana `3000`, OTLP `4317/4318`, MongoDB `27017`.

Código compartido entre servicios: `services/common/` (JWT, helpers HTTP,
telemetría). Los tres servicios lo enlazan como librería estática.

## 10. Contratos entre componentes

Convenciones para **todos** los servicios:

- JSON en request/response. Errores: `{"error": "<mensaje>"}` con el código
  HTTP adecuado (400, 401, 403, 404, 409, 413, 500).
- `GET /health` → `200 {"status":"ok","service":"<nombre>","instance":"<id>"}`
  (sin autenticación; lo usan el dashboard y, en V2.0, el failover).
- Todo lo demás requiere `Authorization: Bearer <JWT>` salvo registro y login.
- CORS abierto (`Access-Control-Allow-Origin: *`); la webapp vive en otro origen.
- Configuración solo por variables de entorno (`PORT`, `TECFLIX_JWT_SECRET`,
  `INSTANCE_ID`, y las propias de cada servicio).

### 10.1 JWT

Header `{"alg":"HS256","typ":"JWT"}`. Claims:

| Claim | Tipo | Significado |
|-------|------|-------------|
| `sub` | string | id del usuario (o nombre del servicio) |
| `username` | string | nombre visible; metadata lo guarda como autor |
| `role` | `"user"` \| `"service"` | `service` = llamadas internas entre APIs |
| `quota` | number | cuota de almacenamiento en bytes (fija, se define al registrar) |
| `exp` | number | expiración (epoch, segundos) |

Lo emite el API de usuarios en el login. Implementación de referencia:
`services/common/include/tecflix/jwt.hpp` (`signJwt` / `verifyJwt`).

### 10.2 API de Usuarios (`:8081`)

| Método | Ruta | Body → Respuesta |
|--------|------|------------------|
| POST | `/api/users` | `{username,email,password}` → `201 User` (409 si el username existe) |
| POST | `/api/auth/login` | `{username,password}` → `200 {token, user: User}` (401 si falla) |
| GET | `/api/users/{id}` | → `200 User` |
| PUT | `/api/users/{id}` | `{email?,password?}` → `200 User` (solo el propio usuario) |
| DELETE | `/api/users/{id}` | → `204`; antes llama a `DELETE {metadata}/internal/users/{id}/videos` con token de servicio |

`User = {id, username, email, quotaBytes, createdAt}`. Nunca se devuelve el
password; se guarda con hash + salt. `quotaBytes` es constante
(`USER_QUOTA_BYTES`, default 1 GiB) y no existe endpoint que la cambie.
El espacio usado se consulta en metadata (`/api/users/{id}/usage`).

Persistencia: `users.dat` (registros binarios de tamaño fijo) + `users.idx`
(árbol B en disco, páginas de tamaño fijo, clave = `username`, valor =
offset del registro). El árbol B es implementación propia.

### 10.3 API de Metadata (`:8082`)

| Método | Ruta | Body → Respuesta |
|--------|------|------------------|
| GET | `/api/videos?page=1&pageSize=20&sort=title\|author\|uploadedAt&order=asc\|desc` | → `200 {items: Video[], total, page, pageSize}`; solo públicos + propios |
| POST | `/api/videos` | `{title, description?, topic?, tags?, visibility, sizeBytes}` → `201 Video` con `status:"pending"`; `409` si `usado + sizeBytes > quota` |
| GET | `/api/videos/{id}` | → `200 Video`; `404` si no existe **o no es visible para quien pregunta** |
| PATCH | `/api/videos/{id}` | `{title?, description?, topic?, tags?, visibility?}` → `200 Video` (solo el autor) |
| DELETE | `/api/videos/{id}` | → `204` (solo el autor); borra también los archivos vía streaming |
| GET | `/api/users/{id}/usage` | → `200 {usedBytes}` |
| PATCH | `/internal/videos/{id}` | (rol `service`) `{status, durationSeconds?, sizeBytes?}` → `200` |
| DELETE | `/internal/users/{id}/videos` | (rol `service`) borra todos los videos del usuario → `204` |

`Video = {id, title, description, topic, tags[], visibility: "public"|"private",
status: "pending"|"processing"|"ready"|"failed", authorId, authorName,
sizeBytes, durationSeconds, uploadedAt}`. `id` es el ObjectId de Mongo
(24 hex). El ordenamiento y la paginación se hacen en Mongo, no en memoria.

### 10.4 API de Streaming (`:8083`)

| Método | Ruta | Descripción |
|--------|------|-------------|
| POST | `/api/videos/{id}/upload` | Body = bytes crudos del video. Solo el autor, con el video en `pending`/`failed`. `413` si excede el `sizeBytes` declarado. → `202` y transcodifica en segundo plano |
| GET | `/api/videos/{id}/dash/manifest.mpd` | Manifiesto DASH |
| GET | `/api/videos/{id}/dash/{archivo}` | Segmentos (`init-*.m4s`, `chunk-*.m4s`) |
| DELETE | `/internal/videos/{id}` | (rol `service`) borra los archivos del video |

El control de acceso se delega: streaming reenvía el token del usuario a
`GET {metadata}/api/videos/{id}`; si metadata responde 200, puede verlo.

### 10.5 Flujo de subida (integra los tres APIs)

1. Webapp → `POST {metadata}/api/videos` con la metadata y el tamaño del
   archivo. Metadata valida la **cuota** (claim `quota` vs. suma de
   `sizeBytes` del usuario) y crea el video en `pending`.
2. Webapp → `POST {streaming}/api/videos/{id}/upload` con el archivo.
3. Streaming guarda el original, marca `processing`, corre FFMPEG y al
   terminar marca `ready` (con `durationSeconds`) o `failed`.
4. Webapp consulta `GET {metadata}/api/videos/{id}` hasta ver `ready` y
   reproduce con Shaka (agregando el header `Authorization` con un
   *request filter*).

## 11. División del trabajo

| Quién | Componentes | Puntos de rúbrica |
|-------|-------------|-------------------|
| **Adrián** | API de usuarios (árbol B, archivo binario, REST, login/JWT) · Aplicación web (React + TS + Shaka) · Pruebas funcionales (Playwright) · Ejecución y análisis de load-tests · Paper (sin AI) | 15 + 10 + 5 + 15 = 45 |
| **Claude (agente)** | `services/common` · API de streaming · API de metadata · Observabilidad (OTel + dashboard) · Docker Compose · Pipelines de CI · Infraestructura de redundancia V2.0 | 15 + 5 + 10 + 5 + 20 = 55 |

Las ≥5 optimizaciones de V2.0 son compartidas: Adrián las identifica y mide
con el dashboard (es materia del paper); se implementan en el componente que
corresponda. Todo PR del agente lo revisa Adrián (regla de la sección 6 y de
la defensa: hay que poder explicar todo).

Candidatos de optimización ya identificados (se dejan a propósito sin
optimizar en V1.0 para medir el antes/después): streaming consulta a metadata
en **cada** segmento (cachear), segmentos sin `Cache-Control`, una sola
transcodificación a la vez, listado sin índices en Mongo, árbol B sin caché
de páginas.
