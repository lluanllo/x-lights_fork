# Fase 11 — Compositor visual: análisis de medios y montaje de Matrix y Mega Tree

Diseño previo a la implementación. Las decisiones que se tomen al implementar se anotan aquí y en `MODELO_DE_DATOS.md` («Cambios de la Fase 11»).

## 1. Auditoría de la Fase 10

| Pieza | Estado | Qué falta para la Fase 11 |
|---|---|---|
| `MediaLibrary` | Busca, mide, hace miniaturas, sugiere categorías por el nombre y guarda `media-library.json` con id por contenido. | No sabe qué hay dentro de un archivo: solo brillo y color medio de la miniatura. Un vídeo se juzga por un fotograma. |
| `MediaDirection` | Elige un archivo por segmento libre, en una figura, con cuota por nivel y explicaciones. | Una figura a la vez, en orden: no planifica Matrix y Mega Tree juntas. Un archivo por segmento: no hay planos, cortes al compás ni fragmentos de vídeo. Encuadre limitado a los tres modos de `Scaling`. |
| Coreografía | Emite `Pictures` y `Video` con la plantilla `matrix-picture` o `matrix-video`. | Solo hay plantillas para el papel `matrix`: el Mega Tree no puede recibir medios. Una asignación por segmento y figura. |
| Interfaz | Biblioteca, lista de medios del paso 3 con elegir, quitar y conservar. | Sin análisis, sin storyboard ni carriles, sin reconstruir ni versiones. |

## 2. Investigación: análisis semántico local

### 2.1. Requisitos

Local, sin conexión, sin enviar fotos fuera, licencia compatible con uso comercial (código y pesos), mantenible y sustituible.

### 2.2. Motor de inferencia

**ONNX Runtime** ya es una dependencia de xLights (licencia MIT): la usa la separación de pistas (`src-core/media/StemSeparator.cpp`), está enlazada en Windows con `HAVE_ORT` (`CMakeLists.txt`, `xLights.vcxproj`, versión `ci_scripts/onnxruntime_version.txt` = 1.24.4), tiene script para Linux (`ci_scripts/fetch_onnxruntime_linux.sh`) y paquetes oficiales para macOS. Los modelos descargables ya tienen sitio: `<show>/ai-models/` (`src-core/media/AIModelStore.h`). No se añade ninguna dependencia nueva.

### 2.3. Modelos considerados

| Modelo | Licencia del código | Licencia de los pesos | Tamaño ONNX (visión, int8) | Decisión |
|---|---|---|---|---|
| **SigLIP base patch16-224** (Google) | Apache-2.0 | Apache-2.0 | 95–99 MB (+111 MB el de texto) | **Elegido** |
| SigLIP 2 base patch16-224 | Apache-2.0 | Apache-2.0 | 95 MB (+283 MB texto, tokenizador de 34 MB) | Mejor multilingüe, pero el texto pesa el triple. Sustituto natural más adelante. |
| CLIP ViT-B/32 (OpenAI) | MIT | MIT | 89 MB | La ficha del modelo declara fuera de alcance «cualquier uso desplegado, comercial o no». Descartado. |
| OpenCLIP (LAION) | MIT | MIT | 89 MB | Entrenado con LAION, con la misma advertencia de despliegue. Descartado por prudencia. |
| MobileCLIP (Apple) | MIT | Apple AMLR (solo investigación) | — | Descartado: no comercial. |
| Jina CLIP v2 | Apache-2.0 | CC-BY-NC-4.0 | — | Descartado: no comercial. |

Comprobado en la API de Hugging Face el 2026-11-08: `google/siglip-base-patch16-224` declara `license: apache-2.0`. Los ONNX cuantizados vienen de `Xenova/siglip-base-patch16-224`, una conversión de esos mismos pesos (la Apache-2.0 permite redistribuir derivados).

### 2.4. Cómo se usa SigLIP

- **Visión** (`vision_model_quantized.onnx`): imagen 224×224, `(x/255 - 0.5)/0.5`, sin recorte; salida `pooler_output` de 768 valores.
- **Texto** (`text_model_quantized.onnx` + `tokenizer.json`): tokenizador Unigram (SentencePiece) con `Metaspace`, minúsculas, `</s>` al final y relleno con `</s>` hasta 64. Se implementa en C++ (Viterbi sobre el vocabulario de `tokenizer.json`) y se valida con los ids de la biblioteca `tokenizers` de Hugging Face.
- **Puntuación**: `sigmoid(exp(4.765) · cos + (-12.932))`. Los dos números se leyeron del `model.safetensors` original por rangos de bytes. SigLIP es calibrado por concepto, no reparte probabilidad entre conceptos.
- **Vocabulario**: `resources/composer/visual-concepts.json`, editable, con conceptos en inglés (lo que entiende el modelo), su grupo (sujeto, escena, ambiente, estilo), las categorías fijas de la Fase 10 que implican, su energía y su idoneidad. Los embeddings de texto se calculan una vez por modelo y versión del vocabulario y se guardan en caché.

