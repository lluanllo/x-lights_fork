# Auditoría técnica — Juajilandia Show Composer (Fase 0)

Inspección de solo lectura del código real. Las rutas son relativas a la raíz del
repositorio. Las referencias `archivo:línea` corresponden al commit indicado abajo y se
comprobaron con un script contra el árbol de trabajo. Lo que no se pudo comprobar está
marcado como **No verificado**.

## 1. Conclusiones que condicionan todo el diseño

1. **Ya existe el patrón a seguir.** `MovingHeadSong::Generate`
   (`src-core/effects/MovingHeadSongGenerator.h:56`) es un generador wx-free que devuelve
   datos planos. La interfaz (`src-ui-wx/sequencer/MovingHeadSongDialog.cpp:390`) los
   convierte en efectos reales con deshacer. El Composer debe ser una versión ampliada de
   esta forma, no otra arquitectura.
2. **No hace falta generar XML a mano.** `SequenceFile::Save` + `EffectLayer::AddEffect`
   cubren la persistencia. El iPad ya crea secuencias sin wx en
   `src-iPad/Bridge/XLSequenceDocument.mm:623`.
3. **El `.xsq` no conserva nodos desconocidos.** `SequenceFile::BuildDocument`
   (`src-core/render/SequenceFile.cpp:1363`) reescribe el documento desde cero con una lista
   fija de nodos. Cualquier metadato propio dentro del `.xsq` se pierde en el siguiente
   guardado. Por eso los datos del Composer van en pistas de tiempos nativas y en un archivo
   adjunto.
4. **Hoy no existe sincronización real de letra con audio en Windows.** La división de frases
   es uniforme (`src-core/lyrics/LyricBreakdown.cpp:47`). El único transcriptor local para
   Windows (OpenVINO/Whisper) no está en tu build de Visual Studio, transcribe en lugar de
   alinear y reparte las palabras en proporción a sus caracteres dentro de cada bloque
   (`src-core/ai/OpenVINOService.cpp:234`).
5. **El diccionario de fonemas no sirve para español.** Es CMU (inglés). `NAVIDAD`, `NIEVE` y
   `CORAZÓN` no tienen entrada, y `AMOR` y `CORAZON` tienen pronunciación inglesa. Una palabra
   sin entrada no genera fonemas ni avisa (`src-core/lyrics/PhonemeDictionary.cpp:168`,
   `src-core/lyrics/LyricBreakdown.cpp:67`): la cara se queda con la boca quieta.
6. **La detección de tempo supone un tempo constante.** `DetectTempo` calcula un único periodo
   global por autocorrelación (`src-core/media/TempoDetector.cpp:85`) y coloca un beat por
   periodo. Esto choca con tu requisito de no asumir tempo constante.
7. **No hay detección automática de secciones ni de estribillos.** `SongStructureManager` solo
   almacena y edita regiones. El análisis es desarrollo nuevo.
8. **No hay metadatos de compatibilidad efecto/modelo.** `resources/effectmetadata/_schema.json`
   no los define. Solo existen validaciones por efecto en
   `RenderableEffect::CheckEffectSettings` (`src-core/effects/RenderableEffect.h:175`). El
   registro de capacidades es desarrollo nuevo.
9. **Casi no hay pruebas automáticas.** `xLights-Test/` tiene dos pruebas (`ip_host_test.cpp`,
   `string_test.cpp`) y no aparece en los workflows de CI. El arnés de pruebas forma parte del
   trabajo.

## 2. Versión y estado del repositorio

| Dato | Valor |
|---|---|
| Directorio del checkout | `xLights/` (la carpeta padre no es un repositorio) |
| Rama al inspeccionar | `master` |
| Commit | `37c29d698` — "Fix the effect assist window #7241 (#7290)", 2026-10-28 |
| `git describe` | `nightly-4-g37c29d698` |
| Versión en `README.txt` | `2026.20 October ??, 2026` (en desarrollo) |
| `master` frente a `origin/master` local | 0 commits de diferencia |
| `origin/master` real en GitHub al auditar | `8fe26c1a6`: **upstream ya ha avanzado**; no se ha hecho `fetch` ni `merge` |
| Versión de tu layout | `xlightsVersion=2026.18` en `xlights_rgbeffects.xml` |
| Licencia | GPLv3 (`License.txt`) |

