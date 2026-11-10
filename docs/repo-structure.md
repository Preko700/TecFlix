# Estructura del repositorio

```
webapp/                  Aplicación web (React + TypeScript + Shaka Player)
services/
  common/                Librería C++ compartida: JWT, helpers HTTP, telemetría
  user-api/              API de usuarios (árbol B en disco)
  metadata-api/          API de metadata (MongoDB)
  streaming-api/         API de streaming (DASH + FFMPEG)
dashboard/               Configuración de observabilidad (OTel, Grafana)
infra/
  docker/                docker-compose y Dockerfiles
  loadtest/              Scripts de pruebas de carga
docs/                    Documentación y paper
.github/workflows/       Un pipeline de CI por componente
```

Cada servicio C++ se compila por separado (Linux / WSL):

```bash
cmake -S services/<servicio> -B build/<servicio> -G Ninja
cmake --build build/<servicio> && ctest --test-dir build/<servicio>
```

Dependencias en Ubuntu: `g++ cmake ninja-build libssl-dev nlohmann-json3-dev
libcpp-httplib-dev libgtest-dev libgmock-dev libmongoc-dev gcovr ffmpeg`.

Contratos entre componentes: sección 10 de [`SPEC.md`](../SPEC.md).
