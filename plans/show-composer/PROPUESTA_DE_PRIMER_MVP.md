# Propuesta de primer MVP — Juajilandia Show Composer

## 1. Qué es

**Una canción, su letra, un árbol, una matriz y una cara: el sistema genera un `.xsq` nuevo que se abre, se edita y se renderiza en xLights, con la boca sincronizada, una animación en el árbol, texto en la matriz y un vídeo pendiente de asignar.**

Demuestra que todos los subsistemas funcionan juntos de principio a fin. **No demuestra todavía que el resultado sea bonito ni que refleje tu estilo.**

## 2. Modelos de prueba

| Papel | Modelo real | Por qué |
|---|---|---|
| Árbol | `Mega Tree` | Tree de 2 × 1024 nodos |
| Matriz | `HD Matrix` | Matrix de 980 nodos, con controlador |
| Cara | `Carita arbol` | Custom con definición `Face` de tipo `NodeRange`, el caso más simple |

Para las pruebas automáticas se usa un fixture sintético equivalente, sin tus datos.

## 3. Entradas

| Entrada | Detalle |
|---|---|
| Audio | Un archivo |
| Letra | Texto plano, una frase por línea; línea en blanco separa bloques; `[Estribillo]` es opcional |
| Tiempos | Opcional: `.lrc`, `.xtiming`, `.srt`, o marcado manual |
| Idioma | `es` o `en` |
| Perfil | Un único perfil de partida, escrito a mano e identificado como tal |
| Semilla | Se genera y se guarda; se puede fijar |

## 4. Los 10 pasos pedidos

| # | Paso | Cómo | Código real implicado |
|---|---|---|---|
| 1 | Cargar audio | Instancia privada | `AudioManager` |
| 2 | Analizar | Duración, BPM, pulsos, ataques, energía, voz, secciones | `DetectOnsets`, `DetectTempo`, `GetFrameData`, filtro `VOCALS`, código nuevo |
| 3 | Incorporar letra | Texto a bloques, frases y palabras | `SanitizePhraseText`, `LyricDocument` nuevo |
| 4 | Sincronizar | Orden: archivo de tiempos, manual, voz y sílabas, uniforme | `ParseLRC`, `SequenceFile::ProcessXTiming`, `LyricAligner` nuevo |
| 5 | Pista de caras | Pista de 3 capas + `Faces` | `SpanishPhonemizer` nuevo, `PhonemeDictionary`, `FacesEffect` |
| 6 | Animación en el árbol | Una plantilla por tipo de sección | `Spirals`, `Bars`, `Color Wash` vía `EffectLayer::AddEffect` |
| 7 | Contenido en la matriz | Frase repetida con `Text`, y reposo en el resto | `TextEffect` |
| 8 | Vídeo pendiente | Pista de marcadores, reserva `Color Wash` y sidecar | `MediaCue` nuevo |
| 9 | Secuencia nativa | Escritura segura | `SequenceFile`, `SequenceElements` |
| 10 | Abrir y renderizar | Asistente mínimo y `--headless` | `xLightsFrame`, `HeadlessRenderContext` |

## 5. Alcance

**Incluido:**

- Opción de desarrollo `--compose`, para pruebas sin ventana.
- Diálogo mínimo en Herramientas, en español: elegir audio, letra, idioma, y generar, con progreso y cancelación.
- Asignación de modelos por roles deducidos, con corrección simple.
- Informe de lo pendiente en un panel.
- Sidecar con versión, semilla y hashes.

**Excluido a propósito:**

| Excluido | Por qué |
|---|---|
| Aprender estilo | Faltan los `.xsq` |
| Asignar vídeo desde la interfaz | Fase 6; en el MVP se hace a mano en el secuenciador |
| Los 9 pasos del asistente | Fase 7 |
| Varias voces | Fase 9 |
| Grupos como destino | Solo 3 modelos: comprobar primero lo más simple |
| Separación de pistas | Evitar la descarga; se medirá sin ella |
| Efectos de física y shaders | Rompen la comparación de renders |

## 6. Archivos

**Nuevos, en `src-core/composer/`:** `ComposerTypes.h`, `ComposerJson`, `SongAnalysis`, `LyricDocument`, `LyricAligner`, `SpanishPhonemizer`, `ModelCapabilities`, `CompositionPlan`, `Choreography`, `MediaCue`, `SequenceWriter`, `GenerationValidator`, `ComposerRunner`.

**Nuevos, otros:** `src-ui-wx/composer/ShowComposerDialog`, `resources/composer/templates/`, `resources/composer/profiles/starter.json`, `xLights-Test/tests/composer_*`, `xLights-Test/fixtures/composer-mini-show/`.

**Modificados:**

