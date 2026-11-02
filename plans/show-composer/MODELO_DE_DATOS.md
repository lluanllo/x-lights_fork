# Modelo de datos — Juajilandia Show Composer

Esta es una **propuesta de contratos**. Solo la sección 2, el análisis musical, está implementada; el resto no existe todavía. Se usa JSON con `nlohmann/json`, que ya está en el núcleo.

## 1. Reglas comunes

| Regla | Detalle |
|---|---|
| Versionado | Cada documento raíz lleva `"schemaVersion": N`. Lector: acepta ≤ su versión y migra; si es mayor, avisa y no sobrescribe. |
| Tiempo | Enteros en **milisegundos** dentro de los contratos. Cuantizado a fotograma solo al escribir, con `RoundToMultipleOfPeriod`. |
| Confianza | Real 0–1. Se muestra como Alta (≥0,7), Media (≥0,4), Baja (<0,4). |
| Origen | Todo dato derivado lleva `source`, como `"onset-detector"` o `"user"`. |
| Identificadores | En inglés, estables, sin depender del idioma de la interfaz. |
| Textos visibles | Nunca en los contratos del núcleo: `ComposerMessage { code, args }`. Excepción: lo escrito por el usuario. |
| Orden | Los arrays se guardan ordenados de forma estable, para poder comparar hashes. |
| Errores de lectura | `strtol` y `strtod`; sin `std::stoi` (AGENTS.md §6). |

## 2. Análisis musical

**Implementado en la Fase 3.** La fuente de verdad es `src-core/composer/SongAnalysis.h`. Esta sección resume el contrato y añade lo que el código no explica por sí mismo: cómo se guardan las correcciones y qué podrá hacer con ellas la interfaz futura.

### Tipos

| Tipo | Contenido |
|---|---|
| `MusicalEvent` | `timeMS`, `durationMS`, `type`, `intensity` y `confidence` (0–1), `sectionIndex`, `source`, `beatIndex` (pulsos y compases), `barPosition` (0 en el primer pulso, -1 si el compás no se conoce), `provenance` |
| `MusicalEventType` | `beat`, `downbeat`, `onset`, `section-start`, `section-end`, `climax`, `silence`, `vocal-start`, `vocal-end`, `ending` |
| `SongSection` | `startMS`, `endMS`, `kind`, `repeatGroup` (-1 si no se repite), `energy`, `confidence`, `evidence` (códigos), `provenance` |
| `SectionKind` | `intro`, `verse`, `chorus`, `bridge`, `instrumental`, `outro`, `unknown` |
| `EnergyPoint` | `timeMS`, `rms` (absoluto), `total`, `low`, `mid`, `high`, `variation` e `intensity` (relativos a la canción) |
| `TempoPoint` | Tempo local cada segundo, con su confianza |
| `VocalSegment` | Tramo con voz probable, con `confidence` ≤ 0,3 y `method` |
| `AnalysisOptions` | Intervalo de energía, sensibilidad y mínimo de ataques, rango de BPM, umbral y duración de silencio, sección mínima, compás forzado y estimación de voz |
| `AnalysisResult` | `status` (`Ok`, `Cancelled`, `Failed`), `errorCode` estable (`ANALYZER_*`), `reusedCache`, el análisis y el coste de cada etapa |

Estribillo, estrofa o instrumental **no** son eventos, sino el `kind` de una sección: así una sección tiene un solo significado. Las etiquetas internas son identificadores ingleses estables y la interfaz las traduce.

### Persistencia

`<audio>.composer-analysis.json`, junto al audio o donde indique `--out`. Contiene `schemaVersion`, `analyzerVersion`, `musicQuality`, `audioHash` (SHA-256 de los bytes), `optionsKey`, los datos básicos, los arrays ordenados de forma estable, `warnings` y `corrections`.

- **Reutilización:** solo si coinciden hash, versión del analizador y opciones.
- **Escritura:** primero en `.composer-tmp` y después se renombra. Un análisis cancelado no deja nada.
- **Archivo dañado:** se informa con `ANALYZER_CACHE_UNREADABLE` y no se sustituye, porque puede ser la única copia de las correcciones.
- **Estabilidad:** los mismos bytes con el mismo binario y la misma máquina. No se promete igualdad entre plataformas, porque Apple decodifica con AudioToolbox y el resto con FFmpeg.

