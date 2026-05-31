# Integración de Karakeep (Hoarder) en CrossPoint Reader

> Estado: **Plan verificado y proxy funcionando** tras exploración del repositorio y prueba real contra API.  
> Fecha: 2025-05-31  
> Hardware objetivo: Xteink X4 (ESP32-C3, 380 KB RAM, sin PSRAM)

---

## 1. Arquitectura: Proxy + Firmware ligero

Tras inspeccionar la API real de Karakeep y las restricciones del ESP32-C3, la arquitectura elegida es un **proxy Python** (`tools/karakeep-proxy/`) que absorbe toda la complejidad, y un **firmware mínimo** que solo descarga binarios listos para leer.

```
CrossPoint (ESP32-C3)  <──HTTP+JSON──>  Karakeep Proxy (Python)  <──HTTP+Bearer──>  Karakeep (Node.js)
   380 KB RAM                              Cualquier servidor                         Tu instancia
```

**¿Por qué un proxy?**
- La API de Karakeep **no tiene filtro por tag**, devuelve HTML crudo, y requiere Bearer auth complejo.
- Un ESP32-C3 con ~50 KB libres tras WiFi+TLS **no puede** parsear HTML, filtrar arrays JSON, ni generar EPUBs ZIP.
- El proxy hace todo esto y expone una API simplificada que el firmware consume con `StreamingJsonParser` + `esp_http_client` básico.

**El proxy ya existe y funciona** (`tools/karakeep-proxy/`). Genera:
- `GET /bookmarks?tag=Sync` → JSON filtrado con metadatos enriquecidos (incluye `isRead`).
- `GET /bookmarks/<id>/content?format=txt` → Texto plano con headers, blockquotes y listas preservados.
- `GET /bookmarks/<id>/content?format=epub` → EPUB2 válido con portada, NCX y CSS (CrossPoint lo lee nativamente con formato enriquecido: negrita, cursiva, títulos).
- `POST /bookmarks/<id>/read` → Marca como leído añadiendo tag `Read` en Karakeep.
- `POST /bookmarks/<id>/unread` → Desmarca leído.
- `POST /bookmarks/<id>/tags` → Añade o quita tags arbitrarios.

---

## 2. Hallazgos clave del repositorio

### 2.1 Formatos que CrossPoint lee nativamente
- **EPUB** → `EpubReaderActivity` con CSS, fuentes, negrita, cursiva, imágenes, TOC. Requiere ZIP válido.
- **TXT / MD** → `TxtReaderActivity`, solo texto plano sin formato enriquecido (un solo font ID, estilo REGULAR).

> El proxy genera **ambos formatos**. El usuario puede elegir `.epub` para lectura enriquecida (recomendado) o `.txt` para máxima velocidad de descarga.

### 2.2 Parser JSON disponible
`lib/JsonParser/StreamingJsonParser.h` es un parser SAX-style de **512 bytes de buffer** y cero allocaciones dinámicas. Es perfecto para la respuesta del proxy (`bookmarks` array plano).

### 2.3 Cliente HTTP con headers custom
`KOReaderSyncClient.cpp` ya demuestra cómo usar `esp_http_client` directamente con headers Bearer, buffers TLS de 2 KB y verificación de heap mínimo (`MIN_HEAP_FOR_TLS = 55000`). El firmware Karakeep seguirá este patrón exacto.

---

## 3. Pasos de implementación

### Paso 0: Desplegar el proxy (ya listo)

```bash
cd tools/karakeep-proxy
pip install -r requirements.txt

export KARAKEEP_URL="http://tu-karakeep:3000"
export KARAKEEP_API_KEY="ak2_..."
# Opcional: export PROXY_PORT=8787
# Opcional: export READ_TAG="Read"
python3 main.py
```

El proxy escucha en `0.0.0.0:8787` por defecto. **Debe ser accesible desde la red WiFi a la que se conecta el Xteink X4**.

---

### Paso 1: Crear `KarakeepCredentialStore`

**Archivos a crear:**
- `lib/Karakeep/KarakeepCredentialStore.h`
- `lib/Karakeep/KarakeepCredentialStore.cpp`

