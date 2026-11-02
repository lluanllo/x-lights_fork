# Matriz de reutilización — Juajilandia Show Composer

Leyenda: ✅ se reutiliza tal cual · 🟡 se amplía · 🆕 desarrollo nuevo. Las ubicaciones se comprobaron contra el commit
`37c29d698`.

## Audio y música

| Funcionalidad | Existente | Ubicación real | Reutilizar | Ampliar | Desde cero | |
|---|---|---|---|---|---|---|
| Cargar y decodificar | `AudioManager` | `src-core/media/AudioManager.h:175` | Instancia privada | — | — | ✅ |
| Duración, título, artista | `LengthMS`, `Title`, `Artist` | `AudioManager.h:217`, `:204`, `:207` | Todo | — | — | ✅ |
| Ataques | `DetectOnsets` | `src-core/media/OnsetDetector.h:31` | Todo | — | — | ✅ |
| Envolvente de ataques | `ComputeOnsetEnvelope` | `OnsetDetector.h:45` | Base de los beats | — | — | ✅ |
| BPM global | `DetectTempo` | `src-core/media/TempoDetector.h:33` | Valor inicial | — | — | ✅ |
| Beats con tempo variable | Parcial: un periodo fijo | `src-core/media/TempoDetector.cpp:85` | — | — | Seguimiento por programación dinámica | 🆕 |
| Downbeats y compases | No existe sin VAMP | — | — | — | Sí | 🆕 |
| Acordes y tonalidad | `DetectChords` | `src-core/media/ChordDetector.h:37` | Todo | — | — | ✅ |
| Tono | `DetectPitch` | `src-core/media/PitchDetector.h:40` | Opcional | — | — | ✅ |
| Espectrograma | `ComputeSpectrogram` | `src-core/media/Spectrogram.h:33` | Base de secciones | — | — | ✅ |
| Energía por banda | `GetFrameData`, `FrameData` | `AudioManager.h:235`, `:65` | Todo | — | — | ✅ |
| Filtros | `AUDIOSAMPLETYPE`, `GetFilteredAudioData` | `AudioManager.h:28`, `:245` | Todo | — | — | ✅ |
| Pistas separadas | `SeparateStems` | `src-core/media/StemSeparator.h:53` | Opcional, con aviso de descarga | — | — | ✅ |
| Modelos descargables | `AIModelStore` | `src-core/media/AIModelStore.h` | Todo | — | — | ✅ |
| Plugins VAMP | `xLightsVamp` | `src-core/media/xLightsVamp.h:25` | Opcional | Sacar la ejecución de `VAMPPluginDialog` | — | 🟡 |
| Actividad vocal | No existe | — | Filtros `VOCALS`, `STEM_VOCALS` | — | Detector | 🆕 |
| Secciones | No existe | — | Espectrograma, envolvente | — | Algoritmo | 🆕 |
| Estribillos | No existe | — | Letra y autosimilitud | — | Algoritmo | 🆕 |
| Representación temporal unificada | No existe | — | — | — | `SongAnalysis`, `MusicalEvent` | 🆕 |

## Letras y caras

| Funcionalidad | Existente | Ubicación real | Reutilizar | Ampliar | Desde cero | |
|---|---|---|---|---|---|---|
| Pista de 3 capas | Formato nativo | `src-core/render/SequenceFile.cpp:1091` | Formato | — | — | ✅ |
| Importar `.xtiming` | `ProcessXTiming` | `src-core/render/SequenceFile.h:173` | Todo | — | — | ✅ |
| Importar `.pgo` | `ProcessPapagayo` | `SequenceFile.cpp:995` | Parser | Evitar el diálogo y el `nullptr` en headless | — | 🟡 |
| Importar `.srt` | `ProcessSRT` | `SequenceFile.h:176` | Todo | — | — | ✅ |
| Leer `.lrc` | `ParseLRC` | `src-core/lyrics/LRCParser.h:33` | Todo | — | — | ✅ |
| Limpiar texto | `SanitizePhraseText` | `LRCParser.h:46` | Todo | Añadir `¿` y `¡` en la normalización previa | — | 🟡 |
| Frase → palabras | `BreakdownPhrase` | `src-core/lyrics/LyricBreakdown.cpp:21` | Solo como último recurso | — | Reparto por sílabas | 🟡 |
| Palabra → fonemas | `BreakdownWord` | `LyricBreakdown.cpp:60` | Reparto de tiempos | Extraer función con fonemas ya resueltos | — | 🟡 |
| Diccionario inglés | `PhonemeDictionary` | `src-core/lyrics/PhonemeDictionary.cpp:67` | Para `en` | — | — | ✅ |
| Fonemas en español | No existe (CMU) | — | — | — | `SpanishPhonemizer` | 🆕 |
| Palabras desde audio | `aiBase::GenerateLyricTrack` | `src-core/ai/aiBase.h:182` | Como anclas opcionales | — | Alineación con la letra | 🟡 |
| Whisper local | `OpenVINOService` | `src-core/ai/OpenVINOService.cpp:180` | Opcional | Compilar con la opción de CMake | — | 🟡 |
| Letras en línea | `LRCLIBSearchDialog` | `src-ui-wx/sequencer/LRCLIBSearchDialog.cpp` | Solo opción explícita | — | — | ✅ |
| Alineación de letra propia | No existe | — | — | — | `LyricAligner` | 🆕 |
| Marcado manual de frases | Teclas `t` y `s` | `src-ui-wx/sequencer/MainSequencer.cpp` | Concepto | — | Modo guiado | 🆕 |
| Efecto de caras | `FacesEffect` | `src-core/effects/FacesEffect.cpp` | Todo | — | — | ✅ |
| Definiciones de cara | `Model::GetFaceInfo` | `src-core/models/Model.h:177` | Todo | — | — | ✅ |