### Correcciones manuales

Lo que corrige una persona se guarda **aparte** de lo detectado, en `corrections`. Se vuelve a aplicar después de cada análisis (`ApplyCorrections`), de modo que un análisis nuevo sustituye las detecciones y nunca las correcciones.

| Corrección | Campos | Efecto |
|---|---|---|
| `EventCorrection` | `action` (`add`, `move`, `remove`, `confirm`), `type`, `originalMS`, `timeMS`, `durationMS`, `locked` | Busca el evento detectado a ±50 ms de `originalMS` y lo quita, lo mueve o lo marca |
| `SectionCorrection` | `startMS`, `endMS`, `kind`, `locked` | Recorta las secciones que pisa y coloca la suya. Sirve para mover límites y marcar un estribillo o una parte instrumental |
| `BarCorrection` | `firstDownbeatMS`, `beatsPerBar`, `locked` | Vuelve a contar los compases desde el pulso más cercano a ese instante |

`provenance` distingue los cuatro estados que pide la interfaz:

| Valor | Significado |
|---|---|
| `detected` | Lo encontró el analizador. Un análisis nuevo puede cambiarlo |
| `corrected` | Lo cambió una persona sin bloquearlo |
| `locked` | Lo cambió o lo fijó una persona. Ninguna regeneración lo toca |
| `confirmed` | Una persona dio por bueno lo detectado |

Si el audio cambia, las correcciones se conservan y se añade el aviso `corrections-from-other-audio`, porque pueden haber quedado desplazadas.

### Consulta de un instante

`SnapshotAt(const SongAnalysis&, int timeMS)` devuelve la sección y su tipo, la energía, el pulso y el ataque más cercanos, si es silencio, la voz (`present`, `absent` o `unknown` cuando no existe estimación), una confianza y los eventos que empiezan a menos de 250 ms.

### Pistas nativas

`BuildTimingTracks` y `WriteXTiming` generan un `.xtiming` con las marcas en fotogramas. Cada pista tiene un `id` estable y un nombre visible traducible.

| `id` | Nombre por defecto | Fuente |
|---|---|---|
| `composer.beats` | `Composer Beats` | `beat`, con la posición en el compás como etiqueta |
| `composer.downbeats` | `Composer Downbeats` | `downbeat`, numerados |
| `composer.onsets` | `Composer Onsets` | `onset` |
| `composer.climax` | `Composer Climax` | `climax` |
| `composer.silence` | `Composer Silence` | `silence` |
| `composer.sections` | `Composer Sections` | `sections`, con `kind` y grupo de repetición |

`BuildSongStructure` convierte las secciones en regiones del `SongStructureManager` de xLights. Nada de esto se escribe todavía sobre una secuencia existente.

## 3. Letras

### LyricDocument

Lo que escribió el usuario, estructurado y **sin tiempos**.

```cpp
enum class LyricBlockKind { Verse, Chorus, Bridge, Narration, Instrumental, Unknown };

struct LyricWord   { std::string text; int syllables; };
struct LyricPhrase { std::string text; std::vector<LyricWord> words; };
struct LyricBlock {
    LyricBlockKind kind;
    std::string voice;        // "lead" por defecto
    std::string languageTag;  // "es", "en"
    std::vector<LyricPhrase> phrases;
    bool userConfirmed;       // false si el tipo es solo una suposición
};
struct LyricDocument {
    int schemaVersion = 1;
    std::string title, artist, sourceFile;
    std::vector<LyricBlock> blocks;
};
```

### LyricAlignment

Lo que se cree saber sobre **cuándo** suena. Separado para poder sustituir el proveedor sin tocar el texto.

```cpp
enum class AlignmentSource { ExistingTrack, Manual, WhisperAnchors, VocalActivity, Uniform };

struct TimedWord {
    int blockIndex, phraseIndex, wordIndex;
    int startMS, endMS;
    float confidence;
    AlignmentSource source;
    std::vector<std::string> phonemes;   // Preston Blair
    bool phonemesFromRules;              // true si salen de SpanishPhonemizer
};
struct TimedPhrase {
    int blockIndex, phraseIndex;
    int startMS, endMS;
    float confidence;
    AlignmentSource source;
    bool userEdited;
};
struct LyricAlignment {
    int schemaVersion = 1;
    std::string voice;
    AlignmentSource primarySource;
    float overallConfidence;
    std::vector<TimedPhrase> phrases;
    std::vector<TimedWord> words;
    std::vector<std::string> wordsWithoutPhonemes;
    std::vector<std::string> warnings;
};
```