Guarda en `/.crosspoint/karakeep.json`:
- `proxyUrl` → URL del proxy (ej. `http://192.168.10.214:8787`)
- `apiKey` → API key de Karakeep (solo se usa para verificación opcional; el proxy maneja el auth real)

```cpp
class KarakeepCredentialStore {
  static KarakeepCredentialStore instance;
  std::string proxyUrl;
  std::string apiKey;
  KarakeepCredentialStore() = default;
public:
  static KarakeepCredentialStore& getInstance() { return instance; }
  bool hasCredentials() const { return !proxyUrl.empty(); }
  const std::string& getProxyUrl() const { return proxyUrl; }
  const std::string& getApiKey() const { return apiKey; }
  void setCredentials(const std::string& url, const std::string& key);
  bool saveToFile() const;
  bool loadFromFile();
};
#define KARAKEEP_STORE KarakeepCredentialStore::getInstance()
```

---

### Paso 2: Añadir entries en `SettingsList.h`

Añadir después del bloque KOReader Sync (línea ~221):

```cpp
// --- Karakeep (web-only, uses KarakeepCredentialStore) ---
SettingInfo::DynamicString(
    StrId::STR_KARAKEEP_URL, [] { return KARAKEEP_STORE.getProxyUrl(); },
    [](const std::string& v) {
      KARAKEEP_STORE.setCredentials(v, KARAKEEP_STORE.getApiKey());
      KARAKEEP_STORE.saveToFile();
    },
    "karakeepUrl", StrId::STR_KARAKEEP),
SettingInfo::DynamicString(
    StrId::STR_KARAKEEP_API_KEY, [] { return KARAKEEP_STORE.getApiKey(); },
    [](const std::string& v) {
      KARAKEEP_STORE.setCredentials(KARAKEEP_STORE.getProxyUrl(), v);
      KARAKEEP_STORE.saveToFile();
    },
    "karakeepApiKey", StrId::STR_KARAKEEP),
```

---

### Paso 3: Strings i18n

Añadir en `lib/I18n/translations/*.yaml` (comenzar por `english.yaml` y `spanish.yaml`):

```yaml
STR_KARAKEEP: "Karakeep"
STR_KARAKEEP_URL: "Karakeep Proxy URL"
STR_KARAKEEP_API_KEY: "Karakeep API Key"
STR_KARAKEEP_BROWSER: "Karakeep"
STR_KARAKEEP_LOADING: "Loading..."
STR_KARAKEEP_NO_BOOKMARKS: "No bookmarks"
STR_KARAKEEP_DOWNLOADING: "Downloading..."
STR_KARAKEEP_MARK_READ: "Mark read"
STR_KARAKEEP_MARK_UNREAD: "Mark unread"
```

Regenerar:
```bash
python scripts/gen_i18n.py lib/I18n/translations lib/I18n/
```

---

### Paso 4: Crear `KarakeepClient` (firmware)

**Archivos a crear:**
- `lib/Karakeep/KarakeepClient.h`
- `lib/Karakeep/KarakeepClient.cpp`

**API simplificada que habla con el proxy:**

```cpp
struct KarakeepBookmark {
  std::string id;
  std::string title;
  bool isRead;
  std::vector<std::string> tags;
};

class KarakeepClient {
public:
  enum Error { OK = 0, NO_CREDENTIALS, NETWORK_ERROR, SERVER_ERROR, JSON_ERROR, LOW_MEMORY };

  // GET /bookmarks?tag=Sync&limit=20
  static Error fetchBookmarks(const std::string& tagFilter,
                               std::vector<KarakeepBookmark>& out,
                               size_t limit = 20);

  // GET /bookmarks/<id>/content?format=txt|epub
  // Download directly to SD path, with progress callback
  static Error downloadContent(const std::string& bookmarkId,
                                const std::string& format,  // "txt" or "epub"
                                const std::string& destPath,
                                std::function<void(size_t, size_t)> progress = nullptr);

  // POST /bookmarks/<id>/read
  static Error markRead(const std::string& bookmarkId);

  // POST /bookmarks/<id>/unread
  static Error markUnread(const std::string& bookmarkId);

  static const char* errorString(Error error);
};
```

