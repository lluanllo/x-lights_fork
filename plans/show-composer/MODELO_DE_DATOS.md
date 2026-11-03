# Modelo de datos — Juajilandia Show Composer

Esta es una **propuesta de contratos**. Las secciones 2 (análisis musical), 3 (letras), 4 (modelos) y 6 (composición, como plan abstracto) están implementadas; el resto no existe todavía. Se usa JSON con `nlohmann/json`, que ya está en el núcleo.

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

**Implementado en la Fase 4.** La fuente de verdad son `LyricDocument.h`, `SpanishPhonemizer.h` y `LyricAligner.h` en `src-core/composer/`. Los tres módulos son independientes: el texto no depende de los tiempos, la pronunciación no depende de los tiempos y los tiempos no dependen de cómo se escribió la letra.

### LyricDocument: el texto tal como se escribió

| Tipo | Contenido |
|---|---|
| `LyricDocument` | `format` (`text`, `lrc`, `srt`, `track`), `language` (`es`, `en`, `und`) con su confianza, bloques y avisos |
| `LyricBlock` | `kind` (`unlabeled`, `intro`, `verse`, `pre-chorus`, `chorus`, `bridge`, `instrumental`, `outro`, `other`), la etiqueta escrita y su número |
| `LyricLine` | `id` estable, el texto literal, palabras, voz, `instrumental`, y `startMS` y `endMS` solo si la fuente los traía |
| `LyricWord` | `text` literal y `key` (minúsculas, sin puntuación alrededor) |

- Las etiquetas (`[Estrofa 1]`, `[Estribillo]`, `[Voz: Ana]`) son opcionales. Sin ellas, cada párrafo es un bloque `unlabeled`. Un bloque es estribillo solo si alguien lo etiquetó así.
- **El `id` de una línea** es su clave más el número de apariciones iguales anteriores (`vuelve la navidad#2`). Insertar una estrofa en otro sitio no lo cambia; editar esa línea sí, y entonces sus correcciones se informan como conflicto.
- LRC y SRT se leen con los lectores de xLights (`lrc::ParseLRC`, `srt::ParseSRT`). El texto nunca se modifica: la normalización fonética trabaja sobre una copia.

### SpanishPhonemizer: letras → fonemas → formas de boca

| Etapa | Qué hace |
|---|---|
| Letras | UTF-8 por punto de código. Tres letras iguales o más seguidas son una nota sostenida (`Navidaaad`). Las cifras no se inventan como palabras |
| Grafemas | Tabla de dígrafos con contexto: `ch`, `ll`, `rr`, `qu` y `gu` (estos dos solo ante `e`, `i`). `ü` es su propio grafema |
| Fonemas | Tabla de reglas por grafema y contexto. Ids ASCII: `a e i o u`, deslizantes `j w`, `p b t d k g f T s x tS jj L m n J l r rr` |
| Sílabas | Diptongo, hiato (dos vocales fuertes o `í`/`ú` acentuada), grupos inseparables (`pr`, `bl`…), reparto de consonantes entre vocales |
| Acento | La tilde manda; si no, llana si termina en vocal, `n` o `s`; aguda en otro caso |
| Peso | Peso relativo de cada sílaba, por su vocal, sus consonantes, el acento y la nota sostenida. **Es una estimación, nunca una duración** |
| Visemas | `VisemeOf`: `AI E FV L MBP O U WQ etc`, agrupados como agrupa xLights las vocales inglesas en `phoneme_mapping` |

Opciones de dialecto: `Distinction` (por defecto, /θ/) o `Seseo`, y `yeismo` (por defecto) o distinción de `ll` (/ʎ/). Lo que la ortografía no decide, como la /x/ de «México», está en una tabla de excepciones versionada (`SPANISH_EXCEPTIONS_VERSION`) y ampliable con `AddSpanishException`.

### LyricAligner: cuándo suena cada cosa