### 2.1. Cambios locales detectados

Son el trabajo de traducción al español. Antes de esta auditoría estaban sin confirmar.

| Archivo | Cambio |
|---|---|
| `src-ui-wx/xLightsApp.cpp` | Carga `wxTranslations`; idioma por defecto `es` (`:1136`) |
| `src-ui-wx/preferences/OtherSettingsPanel.cpp` / `.h` | Selector de idioma en Preferencias |
| `src-ui-wx/effectpanels/JsonEffectPanel.cpp` | `TranslateMetadata()` para etiquetas y tooltips de los JSON de efectos |
| `CMakeLists.txt`, `xLights/xLights.vcxproj` | Compilan `es.po` a `.mo` después de enlazar |
| `.gitignore`, `README.txt`, `plans/platform-parity/11-preferences-settings.md` | Soporte y documentación |
| `ci_scripts/localization/build_catalog.py` (nuevo) | Extrae, actualiza y compila el catálogo |
| `resources/locale/` (nuevo) | `es.po`, `xLights.pot`, `README.md` |

Defectos en esos cambios (documentados, **no corregidos**):

- `OtherSettingsPanel.cpp` añade `ID_CHOICE_LANGUAGE` dentro de los marcadores `//(* … //*)`,
  pero `src-ui-wx/wxsmith/OtherSettingsPanel.wxs` no se actualizó. Si alguien abre ese panel
  en wxSmith, el selector de idioma desaparecerá (AGENTS.md §4).
- La fila 85 añadida a `11-preferences-settings.md` queda **antes** de la cabecera de la tabla
  y tiene `:??` y estado `?`.
- `xLights.vcxproj` ejecuta `py -3` sin condición, de modo que un equipo sin el lanzador de
  Python no podrá compilar. CMake sí lo comprueba.
- `xLights.cbp` (Linux) y Xcode (macOS) no compilan el catálogo. En esas plataformas la interfaz
  saldrá en inglés aunque el idioma elegido sea español.

### 2.2. Calidad del catálogo español

Medido sobre `resources/locale/es.po`:

| Métrica | Resultado |
|---|---|
| Codificación | UTF-8 correcta, sin caracteres corruptos |
| Entradas | 2.800, ninguna vacía |
| Idénticas al inglés | 134 |
| Marcado como | Borrador de traducción automática (`resources/locale/README.md`) |

Errores reales de traducción automática encontrados:

| Original | Traducción actual | Problema |
|---|---|---|
| `Timing Track` | `Pista de cronometraje` | Término poco natural (aparece 8 veces en total) |
| `Faces` | `caras` | Minúscula en una etiqueta |
| `fixture number` | `número de partido` | Sentido incorrecto |
| `iPad dimmer intensity authoring` | `Autorización de intensidad…` | Sentido incorrecto |
| `\n%d could not be added…` | sin el `\n` inicial | Rompe el formato del mensaje |

Hay además textos sin `_()` que ningún catálogo puede traducir, como
`wxProgressDialog prog("Separating Stems", …)` (`src-ui-wx/sequencer/Waveform.cpp:950`).

### 2.3. Datos reales de la instalación

Analizados `xlights_rgbeffects.xml` y `xlights_networks.xml`, solo para leer.

| Dato | Valor |
|---|---|
| Modelos | 57 |
| Por tipo | Custom 24, Arches 6, Single Line 5, Tree 5, Poly Line 4, Star 4, Window Frame 4, Circle 2, Matrix 2, Sphere 1 |
| Grupos | 245 (unos 99 con nombres de proveedor y unos 78 con nombres propios en español) |
| Submodelos / estados / definiciones de cara | 1.438 / 354 / 64 |
| Modelos con cara | 9 (por ejemplo `Carita arbol`, `Carita verde`, `Mega Tree` con cara matricial `Santa`) |
| Modelos marcados `No Controller` | Varios, incluido `Matrix-2` y los Showstopper: no deben participar por defecto |
| `Mega Tree` | Tree, 2 × 1024 nodos, 16 hebras por cadena |
| `HD Matrix` | Matrix, 980 nodos, 20 hebras por cadena |
| `Tree Mini -1` | Tree, 100 nodos, 10 hebras |
| Efectos DMX | No hay modelos DMX: Moving Head queda fuera del MVP |