## Secuencias

| Funcionalidad | Existente | Ubicación real | Reutilizar | Ampliar | Desde cero | |
|---|---|---|---|---|---|---|
| Crear secuencia | `SequenceFile` + patrón iPad | `src-iPad/Bridge/XLSequenceDocument.mm:623` | Patrón | — | — | ✅ |
| Guardar | `SequenceFile::Save` | `src-core/render/SequenceFile.cpp:1582` | Todo | Escritura segura por encima | — | 🟡 |
| Abrir | `SequenceFile::Open`, `LoadSequenceElements` | `SequenceFile.h:95`, `xLightsShowContext.h:145` | Todo | — | — | ✅ |
| Añadir modelos | `AddMissingModelsToSequence` | `src-core/render/SequenceElements.h:127` | Todo | — | — | ✅ |
| Crear efecto | `EffectLayer::AddEffect` | `src-core/render/EffectLayer.h:38` | Todo | — | — | ✅ |
| Capas | `AddEffectLayer`, `FindOpenLayer` | `src-core/render/Element.h:86`, `:87` | Todo | — | — | ✅ |
| Crear pista | Duplicado en wx e iPad; `nullptr` en headless | `tabSequencer.cpp:3769`, `iPadRenderContext.cpp:2153`, `HeadlessRenderContext.h:66` | Cuerpo | Función auxiliar en el núcleo | — | 🟡 |
| Pista desde vectores | `AddNewTimingSection` | `SequenceFile.h:160` | Todo | — | — | ✅ |
| Regiones de canción | `SongStructureManager` | `src-core/render/SongStructureManager.h` | Todo | — | — | ✅ |
| Deshacer | `UndoManager` | `src-core/render/UndoManager.h` | Al asignar recursos | — | — | ✅ |
| Generador de precedente | `MovingHeadSong` | `src-core/effects/MovingHeadSongGenerator.h:56` | Patrón | — | — | ✅ |
| Plan de composición | No existe | — | — | — | `CompositionPlan`, `Choreography` | 🆕 |
| Plantillas de efectos | Presets, no aptos como plantillas | `src-core/effects/EffectPresetManager.h` | Formato de ajustes | — | `TemplateLibrary` | 🆕 |
| Metadatos de parámetros | JSON de efectos | `resources/effectmetadata/*.json` | Rangos y valores | — | — | ✅ |

## Modelos

| Funcionalidad | Existente | Ubicación real | Reutilizar | Ampliar | Desde cero | |
|---|---|---|---|---|---|---|
| Tipos | `DisplayAsType` | `src-core/models/DisplayAsType.h:19` | Todo | — | — | ✅ |
| Dimensiones y nodos | `GetDefaultBufferWi`, `GetDefaultBufferHt`, `GetNodeCount`, `GetNumStrands` | `Model.h:257`, `:258`, `:471`, `:598` | Todo | — | — | ✅ |
| Submodelos | `GetSubModel`, `GetNumSubModels` | `Model.h:426`, `:428` | Todo | — | — | ✅ |
| Estados | `GetStateInfo` | `Model.h:179` | Todo | — | — | ✅ |
| DMX | `IsDMXModel` | `Model.h:259` | Excluir | — | — | ✅ |
| Validación por efecto | `CheckEffectSettings` | `src-core/effects/RenderableEffect.h:175` | Todo | — | — | ✅ |
| Roles | No existe | — | — | — | `ModelRole` | 🆕 |
| Compatibilidad efecto/rol | No existe | — | — | — | Tabla en plantillas | 🆕 |