| Proveedor | Fuente | Confianza de la frase |
|---|---|---|
| A, `existing-track` | Una pista de xLights ya sincronizada. Sus tiempos y sus fonemas se conservan | 1,0 |
| B, `manual-phrases` | Marcas de frase hechas por una persona (correcciones de nivel frase) | 1,0 |
| C, `line-times` | SRT (inicio y final) o LRC (solo inicios: el final se estima) | 0,7 y 0,4 |
| D, `experimental` | Indicio de voz y sílabas. **Solo si se pide** (`allowExperimental`) | 0,1 |
| Ninguno | Texto sin tiempos: queda `unsynchronized` y no se mueve la boca | — |

Dentro de una frase, las palabras se reparten por peso silábico (confianza 0,5 × la frase) y las formas de boca por sílaba: consonantes breves y la vocal el resto (0,4 × la frase). Formas iguales seguidas dentro de una palabra se unen, nada dura menos de un fotograma y cada pausa es un `rest` explícito.

### Correcciones manuales

`LyricCorrection { level (phrase, word, phoneme), targetId, state, startMS, endMS, shape, originalStartMS, originalEndMS }`. El estado es `corrected`, `locked` o `confirmed`; una corrección nunca es `detected`.

- Se guardan aparte y se aplican después de cada alineación. **Siempre ganan** a lo detectado: las palabras detectadas se reparten alrededor de las fijadas.
- Si su destino ya no existe (la letra cambió), se informa en `conflicts` y la corrección **se conserva**.
- El diálogo «Sincronizar letra» guarda la letra y las correcciones en `<secuencia>.<pista>.composer-lyrics.json` (`LyricSession`). Marcar con el oído da `confirmed`; ajustar ±50 ms, `corrected`; «Confirmar frase», `locked`.

### Pista nativa

`BuildLyricTrack`: capa 0, frases; capa 1, palabras; capa 2, formas de boca con `rest` en las pausas. Es el orden que lee `FacesEffect`. Las frases sin tiempos se dejan fuera en vez de inventarles uno. `WriteTimingTracks` escribe el `.xtiming` de varias capas que importa «Import Timing Track».

**Límite de xLights:** «Import Timing Track» pasa el nombre de la pista por `RemoveUnsafeXmlChars`, que quita los caracteres no ASCII («española» → «espaola»). Las marcas conservan los acentos. `SequenceWriter` y el diálogo escriben el nombre completo.

## 4. Modelos

**Implementado en la Fase 5.** La fuente de verdad es `src-core/composer/ModelCapabilities.h`.

`ExtractCapabilities` lee el layout cargado una sola vez y produce datos planos: un `TargetInfo` por modelo, grupo y submodelo. El planificador ya no vuelve a tocar objetos de xLights.

| Campo | Contenido |
|---|---|
| Identidad | `name`, más `signature` = tipo, nodos y buffer (`Tree:800:16x50`). xLights no tiene un identificador estable, y la firma sobrevive a recablear pero no a cambiar el modelo. Se acompaña de la huella del layout |
| Forma | `displayAs`, buffer, nodos, hebras, canales por nodo, primer y último canal |
| Controlador | `controller` y `noController`. Un modelo «No Controller» **participa**: la parte eléctrica no decide lo visual |
| Relaciones | Caras con algo que dibujar, estados, submodelos, grupos que lo contienen y, en los grupos, sus miembros directos y todos los modelos que alcanzan (grupos anidados resueltos, con protección contra ciclos) |
| Roles | Varios por modelo, con estado (`confirmed`, `suggested`, `unknown`, `excluded-manually`), fuente y confianza |
| Capacidades | `text`, `lyric-track`, `image`, `video`, `face`, `geometric`, `color`, `group-delivery`, `states`: `yes`, `limited`, `no` o `unknown`, cada una con su motivo |

**Prioridad de los roles**

1. Manual (`roles.json`) → `confirmed`.
2. Tipo de modelo (`Matrix` → matriz, `Window Frame` → ventana…) y definición facial → `suggested`, 0,9.
3. Geometría: un `Tree` de 500 nodos o más, o de 40 filas o más, es megaárbol; si no, árbol pequeño → 0,7.
4. El nombre solo es un indicio (0,3) cuando el tipo no dice nada mejor, y **nunca** da matriz ni megaárbol: «Ventana Matrix 1» es un `Window Frame`.

