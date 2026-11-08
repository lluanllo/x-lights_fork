# Fase 13 — Revisión visual y aprendizaje del criterio artístico

Decisiones de arquitectura. Los formatos están en `MODELO_DE_DATOS.md` («Cambios de la Fase 13»).

## 1. Cierre de la Fase 12

- **Pantallas sin controlador.** Una superficie ahora dice dónde puede usarse (`availability`):
  - `layout`: está en el layout, pero no dibuja imágenes.
  - `preview`: dibuja imágenes, pero ningún controlador la mueve.
  - `installation`: dibuja imágenes y está cableada.
- **Elección automática.** Para la instalación real (por defecto) solo toma pantallas cableadas. Para ver en el ordenador (`ScreenSettings.preview`, `--screens-for preview`, casilla en «Pantallas...») puede usar cualquiera que dibuje imágenes.
- **Elección de la persona.** La persona puede seguir eligiendo una pantalla sin cablear; se le avisa de que solo se verá en el ordenador.
- **Qué cuenta como cableada.** Un controlador inactivo cuenta como cableado: lo que importa es que la figura tenga uno asignado. Las pruebas desactivan los controladores a propósito.

## 2. Revisión de una generación (`VisualReview`)

- **Identificadores estables.** Cada plano del storyboard tiene un `decisionId` estable: `<pantalla>/<clave de escena>/<n>`.
  - La clave de escena es `tipo#ocurrencia.parte`, por ejemplo `chorus#2.1`.
  - No depende de los milisegundos. Una corrección (`MediaCorrection.decisionId`) encuentra otra vez su segmento aunque los tiempos se muevan, y el plano se desplaza con él.
- **Qué guarda la revisión de cada plano:**
  - lo que decidió el programa: pantalla, tiempos, archivo, tipo, qué se ve, por qué, confianza, movimiento y entrada/salida;
  - lo que dice la persona: estado (pendiente, aprobado, rechazado, necesita corrección), motivos (no encaja con la música o con la escena, se repite, pantalla inadecuada, movimiento o transición incorrectos), nota, archivo de sustitución y «no usar nunca».
- **Aprobación.** La lista de comprobación tiene 5 pasos: abrí la secuencia en xLights, revisé la composición, las pantallas principales y las transiciones y movimientos, y terminé. `ApproveShow` solo aprueba si se cumplen todas estas condiciones:
  - los cinco pasos están marcados;
  - hay un nombre de revisor;
  - ningún plano sigue rechazado o esperando corrección.

  Una revisión parcial nunca es una aprobación. Un archivo que diga «approved» sin la lista completa se lee como «en curso». Aprobar el espectáculo no aprueba los planos que nadie juzgó.
- **Aplicar correcciones sin rehacer todo** (`ReviewCorrections`). Sin `Avoid` el director dejaría un hueco; con él vuelve a elegir solo en el tramo que deja el plano rechazado.

  | Plano | Corrección |
  |---|---|
  | Aprobado | `Lock` de la persona |
  | Pendiente | `Lock` con origen `review`: se queda como estaba |
  | Sustituido | `Lock` con el nuevo archivo |
  | Rechazado | `Avoid`, solo ese archivo en esa escena y pantalla |

  «Reconstruir el montaje» suelta los `Lock` de origen `review` (`ReleaseReviewHolds`) y conserva los de la persona.
- **Persistencia.**
  - La revisión va en el sidecar (esquema 2), en `review.json` del informe y en las correcciones (`CorrectionSet.review`), para que la versión siguiente la herede por `decisionId`.
  - Una aprobación solo se mantiene para el mismo `specHash`.
  - El registro de la Fase 8 (`.composer-review.json`) se sigue leyendo y se sigue escribiendo al aprobar.

## 3. Etiquetas humanas (`MediaHumanLabels`)

- **Qué se guarda.** `MediaItem.human` guarda, aparte del análisis:
  - confirmadas, rechazadas, añadidas y excluidas;
  - usos a evitar (matriz, árbol, partes tranquilas o intensas, apertura, final);
  - «revisado entero», revisor y fecha.

  El análisis del modelo no se toca nunca.
