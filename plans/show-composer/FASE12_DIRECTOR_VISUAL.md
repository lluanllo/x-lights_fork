# Fase 12 — Director visual avanzado: pantallas, distribución y calidad cinematográfica

Decisiones de arquitectura de la Fase 12 y su estado. Los formatos de datos están en `MODELO_DE_DATOS.md` («Cambios de la Fase 12»).

## 1. Punto de partida (Fase 11)

| Pieza | Fase 11 | Límite que resuelve la Fase 12 |
|---|---|---|
| Carriles | Dos fijos: la matriz principal (sin letra) y el Mega Tree. | Cualquier número de pantallas, cada una con su papel; la matriz con letra puede mostrar imágenes entre frases. |
| Mega Tree | Imágenes derivadas «envueltas»; vídeo recortado a todo el búfer. | Un vídeo que no es panorámico se veía partido en la costura y deformado. |
| Movimiento | Movimiento `vector` nativo de `Pictures`, lineal. | Sin aceleración ni desaceleración; sin zoom real sobre la imagen original. |
| Transiciones | Corte o fundido según la energía. | Sin fundido encadenado, zoom ni cortinilla; sin descanso visual. |
| Continuidad | Cada pantalla elegía sola, con un complemento de colores. | Sin relación entre planos seguidos ni entre pantallas a la vez. |

## 2. Pantallas y papeles (`MediaSurfaces`)

- **Inventario por propiedades, no por nombres.** Una superficie es una figura con papel de matriz, un árbol con búfer de al menos 8×16, una rejilla densa (≥16×16, densidad ≥0,6) o una submatriz de una matriz. De cada una: búfer, luces, densidad, proporción real del layout, orientación, grados, visibilidad (área real frente a la mayor), soporte nativo de `Pictures`/`Video` (de `ModelCapabilities`), controlador, canales compartidos con otras figuras, proporción de la canción con letra y libre.
- **Papeles:** principal, secundaria, árbol, letra, narración, medios, apoyo y sin usar. Primero se aplican los de la persona; después, en los modos que no son libres, una pantalla con letra en al menos el 15 % de la canción queda como `letra`; luego la principal (la mejor puntuada: visibilidad, luces, tiempo libre, horizontal, con controlador, sin canales compartidos, con soporte nativo), el Mega Tree y una secundaria si merece la pena (al menos un cuarto de las luces de la principal y visible).
- **Modos de letra:** `lyrics-first` (la pantalla con letra solo muestra letra), `smart` (imágenes en la entrada, los instrumentales y las pausas entre frases), `free` (todos los papeles los decide la persona). En todos, una imagen nunca tapa una frase: la ventana de imagen excluye cada frase ±600 ms y exige al menos 3 s.
- **Conflictos:** dos figuras que comparten canales nunca muestran imagen a la vez. Una pantalla queda ocupada en una escena si una figura con la que comparte canales está encendida.
- **Avisos:** canales compartidos, pantalla sin controlador con papel de imagen, pantalla de pocas luces o de forma no comprobada elegida por la persona.
- **Persistencia:** `ScreenSettings` en las correcciones y en `media.screens` del sidecar; `screens.json` y `screens-report.txt` en el informe.

## 3. Mega Tree

Investigación (código de xLights): el búfer de un árbol es la sucesión de sus tiras alrededor del tronco; en un árbol de 360° la columna 0 y la última son vecinas (costura). Un árbol de 180° es una sola cara. `B_CUSTOM_SubBuffer` (`PixelBuffer.cpp`, `SubBufferSpec::Parse`) admite subbúferes por porcentajes, y `Video_CropLeft/Right` recorta el vídeo por columnas.

Decisiones:

- **Vídeo en las dos caras (`video-faces`):** dos piezas, una por mitad del árbol, cada una con el vídeo entero recortado a la proporción de una cara. Cada pieza es un efecto `Video` con su subbúfer, en capas consecutivas, con el mismo inicio, fin, archivo y `Video_Starttime`: se leen sincronizadas. Una cara que cruza la costura se parte en dos piezas (tres en total) que se unen exactamente donde se tocan las tiras (`SpreadOnColumns`).
- **Panorama (`video-wrap`):** solo si la proporción es al menos `max(1,9; 1,4 × perímetro/alto)`; entonces da la vuelta completa en una pieza desde el frente.
- **Frente y giro configurables** por árbol (`frontOffset`, `turn`). Las imágenes derivadas se giran (`roll`) para que el frente caiga donde dice la persona; el giro forma parte del nombre del derivado.
- **Árbol de 180°:** una sola cara, nunca piezas ni envoltura.
- **Proporción y orientación:** el perímetro medio usa `topRatio` (el árbol se estrecha arriba) para no deformar la imagen, y el búfer se dibuja con la fila 0 abajo como en xLights, así que la imagen no sale invertida. El recorte vertical conserva el punto de interés; si ese punto está muy abajo, la parte alta de la imagen puede quedar fuera (se ve en las hojas de contacto; no hay una regla que proteja la copa).