**Escritura:** pista de 3 capas. Capa 0, frases; capa 1, palabras; capa 2, fonemas. Lo exigen `FacesEffect` y `TextEffect`.

## 4. Modelos

```cpp
enum class ModelRole { Matrix, MegaTree, SmallTree, Window, Arch, Line, Star, Face, Figure, Prop, Group, Excluded };

struct ModelCapabilities {
    std::string name;
    std::string displayAs;          // de DisplayAsType
    std::vector<ModelRole> roles;   // puede tener varios
    std::string roleSource;         // "user" o "derived"
    int bufferWidth, bufferHeight;
    int nodeCount, strandCount;
    std::vector<std::string> faceDefinitions;   // nombres reales
    std::vector<std::string> faceTypes;         // NodeRange, Matrix…
    int stateCount, subModelCount;
    bool hasController;
    bool canShowImage;              // resolución suficiente
    int maxReadableChars;           // calculado, 0 si no sirve para texto
    std::vector<std::string> groupMembers;      // solo para grupos
};
```

`roles.json` guarda **solo las decisiones del usuario**, por nombre de modelo. Lo demás se recalcula, porque el layout puede cambiar.

## 5. Estilo

### ShowReference

```cpp
enum class ReferenceKind {
    ExternalReference,  // referencia externa
    PurchasedDonor,     // comprado, donante de efectos
    OwnModified,        // propio modificado
    ApprovedFinal,      // final aprobado
    Experimental,       // experimental
    Discarded           // descartado
};
struct ShowReference {
    int schemaVersion = 1;
    std::string id;
    std::string path;            // relativa al show si es posible
    std::string fileHash;        // detecta cambios y verifica que no se ha alterado
    ReferenceKind kind;
    std::string licenseNote;     // texto libre del propietario
    std::string season;
};
```

### Peso por tipo

Valores iniciales propuestos, editables en el perfil:

| Tipo | Peso en preferencias | Biblioteca de técnicas |
|---|---|---|
| `ApprovedFinal` | 1,0 | Sí |
| `OwnModified` | 0,6 | Sí |
| `PurchasedDonor` | 0,1 | Sí, solo como parámetros |
| `ExternalReference` | 0,05 | Sí, solo como parámetros |
| `Experimental` | 0,0 | Opcional |
| `Discarded` | 0,0 | No |

### ShowFeatures

Salida de la extracción. Solo estadísticas, **nunca secuencias**.

```cpp
struct EffectUsage {
    std::string effect, role, sectionKind;
    int count, totalMS;
    int p25DurationMS, p50DurationMS, p75DurationMS;
    std::map<std::string, ParamStats> params;  // solo los que varían
};
struct ShowFeatures {
    int schemaVersion = 1;
    std::string referenceId;
    int durationMS;
    std::vector<EffectUsage> effects;
    std::vector<PaletteUsage> palettes;
    std::vector<TransitionUsage> transitions;
    std::vector<ModelPairUsage> coOccurrence;
    float changesPerMinute, layersMean, groupShare;
    float videoSecondsPerMinute, pictureSecondsPerMinute, textSecondsPerMinute;
    std::vector<std::string> unmappedModels;
};
```

### StyleProfile

Son **distribuciones**, no reglas rígidas. El siguiente ejemplo es una **ilustración**: no procede de tus shows.