Medido en Python sobre los recursos sintéticos de la Fase 10: el primer concepto acierta (pingüino, castillo, fuego, cielo estrellado, luces abstractas), pero con probabilidades bajas, porque son dibujos. Por eso el nivel de confianza combina la probabilidad, la similitud y la distancia al segundo concepto, y nunca se presenta una puntuación como certeza.

### 2.5. Sin modelo

Si no hay modelo descargado, o la compilación no tiene ONNX Runtime (macOS hoy), el análisis **básico** sigue funcionando: colores, brillo, contraste, complejidad, punto de interés, duplicados, análisis temporal de vídeo e idoneidad por figura. Las categorías salen del nombre (Fase 10) y de unas pocas pistas de color marcadas como «heurística, dudosa». El informe dice que no hubo análisis semántico real.

## 3. Análisis por recurso (`MediaAnalysis`)

Se guarda dentro de cada elemento de `media-library.json` (no hay otra biblioteca). Clave de caché: `hash del contenido | versión del analizador | modelo y versión | parámetros`. Si cambia el modelo, solo se repite la parte semántica; si cambia el archivo, todo.

| Campo | Contenido |
|---|---|
| `key`, `analyzer`, `model` | Identidad del análisis para la caché. |
| `colors` | Hasta 5 colores dominantes (k-means determinista en Lab aproximado) con su proporción. |
| `brightness`, `contrast`, `saturation`, `colorfulness`, `complexity` | 0–1. Complejidad = densidad de bordes. |
| `dark`, `white` | Proporción de píxeles casi negros o casi blancos. |
| `orientation` | `panoramic`, `landscape`, `square`, `portrait`, `tall`. |
| `poi` | Punto de interés (x, y en 0–1) por mapa de saliencia (contraste de color frente al fondo y bordes), y `subject` (`center`, `left`, `right`, `top`, `bottom`). |
| `dhash` | Hash perceptivo de 64 bits; por fotograma clave en vídeos. |
| `concepts` | `{id, score, level}`; nivel `strong`/`probable`/`weak`. |
| `description` | Frase corta formada con los conceptos más fuertes (ids, la interfaz los traduce). |
| `categories` | Categorías fijas que se deducen de los conceptos (se suman a las del nombre; las de una persona mandan). |
| `fit` | Idoneidad para `matrix` y `megaTree` (0–1) y sus motivos. |
| `issues` | `very-dark`, `very-bright`, `low-resolution`, `extreme-aspect`, `duplicate-of:<id>`, `similar-to:<id>`, `too-short`, `static-video`, `dark-spans`, `damaged`. Nada se borra. |
| `video` | Muestras, `scenes` (inicio, fin, energía, brillo, color, hash, conceptos), `motion`, `cuts`, `darkSpans`, `loop` (continuidad final→principio), `energy`, usos `short`/`long`. |

**Vídeo.** Se decodifica a resolución reducida (96×54 como máximo) con el `VideoReader` de xLights, con muestreo de 4 fotogramas por segundo y como mucho 240 muestras, tiempo límite y cancelación. Los cortes son saltos del histograma de color; el movimiento es la diferencia media entre muestras; un tramo oscuro es una sucesión de muestras con brillo bajo.

## 4. Encuadre (`MediaFraming`)

Investigado en `src-core/effects/PicturesEffect.cpp` y `VideoEffect.cpp`:

- `Pictures`, movimiento `vector`: desplaza el centro de `PicturesXC/YC` a `PicturesEndXC/EndYC` (porcentaje del búfer) y escala de `Pictures_StartScale` a `Pictures_EndScale` (porcentaje del tamaño **de la imagen**) a lo largo del efecto, solo con `Scaling = No Scaling`. Es un movimiento de cámara nativo, pero la escala se mide en píxeles de la imagen: con una foto de 4000 px sobre 50 px, el 1 % es un salto enorme.
- `Pictures_WrapX` hace que lo que sale por un lado entre por el otro: con un búfer de árbol de 360°, un desplazamiento de un ancho completo es una vuelta al árbol.
- `Video`: `Video_CropLeft/Right/Top/Bottom` recortan la región del fotograma (porcentajes; `Top` mide desde abajo), `Video_Starttime` empieza en un instante del vídeo (la metadata fija 20 s, pero declara que el máximo es la duración del vídeo; se valida contra la duración real), y `Video_DurationTreatment` repite o no.