**Capacidades sin inventar**

- Texto, imagen y vídeo son `yes` solo en una matriz con buffer suficiente. En un modelo personalizado grande son `unknown`, porque la legibilidad depende de cómo estén los nodos.
- Si un vídeo concreto se puede decodificar se sabe al elegirlo.
- Un `unknown` nunca se trata como incompatible.

**`ShowComposer/roles.json`**

```json
{ "schemaVersion": 1, "layoutFingerprint": "…",
  "models": [ { "name": "Mega Tree", "signature": "Tree:2048:32x64", "roles": ["mega-tree", "face"], "excluded": false, "note": "" } ] }
```

- Solo se aplica si coinciden el nombre y la firma.
- Si el modelo cambió de forma, se avisa (`role-identity-changed`) y no se aplica.
- Si desaparece, se avisa (`role-model-missing`). Si otro modelo tiene la misma firma, se indica como «posible renombrado», pero el rol **no se traslada solo**.
- `--composer-model-capabilities` escribe una plantilla con todos los modelos y su sugerencia actual.
- Un grupo que encendería un modelo excluido tampoco participa.

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

**Implementado en la Fase 5** como plan abstracto, sin efectos. La fuente de verdad es `src-core/composer/CompositionPlan.h`. El texto que sigue a esta nota es la propuesta original de la fase 0, conservada como referencia.

| Tipo | Contenido |
|---|---|
| `StyleProfile` | «Neutral Starter Profile» (`resources/composer/neutral-starter-profile.json`, `learned: false`): duraciones de segmento, cambios por minuto, densidad por energía, máximo de participantes, preferencias por rol, reglas de grupo y miembro, y los tres niveles |
| `GenerationOptions` | Nivel (`conservative`, `balanced`, `creative`), semilla, fotograma y confianza mínima del indicio de voz |
| `CompositionPlan` | Versiones, hashes del análisis, la letra, el layout y el perfil, semilla, nivel, segmentos, correcciones, conflictos y avisos |
| `PlanSegment` | `id` (`s<sección>.<ordinal>`), tiempos, sección y su repetición, energía, intensidad objetivo, densidad, prioridad, señales (`silence`, `climax`, `user-chorus`, `lyrics`…), estado y motivos |
| `PlanAssignment` | Destino, rol, protagonismo (`lead`, `support`, `accent`, `rest`), intención (`feature`, `accompany`, `beat-accent`, `lyric-text`, `face-sing`, `rest`), intensidad, confianza, estado y motivos |
| `PlanCorrection` | `exclude`, `include`, `set-prominence`, `set-density`, `move-boundary`, `lock`, `accept` y `reject`, con su rango |

Estados de decisión: `automatic`, `manual-edit`, `approved-preference` y `locked`.

**Garantías**

- Las decisiones de una sección dependen solo de esa sección, de su orden de repetición, de las capacidades, del perfil y de la semilla. Cambiar una sección no cambia las demás.
- Lo bloqueado sobrevive a regenerar con otra semilla y otro nivel.
- Una decisión manual incompatible se registra en `conflicts` y no se aplica.


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

### Coreografía (Fase 6)

**Implementado.** La fuente de verdad son `TemplateLibrary.h` y `Choreography.h`.

**Plantilla** (`resources/composer/templates/<id>.json`, `schemaVersion: 1`):

| Campo | Contenido |
|---|---|
| Identidad | `id`, `version`, `effect` (el nombre que usa `EffectManager`) |
| A quién | `roles`, `targetTypes` (`model`, `group`, `submodel`), `requires` (capacidades) |
| Cuándo | `intents`, `prominence`, `sections`, `energy` [mín, máx] |
| Cómo | `intensity`, `durationMS` [mín, máx], `maxPerMinute`, `transitions` (tipos nativos), `palette` {`colors`, `families`}, `rhythm` (`downbeat`), `binding` (`lyrics-phrase`, `face-track`), `media`, `minBuffer` |
| Origen | `weight`, `provenance`, `visuallyValidated` (siempre `false` hasta que una persona lo revise) |