- **Planificación.** Lee `EffectiveAnalysis`, donde gana la palabra de la persona. Lo hace sobre una copia de la biblioteca.
- **Formato.** `media-library.json` pasa a la versión 3. Un lector anterior lo rechaza en vez de perder las etiquetas al guardar.
- **Evaluación** (`MediaValidationSetFromLibrary`, `EvaluateMediaValidation`):
  - Cuentan solo los archivos revisados enteros. Si no, un concepto no confirmado ni rechazado sería desconocido.
  - Los duplicados forman un grupo y cuentan una vez. No hay entrenamiento, así que no hay conjuntos que separar: el riesgo de fuga es contar dos veces la misma imagen.
  - Se calculan precisión, recall, F1, precisión de los conceptos fuertes y probables, confusiones y resultados por concepto.
  - Con menos de 20 archivos el informe dice que los números solo orientan. Un concepto con menos de 5 apariciones se marca como «pocas muestras».
  - Los pesos de SigLIP no se modifican.

## 4. Preferencias aprendidas (`ArtisticPreferences`)

- **Dónde se guardan.** En `ShowComposer/preferences.json`, versionado. Es del espectáculo y de quien lo hace, no de una secuencia, por eso no va en el sidecar.
- **Señales.**
  - Cada decisión de una revisión da señales que nombran su origen (`secuencia|decisión`).
  - Una decisión cambiada sustituye su señal; una devuelta a pendiente la retira.
  - Volver a aprender la misma revisión no cuenta nada dos veces.
  - Ninguna decisión pesa más de 1.
- **Valor.** `Σ signo·peso·recencia / (2 + Σ peso·recencia)`:
  - siempre está en (−1, 1);
  - empieza tímido;
  - la recencia es `max(0,25; 0,85^sesiones)`: lo reciente pesa más y lo antiguo nunca desaparece del todo.
- **Respaldo.** Un valor aprendido solo actúa con respaldo ≥ 1,75, es decir, dos decisiones completas. **Un rechazo no prohíbe nada.**
- **Ámbitos separados:**
  - global: tipo, oscuridad, movimiento, transiciones, repetición, cantidad, conceptos;
  - por pantalla (presencia, conceptos);
  - por escena (conceptos, movimiento);
  - por perfil (tipo, movimiento);
  - por archivo y pantalla.
- **Exclusión permanente.** Es «No usar nunca este archivo», es decir `MediaItem.disabled`. Es una decisión de la persona, no algo aprendido.
- **Qué hace en el director** (valor `v` entre −1 y 1, sobre una puntuación de 0 a 1):
  - **Puntuación de cada archivo:**
    - vídeo frente a imagen: ±0,06·v;
    - archivo oscuro: 0,08·v;
    - cada concepto (global, en esa escena y en esa pantalla): 0,08·v;
    - el archivo en esa pantalla: 0,15·v;
    - movimiento: 0,05·v.

    Cada plano afectado lo dice con el motivo `preference`.
  - **Objetivo de cada pantalla:** se multiplica por `(1 + 0,3·cantidad)·(1 + 0,3·presencia)`.
  - **Penalización por repetir:** se multiplica por `(1 − 0,5·repetición)`.
  - **Transiciones:** una forma de entrar con v ≤ −0,3 se cambia por un fundido o un corte.
- **Control de la persona.**
  - Fijar un valor propio, desactivar una preferencia, olvidarla, olvidarlo todo y olvidar una revisión concreta.
  - «Usar lo aprendido al componer» desactiva lo aprendido y deja los valores fijados a mano.
  - `--no-learning` genera como si no hubiera nada.
- **Integración.**
  - El aprendizaje actúa en el director visual (storyboard), que es donde se toman las decisiones que se revisan.
  - `CompositionPlanner` y `ChoreographyEngine` no cambian: la revisión es de imágenes y vídeos. No se duplica nada.

## 5. Interfaz

- **Paso 5:**
  - «Revisar las imágenes y vídeos...»: lista por orden o por pantalla, aprobar, rechazar, otro archivo, «no funciona porque...», nota, no usar nunca, lista de comprobación y aprobación.
  - «Lo aprendido...»: frases, valores, respaldo, procedencia, cambiar, activar o desactivar y olvidar.
  - «Marcar como revisado y aprobado...» abre la misma revisión.
- **Biblioteca:**
  - «Qué se ve...»: lo que dijo el programa (las puntuaciones solo en modo avanzado), confirmar, rechazar, nunca, añadir y usos a evitar.
  - «Cómo acierta el programa...»: medido solo con etiquetas humanas.