Por eso un encuadre que no sea «encajar» o «recortar centrado» usa un **derivado**: la imagen reescalada al tamaño que cubre el búfer de la figura (con margen para moverse), en `ShowComposer/media-derived/<hash>-<parámetros>.png`. Así la escala del 100 % es «cubre el búfer» y los porcentajes significan algo. El derivado se identifica por el hash del original y los parámetros, se reutiliza y se puede volver a crear; el original no se toca.

| Estrategia | Cómo |
|---|---|
| `fit` | `Scale Keep Aspect Ratio` (sin recorte, con bandas). |
| `fill` | `Scale Keep Aspect Ratio Crop`, centrado. |
| `subject` | Derivado que cubre, `No Scaling` y `PicturesXC/YC` con el punto de interés en el centro. |
| `pan-x` / `pan-y` | Derivado que cubre, `vector` desde el lado opuesto hasta el punto de interés, como mucho un búfer cada cinco segundos. |
| `zoom-in` / `zoom-out` | Derivado, `vector` con escala 100→118 (o al revés) y centro hacia el punto de interés. |
| `tree-faces` | Árbol de 360°: derivado del ancho del búfer con la imagen (recortada a la proporción física de media vuelta) repetida en las dos caras, para que se vea desde cualquier lado. |
| `tree-wrap` | Árbol de 360° y panorámica: la panorámica da la vuelta completa al árbol, con `WrapX` y un giro lento (`vector` de 0 a -100 en X). |
| `video-crop` | Vídeo: recorte nativo centrado en el punto de interés, con la proporción de la figura. |

Proporción física de un megaárbol: media vuelta visible tiene aproximadamente el diámetro de la base de ancho y la altura del árbol de alto. Sin dimensiones físicas fiables en el modelo, se toma diámetro ≈ 0,6 × altura (árboles de 360° habituales) y se documenta como supuesto.

## 5. Storyboard (`VisualMediaStoryboard`)

Integrado en `MediaDirection`: el storyboard **produce** el `MediaPlan` (las `MediaAssignment` que ya consume la coreografía), y añade lo que faltaba.

```
VisualMediaStoryboard
  version, seed, level, themeId, analysisMode ("semantic" | "basic")
  lanes[]          { target, role ("matrix" | "mega-tree"), bufferWi, bufferHt, degrees }
  scenes[]         { id, startMS, endMS, sectionKind, band, role ("establish", "verse", "lift", "chorus",
                     "instrumental", "bridge", "climax", "ending"), lookKey, repeatOf, locked }
  shots[]          { id, sceneId, lane, startMS, endMS, mediaId, path, kind, effect,
                     fragment { startMS, lengthMS } (vídeos), framing { strategy, poi, derived },
                     transitionIn/Out { type ("cut" | "Fade" | "Dissolve"), ms },
                     reasons[], level ("very-suitable" | "acceptable" | "doubtful"), confidence,
                     state (automatic | manual-edit | locked) }
  gaps[]           { lane, startMS, endMS, reason }
  stats            { coverage por carril, imágenes, vídeos, repeticiones, derivados, huecos, faltan }
  history          los storyboards anteriores (hasta 5) en el sidecar, para recuperarlos
```

### 5.1. Planificación