```json
{
  "schemaVersion": 1,
  "id": "juajilandia",
  "displayName": "Juajilandia",
  "profileVersion": 3,
  "provenance": { "built": "2026-11-01", "references": ["let-it-snow-2025"], "manualEdits": true },
  "roles": {
    "MegaTree": {
      "effects": { "Spirals": 0.4, "Bars": 0.25, "Butterfly": 0.2, "Galaxy": 0.15 },
      "durationMS": { "p25": 2000, "p50": 4000, "p75": 8000 }
    },
    "Matrix": {
      "effects": { "Video": 0.4, "Text": 0.1, "Plasma": 0.2, "Butterfly": 0.3 },
      "videoMinGapMS": 30000
    },
    "Window": { "effects": { "SingleStrand": 0.6, "Color Wash": 0.4 } }
  },
  "sections": {
    "Chorus": { "density": 0.9, "energy": 0.9, "beatSync": 0.8, "changesPerMinute": 30 },
    "Verse":  { "density": 0.5, "energy": 0.5, "beatSync": 0.3, "changesPerMinute": 12 }
  },
  "palettes": [
    { "colors": ["#FF0000", "#FFD27F", "#D4AF37"], "weight": 0.6 }
  ],
  "transitions": { "Fade": 0.7, "Wipe": 0.2, "None": 0.1 },
  "text": { "maxShare": 0.08, "minChars": 3, "keywords": ["navidad"] },
  "exclusions": [
    { "effect": "Strobe", "role": "*", "reason": "user" }
  ]
}
```

### Correcciones aprendidas

```cpp
enum class DecisionOrigin { Automatic, ManualEdit, ApprovedPreference, OneOffChange };

struct LearnedPreference {
    std::string id;
    std::string description;       // código + argumentos
    std::vector<std::string> evidenceSequences;
    int occurrences;
    enum class State { Proposed, Accepted, Rejected } state;
    nlohmann::json profilePatch;   // solo se aplica si se acepta
};
```

## 6. Composición

### GenerationOptions

```cpp
enum class AutomationLevel { Conservative, Balanced, Creative };

struct VoiceAssignment { std::string voice; std::vector<std::string> targets; };

struct GenerationOptions {
    int schemaVersion = 1;
    std::string audioPath, lyricsPath, languageTag;
    std::string styleProfileId;
    AutomationLevel level = AutomationLevel::Balanced;
    uint64_t seed;
    int frameMS = 25;
    std::vector<std::string> includedModels, excludedModels;
    std::vector<VoiceAssignment> voices;
    std::map<std::string, std::string> trackNames; // "beats" → nombre traducido
    std::string outputPath;
    bool useStems = false;                         // implica posible descarga
    int templateLibraryVersion = 1;
};
```

### CompositionPlan

Abstracto: decide **quién y con qué intensidad**, sin parámetros concretos.

```cpp
enum class Prominence { Lead, Support, Accent, Rest };

struct PlanAssignment { std::string target; Prominence prominence; float intensity; };

struct PlanSegment {
    int startMS, endMS;
    int sectionIndex;
    float targetEnergy;
    std::vector<PlanAssignment> assignments;
    std::string paletteId;
    std::string reasonCode;        // por qué
};
struct CompositionPlan {
    int schemaVersion = 1;
    std::string algorithmVersion;
    uint64_t seed;
    std::string analysisHash, profileHash, optionsHash;
    std::vector<PlanSegment> segments;
};
```

### ChoreographyEvent

Concreto: un efecto nativo. Todo lo que `EffectLayer::AddEffect` necesita y por qué existe.

```cpp
struct ChoreographyEvent {
    std::string id;           // estable: "ev-000123"
    std::string target;       // modelo, grupo o submodelo
    int layerHint;            // por papel: 0 fondo, 1 principal, 2 acento
    int startMS, endMS;
    std::string effect;       // nombre nativo, como "Spirals"
    std::string settings;     // "E_...=...,T_...=...,B_...=..."
    std::string palette;      // "C_BUTTON_Palette1=#...,C_CHECKBOX_Palette1=1,..."
    std::string templateId;
    std::string reasonCode;
    std::string relatedEventId;
    float confidence;
    bool isMediaPlaceholder;  // reserva hasta que el recurso se asigne
};
struct Choreography {
    int schemaVersion = 1;
    std::string planHash;
    std::vector<ChoreographyEvent> events;
    std::vector<MediaCue> mediaCues;
};
```

### Plantillas

`resources/composer/templates/*.json`. Las escribe una persona y se validan con los JSON de efectos.

```json
{
  "id": "tree-spirals-fast",
  "effect": "Spirals",
  "roles": ["MegaTree", "SmallTree"],
  "minBuffer": { "w": 8, "h": 16 },
  "energy": [0.6, 1.0],
  "settings": { "E_SLIDER_Spirals_Count": "{{paletteRep}}" },
  "params": { "paletteRep": { "min": 2, "max": 5, "by": "energy" } }
}
```