| Archivo | Cambio |
|---|---|
| `src-core/lyrics/LyricBreakdown.h/.cpp` | Extraer el reparto de fonemas, sin cambiar el comportamiento |
| `src-ui-wx/xLightsApp.cpp` | `--compose` |
| `src-ui-wx/xLightsMain.cpp`, `.h` | Entrada de menú |
| `CMakeLists.txt`, `xLights/xLights.vcxproj`, `.filters`, `.cbp` | Archivos nuevos |
| `ci_scripts/check_core_include_boundaries.sh` | Añadir `src-core/composer` |
| `resources/locale/es.po` | Cadenas nuevas |
| `README.txt`, `plans/platform-parity/12-ai-automation-scripting.md` | Nota y fila de paridad |
| `.gitignore` | Excluir shows, audio y referencias locales |

## 7. Cómo demostrar que funciona

### Demostración

```powershell
# 1. Generar en una copia de la carpeta del show
xLights.exe --compose -s <copia-del-show> --audio cancion.mp3 --lyrics letra.txt --lang es `
  --models "Mega Tree,HD Matrix,Carita arbol" --out Prueba.xsq --seed 42

# 2. Renderizar sin interfaz
xLights.exe --headless -s <copia-del-show> --outputdir <salida> "<copia-del-show>\Prueba.xsq"

# 3. Reproducibilidad
#    Repetir 1 y 2 con la misma semilla en otra carpeta
xLights.exe --fseqcmp <a.fseq> <b.fseq>
```

La opción `--compose` y sus argumentos son **propuestos**. Las opciones `--headless`, `-s`, `--outputdir` y `--fseqcmp` ya existen.

Después, en la interfaz, en español: Herramientas → Generador de espectáculos, generar, abrir, reproducir con la vista de casa y mirar la boca.

### Criterios de aceptación automáticos

| # | Criterio | Comprobación |
|---|---|---|
| 1 | Existe el `.xsq` nuevo y no sobrescribe | Prueba con el destino ya existente |
| 2 | Se abre sin errores | `HeadlessRenderContext::OpenSequence` |
| 3 | Render correcto | Código de salida 0 |
| 4 | Se renderizan datos reales | `.fseq` con canales distintos de cero en los 3 modelos, en los tramos esperados |
| 5 | Tiene 3 capas | Pista de letra con frases, palabras y fonemas |
| 6 | Español sin huecos | Ninguna palabra sin fonemas; las inglesas sin entrada, en el informe |
| 7 | La cara usa la pista | Efecto `Faces` con `E_CHOICE_Faces_TimingTrack` correcto y definición válida |
| 8 | Sin referencias rotas | Ningún `Video` o `Pictures` con archivo vacío |
| 9 | El marcador existe | Marca en la pista y registro en el sidecar |
| 10 | Validación | `CheckEffectSettings` sin errores en ningún efecto |
| 11 | Ida y vuelta | Reabrir y guardar no pierde efectos ni pistas |
| 12 | Reproducible | Mismo `planHash` y `--fseqcmp` con código 0 |
| 13 | Sin efectos colaterales | Mismo hash en `xlights_rgbeffects.xml` y `xlights_networks.xml` |
| 14 | Cancelable | Cancelar durante el análisis no deja archivos ni altera la secuencia abierta |
| 15 | Capas respetadas | `check_core_include_boundaries.sh` limpio |

### Criterios de aceptación visuales (tuyos)

| # | Criterio |
|---|---|
| 16 | La secuencia se ve y se edita en el secuenciador normal, sin diferencias |
| 17 | La boca se mueve durante la voz y descansa en las pausas |
| 18 | El texto de la matriz se lee |
| 19 | El árbol cambia al cambiar de sección |
| 20 | Toda la interfaz nueva está en español, también los errores |

### Medición que se informará

**Esto se informa, no se exige.** Sirve para decidir el siguiente paso.

| Métrica | Canción |
|---|---|
| Error de inicio de frase: mediana y P90 | `Let It Snow`, comparando con tu pista de palabras existente |
| Error de inicio de palabra: mediana y P90 | La misma |
| Tiempo total de generación | Una canción de 3–4 minutos |
| Tiempo de análisis | La misma |

Si la sincronización automática resulta insuficiente, **el MVP sigue siendo válido** con el marcado manual, y la decisión sobre el resto se tomará con los datos.

## 8. Lo que necesito

1. Tu aprobación de esta propuesta.
2. Un archivo de `SHOW NAVIDAD`: sobre todo `1. Dean Martin - Let It Snow.xsq` con su audio, porque su pista de palabras es la mejor referencia para medir.
3. Una canción en español con su letra.

Sin el punto 2 el desarrollo puede avanzar, pero no se puede medir la sincronización con datos reales.