## 4. Movimiento y transiciones

- **Clips en movimiento:** el movimiento nativo de `Pictures` es lineal. Para un acercamiento, alejamiento o paneo suave sobre una imagen fija se genera un clip H.264 corto con el `VideoWriter` de xLights (20 fps, curva *smootherstep*, muestreo bilineal desde el original) entre dos vistas (rectángulos de la imagen). No se inventan parámetros de efecto: el clip se reproduce con el efecto `Video` nativo.
- **Medios derivados:** `ShowComposer/media-derived/<hash del original>-<hash del encuadre y la duración>.mp4|png`. Se reutilizan si existen, se regeneran a partir del original y se borran los que nadie ha usado en 60 días (`index.json` guarda la última fecha de uso). No se ha añadido ninguna dependencia.
- **Transiciones** (por plano, `transition`): `cut` al pulso con energía ≥0,65; `zoom` al entrar en el clímax; `crossfade` (dos capas que se solapan) solo con energía <0,6, entre planos relacionados, en la calma o dentro de una misma escena; `wipe` de vez en cuando en un cambio de escena; `fade` en el resto; `rest` (1,5 s sin imagen) antes de devolver la escena a las luces.

## 5. Continuidad narrativa

- **Relaciones entre archivos** (conceptos SigLIP con peso por nivel y categorías): mismo ambiente, mismo motivo, relacionados, complementarios (colores opuestos). Planos seguidos relacionados suman; casi idénticos restan; el puente cambia el aire; el final responde a la apertura.
- **Variedad:** un motivo que la pantalla ya mostró dos veces resta (ajuste tras ver el Mega Tree lleno de notas musicales con contenido real).
- **Pantallas a la vez:** la segunda pantalla de una escena elige lo que complementa a la primera (mismo ambiente o motivo, o contraste).
- **Duración del plano:** según el papel de la escena, la energía, la puntuación, el movimiento del vídeo y el detalle de la imagen; mínimo 3,5 s; no se corta un plano que funciona solo porque pasó un compás.
- **Medidas:** `coherence` (planos seguidos relacionados) y `crossScreen` (pantallas relacionadas a la vez) en `storyboard.json`.

## 6. Validación con contenido real (`--composer-media-eval`)

`MediaValidation` hace un conjunto de validación repartido por carpetas y tipos y calcula precisión, recall, falsos positivos y negativos, precisión de los conceptos fuertes y probables, confusiones, utilidad del punto de interés y calidad de la descripción. **Solo cuentan como medida las etiquetas con `source: "person"`.** Las de cualquier otro (`assistant-provisional`) se informan aparte como provisionales.

Contenido real: 113 archivos de las carpetas de las secuencias comerciales de `PRUEBA` (copiados a `FASE12_PRUEBA/E_REAL`; los originales no se tocan, comprobado por hash). Vocabulario ampliado con cinco conceptos justificados por esa biblioteca: `train`, `portrait` (una cara, sin identificar a nadie), `music-notes`, `holly` y `planet`.

## 7. Interfaz

- Paso 1 y paso 3: «Pantallas...», con cada superficie, su tipo, tamaño, luces, letra, papel y motivo; cambio de papel, modo de letra y frente y giro del árbol; vista previa al momento.
- Paso 3: una fila por pantalla con su papel, franja «todas las pantallas» con cómo entra cada plano y dónde cambian juntas, marcas de imágenes entre frases y «Mantener esta pantalla» (bloquea una pantalla y deja reconstruir el resto).
- Paso 5: pantallas usadas, cobertura de cada una, duración media, transiciones, imágenes entre frases, clips en movimiento, coherencia y avisos.

## 8. Pendiente y límites

- Revisión visual humana de los montajes (`FASE12_PRUEBA`): no se ha aprobado nada visualmente.
- Etiquetado humano del conjunto de validación: hasta entonces no hay métrica real del modelo.
- macOS y `xLights-iPadLib` sin compilar (no hay Mac).
- Con letra continua (Chiquetere), la matriz con letra no tiene pausas de 3 s y no muestra imágenes: es lo correcto según la regla de no tapar letra.
- Una propuesta automática puede no encajar con la canción (en la prueba real, `cher.png` en el Mega Tree de Chiquetere): la persona la cambia en el paso 3.