Consecuencias para el diseño:

- **El nombre no sirve para clasificar.** `Ventana Matrix 1` es `Window Frame`, no matriz.
  `Window (Front Door)` es `Poly Line`. `Reno` y los `Copo de Nieve` son `Custom`.
- **Los grupos son el destino habitual de los efectos.** La foto del secuenciador muestra
  `Everything-Top`, `TODOS LOS ARCOS` y `Horizontals`. El planificador debe trabajar con grupos,
  no solo con modelos sueltos.
- **Tus secuencias usan muchas pistas de tiempos.** La foto de `Dean Martin - Let It Snow`
  muestra `MH Timings`, `Small Changes`, `Beats`, `Bars`, `Tonic Pitch`, `Main Parts` y una pista
  de palabras de varias capas. Es justo la forma de salida que debe producir el generador.

### 2.4. Lo que no se ha podido analizar

- **Ningún `.xsq` estaba disponible.** Se buscó en Escritorio, Documentos y Descargas y, a nivel
  de carpetas, en `D:`, `E:` y `F:`. La ruta visible en la foto
  (`SHOW NAVIDAD\SHOWS FINALIZADOS\Navidad 2025\`) no existe en este equipo. **Por tanto no hay
  análisis de estilo real**, y todo ejemplo de estilo en estos documentos es ilustrativo.
- **No se ejecutó xLights.** Existe `xLights/x64/Debug/xLights.exe` (28/10/2026), pero sin un
  `.xsq` no hay nada útil que renderizar.
- **Los plugins VAMP no están instalados** en este equipo (`C:\Program Files\Vamp Plugins` no
  existe). Sus nombres concretos y su calidad quedan **No verificados**.

## 3. Arquitectura actual

### 3.1. Capas

| Capa | Rol | Regla |
|---|---|---|
| `src-core/` | Núcleo sin wx, compartido por escritorio, iPad y headless | Prohibido incluir wx o `src-ui-wx` |
| `src-ui-wx/` | Escritorio wxWidgets | Usa el núcleo |
| `src-iPad/` | SwiftUI + puente ObjC++ | Usa el núcleo |
| `xlDo/` | Cliente de automatización | Usa la API HTTP |

La regla la vigila `ci_scripts/check_core_include_boundaries.sh`, que tiene una lista explícita
`CORE_DIRS`. En CI corre en modo aviso (`.github/workflows/include_policy.yml:22`). **Un paquete
nuevo debe añadirse a esa lista o no se comprobará.**

`xLightsShowContext` (`src-core/render/xLightsShowContext.h:49`) es el estado de show compartido.
De él derivan `xLightsFrame`, `iPadRenderContext` y `HeadlessRenderContext`, así que es el punto de
integración natural: reúne `AllModels`, `_sequenceElements`, `effectManager`, `_seqData` y
`_renderEngine`.

### 3.2. Ciclo de vida de una secuencia

| Paso | Símbolo real | Ubicación |
|---|---|---|
| Crear en escritorio | `xLightsFrame::NewSequence` | `src-ui-wx/import_export/SeqFileUtilities.cpp:125` |
| Crear sin wx (referencia) | `newSequenceAtPath:` | `src-iPad/Bridge/XLSequenceDocument.mm:623` |
| Abrir sin escribir | `SequenceFile::Open` → `LoadSequence` | `src-core/render/SequenceFile.h:95`, `SequenceFile.cpp:395` |
| Cargar elementos | `xLightsShowContext::LoadSequenceElements` | `src-core/render/xLightsShowContext.h:145` |
| Migrar ajustes antiguos | `SequenceFile::AdjustEffectSettingsForVersion`, `RenderableEffect::adjustSettings` | `SequenceFile.h:191`, `RenderableEffect.h:188` |
| Construir XML | `SequenceFile::BuildDocument` | `SequenceFile.cpp:1363` |
| Guardar | `SequenceFile::Save` | `SequenceFile.cpp:1582` |

`SequenceFile::Save` llama directamente a `doc.save_file(mFilePath…)` (`SequenceFile.cpp:1589`): **no
es atómico**. Un corte durante el guardado puede corromper el archivo destino. El Composer no debe
guardar nunca directamente sobre un `.xsq` existente.

Ruta mínima de creación, tomada del iPad:

```cpp
SequenceFile sf(path, frameMS);
sf.SetSequenceType("Media");
sf.SetSequenceDurationMS(durationMS);
sf.SetSequenceTiming(std::to_string(frameMS) + " ms");
sf.SetMediaFile(showDir, mediaPath, false);
sf.Save(ctx.GetSequenceElements()); // elementos vacíos tras CloseSequence()
// y después reabrir por la ruta normal
```

### 3.3. Efectos, capas y parámetros

| Concepto | API real | Ubicación |
|---|---|---|
| Añadir modelo a la secuencia | `SequenceElements::AddMissingModelsToSequence` | `src-core/render/SequenceElements.h:127` |
| Añadir elemento | `SequenceElements::AddElement` | `SequenceElements.h:80` |
| Capas | `Element::AddEffectLayer`, `Element::FindOpenLayer` | `src-core/render/Element.h:86` |
| Crear efecto | `EffectLayer::AddEffect` | `src-core/render/EffectLayer.h:38` |
| Comprobar hueco | `EffectLayer::GetRangeIsClearMS` | `EffectLayer.h:89` |
| Bloqueado | `Effect::IsLocked` | `src-core/render/Effect.h:139` |
| Deshacer | `SequenceElements::get_undo_mgr`, `UndoManager::CreateUndoStep`, `CaptureAddedEffect`, `CaptureEffectToBeDeleted` | `SequenceElements.h:218`, uso en `MovingHeadSongDialog.cpp:497` |
| Registro de efectos | `EffectManager`, enum `RGB_EFFECTS_e` | `src-core/effects/EffectManager.h:23` |

Los ajustes y la paleta son cadenas `clave=valor,clave=valor` con prefijos:

| Prefijo | Significado | Ejemplo real |
|---|---|---|
| `E_` | Efecto | `E_FILEPICKERCTRL_Video_Filename`, `E_TEXTCTRL_Text` |
| `T_` | Transición y capa | `T_TEXTCTRL_Fadein`, `T_CHOICE_In_Transition_Type`, `T_CHOICE_LayerMethod` (`src-core/render/PixelBuffer.cpp:1927`, `:1997`, `:1923`) |
| `B_` | Búfer | `B_CHOICE_BufferStyle` (`src-core/render/Effect.cpp:837`) |
| `C_` | Color | `C_BUTTON_Palette1=#FFFFFF,C_CHECKBOX_Palette1=1` (`MovingHeadSongGenerator.cpp:243`) |

`adjustSettings` solo se llama al cargar, no en efectos recién creados (AGENTS.md §7). **El generador
debe emitir los ajustes en el formato actual**, y una prueba debe guardar y reabrir cada plantilla
para detectar deriva.

### 3.4. Pistas de tiempos

- `TimingElement` (`src-core/render/Element.h:154`). Una marca es un `Effect` cuyo nombre es la
  etiqueta. `subType` se persiste (`SequenceFile.cpp:1486`).
- Las pistas de letra usan **3 capas: frases, palabras y fonemas** (`SequenceFile.cpp:1091-1094`).
- La creación está duplicada: `xLightsFrame::AddTimingElement`
  (`src-ui-wx/sequencer/tabSequencer.cpp:3769`) e `iPadRenderContext::AddTimingElement`
  (`src-iPad/Bridge/iPadRenderContext.cpp:2153`).
- **Trampa:** `HeadlessRenderContext::AddTimingElement` devuelve `nullptr`
  (`src-core/render/HeadlessRenderContext.h:66`), y `SequenceFile::ProcessPapagayo` lo usa sin
  comprobarlo (`SequenceFile.cpp:1091-1092`). En headless, importar `.pgo` fallaría. También pide un
  número al usuario mediante `GetUICallbacks()` (`:1048`).
- Hay importadores en `SequenceFile`: `ProcessXTiming`, `ProcessPapagayo`, `ProcessSRT`,
  `ProcessLorTiming`, `ProcessAudacityTimingFiles`, `ProcessLSPTiming`, `ProcessXLightsTiming`,
  `ProcessVixen3Timing`, `ProcessElevenLabsTimingFiles` (`SequenceFile.h:171-187`).
- `SequenceFile::AddNewTimingSection` crea una pista desde vectores de inicios, finales y etiquetas
  (`SequenceFile.h:160`).

### 3.5. Letras y fonemas

| Función | Comportamiento real | Límite |
|---|---|---|
| `BreakdownPhrase` (`src-core/lyrics/LyricBreakdown.cpp:21`) | Divide en palabras a intervalos **iguales** | No es sincronización |
| `BreakdownWord` (`LyricBreakdown.cpp:60`) | Reparte fonemas dentro de la palabra | Sin entrada en el diccionario, no añade nada |
| `PhonemeDictionary::BreakdownWord` (`src-core/lyrics/PhonemeDictionary.cpp:151`) | Busca con `Upper(word)` | `Upper` opera byte a byte; `¿` y `¡` no se eliminan |
| `ParseLRC` (`src-core/lyrics/LRCParser.h:33`) | Lee letras con tiempos | Solo por línea |
| `SanitizePhraseText` (`LRCParser.h:46`) | Limpia comillas y otros caracteres | — |

Conjunto Preston Blair: `AI E FV L MBP O U WQ etc rest` (`src-core/effects/FacesEffect.cpp:657-666`).
Datos en `resources/dictionaries/`: `standard_dictionary`, `extended_dictionary`, `user_dictionary` y
`phoneme_mapping`.

### 3.6. Caras

- Ajustes: `E_CHOICE_Faces_TimingTrack`, `E_CHOICE_Faces_Phoneme`, `E_CHOICE_Faces_FaceDefinition`,
  `E_CHOICE_Faces_Eyes`.
- Falta de pista y fonema: lo informa `FacesEffect::CheckEffectSettings` (`FacesEffect.cpp:262-266`).
- La capa de fonemas es la de índice 2. El fundido de la boca exige exactamente 3 capas
  (`FacesEffect.cpp:496-498`) y el parpadeo automático, al menos 3 (`FacesEffect.cpp:1169`). El
  generador debe crear siempre las 3.
- **"Matrix Faces" no es un efecto.** Es `Faces` con una definición de tipo `Matrix`, que usa imágenes
  (`FacesEffect.cpp:186`, `:215`). Tu instalación usa `NodeRange` y `Matrix`.
- Comprobar caras de un modelo: `Model::GetFaceInfo` (`src-core/models/Model.h:177`).
- Caras a nivel de secuencia: `SequenceElements::GetSequenceFaces`, usado en `FacesEffect.cpp:155`.

### 3.7. Análisis de audio

| Capacidad | API real | Ubicación | Disponible en tu Windows |
|---|---|---|---|
| Cargar audio | `AudioManager(file, interval, device)` | `src-core/media/AudioManager.h:175` | Sí |
| Duración, frecuencia, metadatos | `LengthMS`, `GetRate`, `Title`, `Artist` | `AudioManager.h:217`, `:186`, `:204`, `:207` | Sí |
| Ataques | `DetectOnsets`, `ComputeOnsetEnvelope` | `src-core/media/OnsetDetector.h:31`, `:45` | Sí |
| Tempo y beats | `DetectTempo` (un solo periodo) | `src-core/media/TempoDetector.h:33` | Sí |
| Acordes y tonalidad | `DetectChords` | `src-core/media/ChordDetector.h:37` | Sí |
| Tono | `DetectPitch` | `src-core/media/PitchDetector.h:40` | Sí |
| Espectrograma | `ComputeSpectrogram` | `src-core/media/Spectrogram.h:33` | Sí |
| Energía por fotograma | `GetFrameData`, clase `FrameData` | `AudioManager.h:235`, `:65` | Sí |
| Filtros | `AUDIOSAMPLETYPE`: `BASS`, `TREBLE`, `VOCALS`, `NONVOCALS`, `LUFS`, `STEM_*` | `AudioManager.h:28` | Sí |
| Separación de pistas | `SeparateStems` (ONNX + descarga de modelo) | `src-core/media/StemSeparator.h:53` | Sí, con `HAVE_ORT` en `xLights.vcxproj`; descarga al primer uso |
| Clasificar sonido | `ClassifySound` | `src-core/media/SoundClassifier.h:51` | **No**, solo Apple |
| Plugins VAMP | `xLightsVamp`, `AudioManager::GetVamp` | `src-core/media/xLightsVamp.h:25`, `AudioManager.h:171` | Solo si se instalan; no en iPad |
| Ejecutar plugin | `VAMPPluginDialog::ProcessPlugin` | `src-ui-wx/sequencer/VAMPPluginDialog.cpp` | Atado a wx |
| Palabras desde audio | `aiBase::GenerateLyricTrack` | `src-core/ai/aiBase.h:182` | Ver 3.8 |

Advertencias:

- El reproductor comparte estado: `AudioManager::SwitchTo` cambia también los búferes de reproducción
  (`plans/platform-parity/03-timing-audio.md`, fila 132). **El análisis debe usar su propia instancia
  de `AudioManager`.**
- Frecuencia interna: 48 kHz en Apple y 44,1 kHz en Windows y Linux (doc. 03, tabla de plataformas).
  El análisis no será idéntico entre plataformas.
- `AudioManager` no detecta downbeats, compases, secciones, estribillos ni actividad vocal.

### 3.8. Servicios de IA y Whisper

- `ServiceManager` (`src-core/ai/ServiceManager.cpp`) registra ChatGPT, Claude, Gemini, ollama,
  GenericClient, Apple Intelligence y OpenVINO.
- `OpenVINOService` solo se compila con `HAVE_OPENVINO_GENAI`. La opción de CMake
  `XLIGHTS_USE_OPENVINO_GENAI` viene apagada (`CMakeLists.txt:988`) y `xLights.vcxproj` no la define.
  **Tu build actual de Visual Studio no la incluye.**
- El usuario debe exportar el modelo Whisper con `optimum-cli` (`OpenVINOService.cpp:111`): no es
  apto para un usuario sin conocimientos técnicos.
- Devuelve bloques con tiempos (`:198`) y reparte las palabras por caracteres dentro de cada bloque
  (`:234`). **Eso no es alineación por palabra.** Además transcribe lo que oye, sin tener en cuenta
  la letra que tú aportas.
- `LRCLIBSearchDialog` obtiene letras sincronizadas por línea desde internet. Solo admisible como
  opción explícita del usuario, nunca en la ruta principal.

### 3.9. Renderizado y modo headless

| Elemento | Ubicación |
|---|---|
| `HeadlessRenderContext::LoadShowFolder` / `OpenSequence` / `RenderAndWait` / `WriteFseq` | `src-core/render/HeadlessRenderContext.h:39-52` |
| CLI `--headless`, `--outputdir`, `--fseqcmp`, `--checksequence` | `src-ui-wx/xLightsApp.cpp`, AGENTS.md §4 |
| Motor | `RenderEngine` (`src-core/render/RenderEngine.h`) |
| Determinismo | `RenderBuffer` usa un RNG por búfer; `XL_SERIAL`, `XL_NO_GPU_COMPUTE` (AGENTS.md §4) |

`CheckSequence` vive en wx (`src-ui-wx/xLightsMain.cpp:5413`). En el núcleo solo están el generador de
informes `CheckSequenceReport` (`src-core/diagnostics/CheckSequenceReport.cpp`) y
`RenderableEffect::CheckEffectSettings`, que es lo reutilizable sin interfaz.

### 3.10. Medios

- `SequenceMedia` (`src-core/render/SequenceMedia.h`): caché de imágenes y vídeos, y embebido en el
  `.xsq`.
- `FileUtils::FixFile` resuelve rutas entre máquinas. `MakeRelativePath` e `IsInShowOrMediaFolder`
  mantienen la portabilidad (AGENTS.md §7).
- `VideoEffect::CheckEffectSettings` da **error** si el archivo está vacío o no existe
  (`src-core/effects/VideoEffect.cpp:194-195`), y si el vídeo no cabe en el modelo (`:251`). Esto
  confirma que un marcador pendiente **no puede** ser un efecto Video vacío.
- Claves reales: `E_FILEPICKERCTRL_Video_Filename`, `E_CHOICE_Video_DurationTreatment`,
  `E_CHECKBOX_Video_Loop`, `E_TEXTCTRL_Pictures_Filename`, `E_CHECKBOX_Pictures_ScaleToFit`.

### 3.11. Importación y referencias

- Patrón de carga de otra secuencia (`src-ui-wx/import_export/ImportEffects.cpp:285-297`):

  ```cpp
  SequenceFile xlf(path);
  auto doc = xlf.Open(showDir, /*ignore_audio=*/true, path);
  SequenceElements se(ctx);
  se.SetFrequency(...);
  se.SetViewsManager(viewsManager);        // debe ir antes de LoadSequencerFile
  se.LoadSequencerFile(xlf, *doc, showDir, /*importing=*/true);
  xlf.AdjustEffectSettingsForVersion(se, ctx);
  ```

  Es exactamente lo que necesita el analizador de referencias. **No hay ninguna llamada a `Save`.**
- `ExportEffects` (`src-core/import_export/ExportEffectsReport.h:22`): informe CSV de uso de efectos,
  precedente directo de la extracción de características.
- `AutoMapper`, `EffectMapper`, `MapHintsIO`, `SequencePackage`: mapeo e importación entre layouts.
- `EffectPresetManager` (`src-core/effects/EffectPresetManager.h`): presets. Tu layout no incluye
  ninguno como nodo propio.

### 3.12. Concurrencia

| Mecanismo | Ubicación | Uso recomendado |
|---|---|---|
| `JobPool` | `src-core/utils/JobPool.h:44` | Reservado al render; no lo reutilizaría para el análisis |
| `parallel_for` | `src-core/utils/Parallel.h` | Paralelismo interno de los algoritmos |
| `std::thread` + `atomic<bool>` + `wxProgressDialog` | `src-ui-wx/sequencer/Waveform.cpp:950-959` | **Patrón a imitar** para el proceso largo con cancelación |
| Cancelación por lote | `SeparateStems(... cancel)` | Mismo contrato para el Composer |
| `RenderProgressInfo` | `src-core/render/` | Solo render |

Regla documentada: las operaciones sobre `Effect` que no son `const` son solo para el hilo principal
(`src-core/render/Effect.h:172`).

### 3.13. Interfaz

- Diálogo manual sin wxSmith: `MovingHeadSongDialog`, con `Bind` y `_()`.
- Menú Herramientas: `Menu1` (`src-ui-wx/xLightsMain.cpp:1099`).
- Pestañas: `Notebook1->AddPage(PanelPreview, _("Layout")…)` (`xLightsMain.cpp:936`).
- `SeqSettingsDialog` ya funciona como asistente de creación.

### 3.14. Internacionalización

- `_()` de wxWidgets en `src-ui-wx/` (153 archivos). **Ningún archivo de `src-core/` usa `_()`**, y no
  puede usarlo.
- El extractor (`ci_scripts/localization/build_catalog.py:14`) busca `_("…")` con una expresión que
  también coincide con `N_("…")`. Eso permite marcar cadenas en el núcleo con una macro vacía.
- **Peligro:** `update_catalog` (`build_catalog.py:69`) **elimina** las traducciones de cadenas que ya
  no aparezcan en el código. Conviene confirmar el catálogo en git antes de ejecutar `--update`.

### 3.15. Sistema de compilación

| Archivo | Qué hay que tocar con código nuevo |
|---|---|
| `xLights/xLights.vcxproj` y `.filters` | Lista explícita de cada `.cpp` y `.h` |
| `xLights/xLights.cbp` | Lista explícita |
| `CMakeLists.txt` | Patrón `src-core/composer/*.cpp` dentro de `SRC_CORE` (`:296`) y el equivalente en `SRC_UI` |
| Xcode | Automático |
| `ci_scripts/check_core_include_boundaries.sh` | Añadir `src-core/composer` a `CORE_DIRS` |

### 3.16. Pruebas

| Elemento | Estado |
|---|---|
| `xLights-Test/` | googletest 1.8.1 (NuGet), solo 2 archivos de prueba, enlaza `ip_utils.obj`, **no aparece en CI** |
| CI Windows | `win_CI.yml`, `cmake-windows.yml`: solo compilan |
| Otros | `Ubuntu_CI.yml`, `include_policy.yml` (aviso) |
| Regresión de render | `--headless` + `--fseqcmp`, documentado en AGENTS.md |

AGENTS.md §11 exige compilar macOS y la biblioteca iPad al tocar el núcleo. **Desde este Windows no se
puede.** Ver `RIESGOS_Y_LIMITACIONES.md`.

## 4. Catálogo de efectos

El código tiene 56 efectos (`RGB_EFFECTS_e`, `EffectManager.h:23`). Lo marcado como **hecho** está en el
código; las columnas de uso son **propuesta de diseño** que deberá confirmarse renderizando en tu
instalación.

| Solicitado | ¿Existe? | Restricción real (hecho) | Uso propuesto |
|---|---|---|---|
| Bars | `eff_BARS` | — | Árboles y matrices, acentos |
| Butterfly | `eff_BUTTERFLY` | — | Árboles y matrices, clímax |
| Color Wash | `eff_COLORWASH` | — | Cualquier modelo; base y marcadores |
| **Chase** | **No como efecto** | `SingleStrand` tiene `Number_Chases` y `Chase_Type1`; también `Marquee` | Arcos, líneas y ventanas |
| Fire | `eff_FIRE` | — | Árboles y matrices |
| Galaxy | `eff_GALAXY` | — | Árboles y matrices |
| Morph | `eff_MORPH` | — | Líneas y transiciones |
| Pictures | `eff_PICTURES` | Requiere archivo | Matrices, tras asignar recurso |
| Video | `eff_VIDEO` | Error si falta el archivo; error si no cabe | Solo con recurso asignado |
| Text | `eff_TEXT` | Necesita texto, archivo o pista de letra (`TextEffect.cpp:102-105`) | Matriz y megaárbol |
| Twinkle | `eff_TWINKLE` | — | Reposo, figuras, estrellas |
| Spirals | `eff_SPIRALS` | — | Árboles |
| Snowstorm | `eff_SNOWSTORM` | — | Matrices, árboles, temática de nieve |
| Fireworks | `eff_FIREWORKS` | — | Matriz y clímax |
| Faces | `eff_FACES` | Pista de 3 capas o fonema fijo; definición del modelo | Modelos con cara |
| Moving Head | `eff_MOVINGHEAD` | Solo DMX; `fixture` 1–8 (`MovingHeadSongDialog.cpp:482`) | Sin uso: no tienes DMX |
| **Matrix Faces** | **No como efecto** | `Faces` con definición `Matrix` e imágenes | `Carita verde`, `Mega Tree` |

Otros relevantes: `eff_ON`, `eff_OFF`, `eff_SHIMMER`, `eff_STROBE`, `eff_METEORS`, `eff_CURTAIN`,
`eff_GARLANDS`, `eff_LINES`, `eff_SHOCKWAVE`, `eff_SNOWFLAKES`, `eff_STATE`, `eff_RIPPLE`, `eff_WAVE`,
`eff_TREE`, `eff_VUMETER`, `eff_MUSIC`.

Los ajustes concretos de cada efecto están en `resources/effectmetadata/*.json`, que además dan rangos y
valores por defecto. Esos JSON son la fuente correcta para validar plantillas y para que el generador no
invente parámetros. `State` también es relevante: tu layout tiene 354 definiciones de estado.

## 5. Dependencias

| Dependencia | Estado | Relevancia |
|---|---|---|
| nlohmann/json | Ya en núcleo (27 archivos) | Contratos y perfiles |
| pugixml | Ya en núcleo | Sin uso directo si se reutiliza `SequenceFile` |
| kiss_fft | Vendorizado | Análisis nuevo |
| ONNX Runtime | Opcional, activo | Separación de voces |
| VAMP | Opcional, plugins externos | Mejoras opcionales |
| OpenVINO GenAI | Opcional, apagado | Proveedor de letra opcional |

**No hace falta ninguna dependencia nueva para el MVP.**