## Referencias y estilo

| Funcionalidad | Existente | Ubicación real | Reutilizar | Ampliar | Desde cero | |
|---|---|---|---|---|---|---|
| Cargar otra secuencia | Patrón de importación | `src-ui-wx/import_export/ImportEffects.cpp:285` | Patrón, trasladado al núcleo | — | — | ✅ |
| Informe de uso de efectos | `ExportEffects` | `src-core/import_export/ExportEffectsReport.h:22` | Lógica de recorrido | — | — | 🟡 |
| Mapeo entre layouts | `AutoMapper`, `EffectMapper` | `src-core/import_export/` | Para técnicas de referencia | — | — | ✅ |
| Archivos de referencia | `GetFileReferences` | `src-core/render/Element.h:71` | Inventario de recursos | — | — | ✅ |
| Características de estilo | No existe | — | — | — | `ShowFeatures` | 🆕 |
| Perfil de estilo | No existe | — | — | — | `StyleProfile` | 🆕 |
| Aprendizaje | No existe | — | — | — | Comparar plan y `.xsq` | 🆕 |

## Medios

| Funcionalidad | Existente | Ubicación real | Reutilizar | Ampliar | Desde cero | |
|---|---|---|---|---|---|---|
| Caché y embebido | `SequenceMedia` | `src-core/render/SequenceMedia.h` | Todo | — | — | ✅ |
| Rutas portables | `FixFile`, `MakeRelativePath` | `src-core/utils/FileUtils.h`, AGENTS.md §7 | Todo | — | — | ✅ |
| Efecto Video | `VideoEffect` | `src-core/effects/VideoEffect.cpp` | Todo | — | — | ✅ |
| Efecto Pictures | `PicturesEffect` | `src-core/effects/PicturesEffect.cpp` | Todo | — | — | ✅ |
| Compatibilidad de vídeo | `MediaCompatibility` | `src-core/media/MediaCompatibility.h` | Al asignar | — | — | ✅ |
| Marcadores | No existe | — | Pista de tiempos | — | `MediaCue` y sidecar | 🆕 |

## Render, validación, interfaz

| Funcionalidad | Existente | Ubicación real | Reutilizar | Ampliar | Desde cero | |
|---|---|---|---|---|---|---|
| Render sin ventana | `HeadlessRenderContext` | `src-core/render/HeadlessRenderContext.h:32` | Todo | — | — | ✅ |
| Comparar `.fseq` | `--fseqcmp` | `src-ui-wx/xLightsApp.cpp` | Todo | — | — | ✅ |
| Render en escritorio | `RenderAll` | `src-ui-wx/` | Todo | — | — | ✅ |
| Informe HTML | `CheckSequenceReport` | `src-core/diagnostics/CheckSequenceReport.cpp` | Estilo, opcional | — | — | 🟡 |
| Validar composición | Parcial | — | `CheckEffectSettings` | — | `GenerationValidator` | 🆕 |
| Trabajo en segundo plano | Patrón `std::thread` | `src-ui-wx/sequencer/Waveform.cpp:959` | Patrón | — | — | ✅ |
| Paralelismo | `parallel_for` | `src-core/utils/Parallel.h` | Todo | — | — | ✅ |
| Diálogo manual | `MovingHeadSongDialog` | `src-ui-wx/sequencer/MovingHeadSongDialog.cpp` | Patrón | — | — | ✅ |
| Traducción | `_()`, `TranslateMetadata`, `build_catalog.py` | `src-ui-wx/effectpanels/JsonEffectPanel.cpp`, `ci_scripts/localization/build_catalog.py` | Todo | Proteger `--update` | — | 🟡 |
| Pruebas | `xLights-Test` | `xLights-Test/` | Proyecto | Añadir pruebas del Composer y CI | — | 🟡 |
| Comprobación de capas | `check_core_include_boundaries.sh` | `ci_scripts/` | Todo | Añadir `src-core/composer` | — | 🟡 |

## Resumen

**Se escribe desde cero:** la representación temporal, los análisis de secciones y voz, el alineador, el fonetizador español, los
roles, las características y el perfil de estilo, el planificador, las plantillas, los marcadores y la validación de composición.

**No se escribe nada de:** decodificación de audio, FFT, ataques, acordes, serialización `.xsq`, efectos, render, modelos, medios,
deshacer ni traducción.