**Implementación:** Patrón `KOReaderSyncClient.cpp` (`lib/KOReaderSync/KOReaderSyncClient.cpp`):
- `esp_http_client` con `buffer_size = 2048`, `buffer_size_tx = 2048`.
- Header `Authorization: Bearer <apiKey>`.
- `MIN_HEAP_FOR_TLS = 55000`.
- Para `downloadContent`: escribir chunks directamente a `HalFile` (mismo patrón que `HttpDownloader.cpp:174-209`).
- Para `fetchBookmarks`: usar `StreamingJsonParser` con callbacks que rellenen `std::vector<KarakeepBookmark>` reservado a `limit` elementos.

---

### Paso 5: Crear `KarakeepBrowserActivity`

**Archivos a crear:**
- `src/activities/browser/KarakeepBrowserActivity.h`
- `src/activities/browser/KarakeepBrowserActivity.cpp`

**Estados:** `CHECK_WIFI → LOADING → BROWSING → DOWNLOADING → ERROR`

**Flujo simplificado (gracias al proxy):**
1. `onEnter()` → `checkAndConnectWifi()` (copiar de `OpdsBookBrowserActivity`).
2. WiFi OK → `KarakeepClient::fetchBookmarks("Sync", bookmarks)`.
3. Mostrar lista con `title` e indicador de leído (`isRead`).
4. **Confirm** sobre un bookmark:
   - `state = DOWNLOADING`.
   - `KarakeepClient::downloadContent(id, "epub", "/karakeep/<title>.epub")`.
   - Al terminar: `activityManager.goToReader(path)`.
   - Opcionalmente: `KarakeepClient::markRead(id)` (configurable en settings).
5. **Long-press Confirm** o menú contextual → opciones: `Mark read/unread`, `Change tags`.
6. `onExit()` → WiFi disconnect + `silentRestart()` (mismo patrón OPDS).

**Directorio de descargas:** `/karakeep/` en SD. Crear con `Storage.mkdir("/karakeep")`.

**Reutilización de código:**
- `StringUtils::sanitizeFilename()` para nombres de fichero.
- `ButtonNavigator` para scroll.
- `GUI.drawButtonHints()`, `GUI.drawProgressBar()`.
- `tr()` para todos los strings.

---

### Paso 6: Integrar en HomeActivity y ActivityManager

**ActivityManager.h:**
- Añadir `KARAKEEP_BROWSER` a `HomeMenuItem` enum.
- Añadir `void goToKarakeepBrowser();`.

**ActivityManager.cpp:**
```cpp
void ActivityManager::goToKarakeepBrowser() {
  replaceActivity(std::make_unique<KarakeepBrowserActivity>(renderer, mappedInput));
}
```

**HomeActivity.h/cpp:**
- Condición: `hasKarakeepCredentials = KARAKEEP_STORE.hasCredentials()`.
- Mostrar entrada de menú si credenciales configuradas.
- `onKarakeepBrowserOpen()` → `activityManager.goToKarakeepBrowser();`.

---

### Paso 7: Testing

**Pre-validación sin hardware:**
```bash
# 1. Verificar proxy
export KARAKEEP_URL=...
export KARAKEEP_API_KEY=...
python3 tools/karakeep-proxy/main.py &

# 2. Probar endpoints
curl "http://localhost:8787/bookmarks?tag=Sync"
curl "http://localhost:8787/bookmarks/<id>/content?format=epub" -o test.epub
file test.epub  # debe decir "EPUB document"
```

**En hardware (Xteink X4):**
1. Compilar: `pio run` (debe dar 0 errores).
2. Flashear y abrir monitor serial: `python3 scripts/debugging_monitor.py`.
3. Configurar proxy URL en `/settings` del web server.
4. Navegar a Karakeep desde menú principal.
5. Verificar heap:
   ```cpp
   LOG_DBG("MEM", "Free: %d", ESP.getFreeHeap());
   ```
   Antes de `fetchBookmarks()`: > 50 KB. Durante descarga: > 25 KB.

---

## 4. Decisiones de diseño documentadas

