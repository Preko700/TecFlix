#!/bin/bash
# Prueba de integración: levanta metadata-api y streaming-api reales (con MongoDB y ffmpeg
# reales) y recorre el flujo de SPEC.md §10.5: crear → subir → transcodificar → reproducir → borrar.
#
# Uso: infra/smoke-test.sh [dir-de-builds]     (default: ./build)
# Requiere: MongoDB en MONGO_URI (default mongodb://127.0.0.1:27017), ffmpeg, curl, openssl.
set -euo pipefail

BUILD=${1:-build}
MONGO_URI=${MONGO_URI:-mongodb://127.0.0.1:27017}
SECRET=secreto-smoke
META=http://127.0.0.1:18082
STREAM=http://127.0.0.1:18083
WORK=$(mktemp -d)

cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$WORK"; }
PIDS=()
trap cleanup EXIT

b64url() { openssl base64 -A | tr '+/' '-_' | tr -d '='; }
# jwt <sub> — token de usuario con cuota de 50 MB, válido 10 minutos
jwt() {
  local head payload
  head=$(printf '{"alg":"HS256","typ":"JWT"}' | b64url)
  payload=$(printf '{"sub":"%s","username":"%s","role":"user","quota":50000000,"exp":%d}' \
    "$1" "$1" $(($(date +%s) + 600)) | b64url)
  printf '%s.%s.%s' "$head" "$payload" \
    "$(printf '%s.%s' "$head" "$payload" | openssl dgst -sha256 -hmac "$SECRET" -binary | b64url)"
}
# status <esperado> <descripción> <args de curl...> — falla si el código HTTP no coincide
status() {
  local expected=$1 what=$2 got
  shift 2
  got=$(curl -s -o "$WORK/body" -w '%{http_code}' "$@")
  if [ "$got" != "$expected" ]; then
    echo "FALLO: $what → HTTP $got (esperado $expected)"; cat "$WORK/body"; echo; exit 1
  fi
  echo "ok: $what"
}
field() { grep -o "\"$1\":\"[^\"]*\"" "$WORK/body" | head -1 | cut -d'"' -f4; }

TECFLIX_JWT_SECRET=$SECRET PORT=18082 MONGO_URI=$MONGO_URI MONGO_DB=tecflix_smoke \
  STREAMING_URL=$STREAM "$BUILD/metadata/metadata-api" >"$WORK/metadata.log" 2>&1 &
PIDS+=($!)
TECFLIX_JWT_SECRET=$SECRET PORT=18083 METADATA_URL=$META DATA_DIR="$WORK/videos" \
  "$BUILD/streaming/streaming-api" >"$WORK/streaming.log" 2>&1 &
PIDS+=($!)
for url in $META $STREAM; do
  for _ in $(seq 50); do curl -sf "$url/health" >/dev/null && break; sleep 0.1; done
done

ALICE="Authorization: Bearer $(jwt alice-$$)"
BOB="Authorization: Bearer $(jwt bob-$$)"

ffmpeg -nostdin -loglevel error -y -f lavfi -i testsrc=duration=6:size=640x360:rate=24 \
  -f lavfi -i sine=duration=6 -pix_fmt yuv420p "$WORK/video.mp4"
SIZE=$(stat -c %s "$WORK/video.mp4")

status 200 "health de metadata" "$META/health"
status 200 "health de streaming" "$STREAM/health"

status 201 "alice crea un video privado" -X POST "$META/api/videos" -H "$ALICE" \
  -d "{\"title\":\"Smoke\",\"visibility\":\"private\",\"sizeBytes\":$SIZE}"
ID=$(field id)

status 409 "la cuota bloquea un video demasiado grande" -X POST "$META/api/videos" -H "$ALICE" \
  -d '{"title":"Enorme","visibility":"private","sizeBytes":60000000}'
status 404 "bob no puede subir al video privado de alice" \
  -X POST "$STREAM/api/videos/$ID/upload" -H "$BOB" --data-binary "@$WORK/video.mp4"
status 202 "alice sube el archivo" -X POST "$STREAM/api/videos/$ID/upload" -H "$ALICE" \
  -H 'Content-Type: video/mp4' --data-binary "@$WORK/video.mp4"

for _ in $(seq 100); do
  curl -s "$META/api/videos/$ID" -H "$ALICE" >"$WORK/body"
  [ "$(field status)" = processing ] || break
  sleep 0.2
done
[ "$(field status)" = ready ] || { echo "FALLO: el video quedó en $(field status)"; cat "$WORK/streaming.log"; exit 1; }
grep -q '"durationSeconds":6' "$WORK/body" || { echo "FALLO: duración incorrecta"; cat "$WORK/body"; exit 1; }
echo "ok: streaming transcodificó y metadata quedó en ready con duración 6 s"

status 200 "alice descarga el manifiesto DASH" "$STREAM/api/videos/$ID/dash/manifest.mpd" -H "$ALICE"
grep -q 'height="720"' "$WORK/body" && grep -q 'height="360"' "$WORK/body" ||
  { echo "FALLO: el manifiesto no tiene las dos calidades"; exit 1; }
status 200 "alice descarga un segmento" "$STREAM/api/videos/$ID/dash/chunk-0-00001.m4s" -H "$ALICE"
status 206 "el segmento admite peticiones por rango" -H 'Range: bytes=0-99' \
  "$STREAM/api/videos/$ID/dash/chunk-0-00001.m4s" -H "$ALICE"
status 409 "no se puede subir dos veces" -X POST "$STREAM/api/videos/$ID/upload" -H "$ALICE" \
  --data-binary "@$WORK/video.mp4"

status 404 "bob no ve el video privado en metadata" "$META/api/videos/$ID" -H "$BOB"
status 404 "bob no puede reproducir el video privado" "$STREAM/api/videos/$ID/dash/manifest.mpd" -H "$BOB"
status 200 "alice lo hace público" -X PATCH "$META/api/videos/$ID" -H "$ALICE" -d '{"visibility":"public"}'
status 200 "ahora bob sí puede reproducirlo" "$STREAM/api/videos/$ID/dash/manifest.mpd" -H "$BOB"
status 200 "bob lo ve en el listado" "$META/api/videos?sort=title&pageSize=100" -H "$BOB"
grep -q "\"id\":\"$ID\"" "$WORK/body" || { echo "FALLO: el video no aparece en el listado de bob"; exit 1; }

status 204 "alice borra el video" -X DELETE "$META/api/videos/$ID" -H "$ALICE"
status 404 "el manifiesto ya no existe" "$STREAM/api/videos/$ID/dash/manifest.mpd" -H "$ALICE"
[ ! -e "$WORK/videos/$ID" ] || { echo "FALLO: los archivos siguen en disco"; exit 1; }
echo "ok: metadata le pidió a streaming borrar los archivos"

echo "SMOKE TEST OK"