Cada parámetro es `{"id", "value"}`, `{"id", "energy": [bajo, alto]}` o `{"id", "choices": [...]}`. El `id` es la propiedad de `resources/effectmetadata`. La clave se deriva igual que la guarda el panel: `E_SLIDER_`, o `E_TEXTCTRL_` para los decimales con divisor, salvo que `settingPrefix` diga otra cosa, y `E_CHECKBOX_`, `E_CHOICE_`, `E_SPINCTRL_`. Al cargar se comprueban el formato, la versión, que el efecto exista, que existan los parámetros, los tipos, los rangos, las opciones, la coherencia de roles y vínculos, y que existan las familias de paletas. Una plantilla mal hecha se rechaza con su motivo.

**Paletas** (`resources/composer/palettes.json`): `id`, `family` y colores `#RRGGBB`. Son valores de partida.

**`ChoreographyEvent`**: `id` (`<segmento>/<destino>/<bloque>`), destino, tipo, rol, protagonismo, intención, tiempos, capa, efecto, plantilla y versión, ajustes (`E_` y `T_`), paleta y colores, intensidad, transición de entrada y de salida, continuidad (`new`, `vary`, `extend`), compatibilidad, restricciones, motivos, confianza, procedencia, estado y `needsReview`.

**`MediaCue`** sugiere una imagen o un vídeo, sin crear nunca un efecto con una ruta vacía.

**Correcciones**: `set-template`, `set-palette`, `set-timing`, `exclude`, `confirm` y `lock`. Lo bloqueado sobrevive a regenerar. Lo incompatible se registra como conflicto y se conserva la decisión anterior.

**`ToSequenceSpec`** convierte la coreografía en `SequenceSpec` para `SequenceWriter`. En esta fase solo se usa en el fixture de prueba.

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

### Implementado en la Fase 7

La fuente de verdad son `GenerationSidecar.h` y `ShowGenerationPipeline.h`. Difiere de la propuesta anterior en esto:

| Campo | Contenido |
|---|---|
| `schemaVersion` | 1. Uno mayor se rechaza sin leerlo ni reescribirlo (`newer`); uno menor que 1, un JSON roto o un campo obligatorio ausente es `damaged` |
| `generator` | Versiones del pipeline, del análisis, del planificador y de la coreografía |
| `generationId` | Hash de todo lo que decide el resultado: audio, opciones, layout, perfil, plantillas, letra, nivel, semilla, versiones y correcciones |
| `sequenceFile` | El `.xsq` al que pertenece, por nombre |
| `inputs` | `audioHash` (SHA-256 de los bytes), tamaño, clave de opciones, hash del análisis, huella del layout, perfil, plantillas, letra y su calidad, nivel, semilla y `frameMS` |
| `outputs` | Hashes del plan, de la coreografía y de lo escrito (`specHash`, independiente del formato del XML), duración y número de efectos |
| `tracks` | Id estable → nombre de cada pista de tiempos de la secuencia |
| `mediaCues` | Las sugerencias de imagen o vídeo, que también van como pista de tiempos. La figura conserva su efecto: no son marcadores que reserven el tramo a oscuras |
| `corrections` | `plan` y `choreography`, con los mismos serializadores que el plan y la coreografía |
| `lyrics` | Un `LyricSession`: el texto tal como se dio y las marcas manuales, sincronizado o no |
| `validation` | Errores, avisos, si se renderizó y `visualReview: not-reviewed`, que solo cambia una persona |

Al regenerar con `--previous`, se recuperan las correcciones, los bloqueos y la letra del sidecar. Si el sidecar es de otro audio (`incompatible`), dañado o más nuevo, la generación se rechaza con el motivo. La pista de letra de la secuencia anterior manda sobre los tiempos calculados entonces, porque recoge lo que la persona haya movido después.

`summary.json`, `generation-report.txt` y `validation.tsv` van a la carpeta de informes (`--out`), con el análisis, la letra alineada, el plan, la coreografía y el `.fseq`. No van en el show.

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