### ¿Por qué proxy en lugar de hablar directamente con Karakeep?
Karakeep devuelve HTML crudo en assets binarios, no tiene filtro por tag, y requiere Bearer auth con paginación cursor-based. Parsear HTML, filtrar JSON, y generar EPUBs en un ESP32-C3 con ~50 KB libres es **imposible sin abort() por OOM**. El proxy absorbe esta complejidad y expone una API que el firmware puede consumir con `StreamingJsonParser` + `esp_http_client` básico.

### ¿Por qué EPUB desde el proxy?
CrossPoint tiene un lector EPUB nativo sofisticado (`EpubReaderActivity`) con soporte de CSS, múltiples fuentes, negrita, cursiva, imágenes y TOC. Generar EPUB en Python con `zipfile` + `xml.etree` (librería estándar) es trivial y produce ficheros válidos que el lector renderiza perfectamente. El TXT es fallback para usuarios que prefieren velocidad de descarga sobre formato.

### ¿Por qué `StreamingJsonParser`?
Buffer fijo de 512 bytes, cero `malloc` durante parseo. `ArduinoJson` requiere `DynamicJsonDocument` que reserva bloques contiguos de heap. En un ESP32-C3 donde cada KB cuenta, el streaming parser es la única opción segura.

---

## 5. Ficheros afectados (resumen)

| Fichero | Acción |
|---|---|
| `tools/karakeep-proxy/main.py` | ✅ Creado y probado |
| `tools/karakeep-proxy/requirements.txt` | ✅ Creado |
| `tools/karakeep-proxy/README.md` | ✅ Creado |
| `lib/Karakeep/KarakeepCredentialStore.h` | Crear |
| `lib/Karakeep/KarakeepCredentialStore.cpp` | Crear |
| `lib/Karakeep/KarakeepClient.h` | Crear |
| `lib/Karakeep/KarakeepClient.cpp` | Crear |
| `src/SettingsList.h` | Añadir entries `DynamicString` |
| `src/activities/ActivityManager.h` | Añadir `KARAKEEP_BROWSER` y `goToKarakeepBrowser()` |
| `src/activities/ActivityManager.cpp` | Implementar `goToKarakeepBrowser()` |
| `src/activities/home/HomeActivity.h` | Añadir `hasKarakeepCredentials`, `onKarakeepBrowserOpen()` |
| `src/activities/home/HomeActivity.cpp` | Integrar entrada de menú condicional |
| `src/activities/browser/KarakeepBrowserActivity.h` | Crear |
| `src/activities/browser/KarakeepBrowserActivity.cpp` | Crear |
| `lib/I18n/translations/*.yaml` | Añadir strings Karakeep |

---

## 6. Mejoras futuras (fuera del MVP)

- **Formato configurable:** Setting en firmware para elegir `.epub` vs `.txt` por defecto.
- **Marcar como leído automático:** Opción para llamar `markRead()` al salir del lector (detectar cuando `ReaderActivity` termina un libro).
- **Tags contextuales:** Menú en el browser para añadir/quitar tags (usa `POST /bookmarks/<id>/tags`).
- **Búsqueda:** Campo de búsqueda en el browser que envía `?q=` al proxy.
- **Caché offline:** Guardar lista de bookmarks en SD (`/.crosspoint/karakeep_list.bin`) para mostrarla mientras se reconecta WiFi.
- **Portadas en lista:** Descargar miniaturas de screenshot/bannerImage para mostrar en el browser (aunque esto consume RAM para thumbnails).

---

## 7. Referencias de código verificadas

- `tools/karakeep-proxy/main.py` → proxy funcional con endpoints probados.
- `lib/KOReaderSync/KOReaderCredentialStore.h` → patrón de credenciales singleton.
- `src/SettingsList.h:192-221` → patrón `DynamicString` para settings web-only.
- `src/activities/browser/OpdsBookBrowserActivity.h/cpp` → patrón de browser con WiFi, lista scrollable, descarga.
- `lib/KOReaderSync/KOReaderSyncClient.cpp` → patrón de HTTP con `esp_http_client`, headers Bearer y buffers pequeños.
- `lib/JsonParser/StreamingJsonParser.h` → parser JSON de bajo consumo.
- `src/activities/reader/ReaderActivity.cpp:119-125` → soporte nativo de `.txt`/`.md`/`.epub`.
- `src/util/StringUtils.h:12` → utilidad de saneado de nombres de fichero.