1. **Escenas.** Las de la dirección visual de la Fase 9 (una por sección musical), con su banda de energía y su `lookKey`; la repetición musical comparte `lookKey`.
2. **Huecos por carril.** En cada escena, el tiempo en que la figura está libre: la letra en la Matrix y una cara cantando mandan; un grupo encendido que la contiene también.
3. **Candidatos.** Por carril y escena, cada recurso utilizable con su puntuación: tema (conceptos y categorías frente a las del tema y la letra), idoneidad para la figura (proporción, resolución, complejidad), energía (vídeo con movimiento en lo alto, imagen tranquila en lo bajo), papel de la escena (presentar el tema en la introducción, contenido sobrio en los versos, lo más reconocible en el estribillo, movimiento en el instrumental, cambio de ambiente en el puente, lo más impactante en el clímax, salida tranquila en el final), calidad (penaliza oscuro, quemado, pequeño, estático) y variedad.
4. **Conjunta.** Para cada escena se valoran a la vez las parejas (Matrix, Mega Tree) entre los mejores candidatos de cada carril, incluido «nada» en uno de ellos: suma de idoneidades más complementariedad (tema parecido, archivo distinto, composición distinta, colores que no chocan) menos repeticiones. No se rellena un carril con lo que sobra del otro.
5. **Orden.** Primero las escenas con más peso (clímax, estribillos, instrumental) para que se queden con lo mejor; después el resto. Una repetición musical recupera el recurso de su primera aparición si sigue siendo el mejor o si está bloqueado.
6. **Planos.** Una escena larga se divide en planos que cortan en compases (downbeats); en energía alta el corte es seco y al compás, en energía baja es un fundido. Un vídeo con varias escenas aporta fragmentos distintos (`Video_Starttime`), elegidos por energía y brillo del fragmento.
7. **Cobertura.** Objetivo por nivel sobre el tiempo libre de cada carril (Matrix: 40 %, 70 %, 90 %; Mega Tree: 25 %, 45 %, 65 %), nunca a costa de la calidad: un recurso por debajo del umbral deja el hueco a las luces.
8. **Repeticiones.** Un recurso no se repite en escenas consecutivas si hay alternativa; no se usa en dos escenas distintas salvo repetición musical o falta de alternativas, y se informa.
9. **Correcciones.** Las de la Fase 10 (`assign`, `lock`, `remove` por segmento y figura) más `lock-scene` (la escena entera tal como está), `move` (a otra figura) y la recuperación de una versión anterior, que se convierte en bloqueos.

Todo es determinista: misma biblioteca (hashes y análisis), canción, dirección, nivel, semilla y correcciones dan el mismo storyboard (`storyboardHash`). La generación nunca ejecuta el modelo: usa el análisis guardado.

## 6. Integración

- **Plantillas nuevas**: `tree-picture` y `tree-video` (papel `mega-tree`, intención `media`).
- **Coreografía**: varias asignaciones por segmento y figura (los planos), transiciones del storyboard en lugar de las de la plantilla, y derivados como archivo del efecto.
- **Escritor y validador**: aceptan los planos y comprueban que cada archivo existe; los derivados tienen que existir antes de escribir.
- **Sidecar**: `media.storyboard` (resumen, hash y correcciones nuevas) y `media.history`.
- **Informe**: `storyboard.json` y `storyboard-report.txt`; `media-plan.json` sigue existiendo.

## 7. Interfaz

- **Paso 1**: «Analizar biblioteca» con progreso real, cancelación y resumen (archivos, imágenes, vídeos, válidos, no admitidos, dañados, analizados, de caché, modo).
- **Biblioteca**: columnas de descripción, Matrix, Mega Tree y estado; selección múltiple para poner o quitar categorías.
- **Paso 3**: storyboard con dos carriles en una línea de tiempo con miniaturas; reconstruir, bloquear escena, bloquear recurso, quitar, sustituir, cambiar de figura y recuperar una versión.
- **Paso 5**: estadísticas del montaje.

## 8. Fuera de alcance

Generar imágenes o vídeos con IA, descargar medios, editor de vídeo, editor de animación, varias canciones, cabezas móviles, voces y transcripción.

## 9. Decisiones tomadas al implementar

- **Derivado solo cuando hace falta.** Un `fill` o un `fit` en una figura de luces cuadradas usa el archivo original con `Scaling` nativo; el derivado se reserva para el movimiento de cámara, el encuadre en el sujeto, los píxeles no cuadrados, el árbol y las imágenes de más de 4096 px. Así un archivo de fuera del espectáculo sigue pudiendo copiarse con permiso (Fase 10) y el efecto lee el archivo de la persona.
- **El árbol como pantalla.** Su capacidad `Image` sigue siendo «desconocida» para el resto del planificador (cambiarla habría cambiado los espectáculos sin medios); el storyboard acepta un megaárbol con búfer de al menos 8 x 16.
- **`Video_Starttime`.** La metadata lo limita a 20 con divisor 100 (0-0,2 s); su propia descripción dice que el máximo es la duración del vídeo. `CheckSetting`, el validador y el escritor solo exigen que no sea negativo; el planificador lo comprueba contra la duración leída del archivo.
- **Orden de reparto.** Primero el clímax y los estribillos, luego presentación y final (con una reserva del 15 % del objetivo), instrumental, puente y versos; una sección que repite a otra elige después de las nuevas. La continuidad de una repetición no se aplica si la escena original está justo antes.
- **Carriles.** La matriz con más luces y más tiempo libre de letra y caras: en el layout real, `Matrix-2`, porque `HD Matrix` muestra la letra toda la canción; el motivo queda en el informe.