`Spirals_Count` es un `id` real de `resources/effectmetadata/Spirals.json`, cuya etiqueta visible es "Palette Rep". La clave completa `E_SLIDER_Spirals_Count` ya se usa en `src-core/import_export/LOREdit.cpp:816`. Las claves se validan contra los JSON de efectos en cada ejecución de pruebas.

## 7. Marcadores

```cpp
enum class MediaType { Video, Picture };
enum class CueState { Pending, Assigned, Review, Completed };

struct MediaCue {
    std::string id;                // "C003"; aparece en la etiqueta de la pista
    int startMS, endMS;
    std::string targetModel;
    MediaType expectedType;
    std::string descriptionCode;   // código + argumentos
    std::vector<std::string> descriptionArgs;
    std::string reasonCode;
    std::string relatedEventId;
    std::string relatedText;       // literal de la letra, si existe
    std::vector<std::string> suggestedTags;
    CueState state = CueState::Pending;
    float confidence;
    std::string assignedPath;      // relativa; vacía si está pendiente
    std::string placeholderEventId;
};
```

**Sincronización:** el ID une la marca de la pista y el registro del sidecar. Si faltan datos:

| Caso | Tratamiento |
|---|---|
| Marca sin registro | Se trata como pendiente con datos mínimos |
| Registro sin marca | "Revisar" |
| Tiempo distinto | Manda la pista |

## 8. Informe y sidecar

```cpp
enum class IssueLevel { Info, Warning, Error };

struct ReportItem { IssueLevel level; std::string code; std::vector<std::string> args; int timeMS; std::string target; };

struct GenerationReport {
    int schemaVersion = 1;
    std::vector<MediaCue> pendingVideos, pendingPictures;
    std::vector<ReportItem> textsToReview;
    std::vector<ReportItem> unreliableFaces;
    std::vector<ReportItem> missingResources;
    std::vector<ReportItem> generatedSummary;   // número de efectos, pistas…
    std::vector<ReportItem> validation;
    float lyricConfidence, structureConfidence;
    bool renderChecked;
};
```

### `<secuencia>.composer.json`

```json
{
  "schemaVersion": 1,
  "generator": { "appVersion": "2026.20", "commit": "...", "algorithmVersion": "0.1.0" },
  "inputs": {
    "audioHash": "...", "profileId": "juajilandia", "profileHash": "...",
    "analysisHash": "...", "optionsHash": "...", "seed": 12345
  },
  "planHash": "...",
  "tracks": { "beats": "Pulsos", "lyrics": "Letra", "mediaCues": "Recursos pendientes" },
  "choreography": { },
  "mediaCues": [ ],
  "report": { }
}
```

## 9. Reparto de responsabilidades

| Contrato | Responsabilidad única | Lo produce | Lo consume |
|---|---|---|---|
| `SongAnalysis` | Qué suena y cuándo | `AnalyzeSong` | Alineador, planificador |
| `MusicalEvent` | Un hecho temporal | `AnalyzeSong` | Planificador, pistas |
| `LyricDocument` | El texto y su estructura | `ParseLyrics` | Alineador |
| `LyricAlignment` | Cuándo suena el texto | `AlignLyrics` | Planificador, escritor |
| `ModelCapabilities` | Qué puede hacer cada modelo | `DescribeModels` | Planificador, validador |
| `ShowReference` | Qué es cada show | Usuario | Extracción |
| `StyleProfile` | Qué se prefiere | `BuildProfile`, usuario | Planificador |
| `CompositionPlan` | Quién actúa y con qué intensidad | `PlanComposition` | Coreografía |
| `ChoreographyEvent` | Un efecto concreto y su motivo | `Choreograph` | Escritor, aprendizaje |
| `MediaCue` | Un recurso pendiente | `Choreograph` | Escritor, interfaz |
| `GenerationOptions` | Lo que eligió el usuario | Interfaz | Toda la tubería |
| `GenerationReport` | Lo que queda por hacer | Validador | Interfaz |

## 10. Abrir sin recursos

Un `.xsq` generado **no contiene referencias rotas**, porque un recurso solo se referencia tras asignarlo. Por tanto, se puede abrir aunque falten vídeos. Si se borra un vídeo ya asignado, el comportamiento es el normal de xLights, y `CheckEffectSettings` lo informa como error.
