# Fase 16 — Comprensión multimodal de audio, voces y figuras

Decisiones de diseño de la fase. Los resultados medidos y las pruebas reales están en el informe
`JUAILILANDIA_REFERENCE_PACK/REPORTS/FASE16_COMPRENSION_MULTIMODAL.md`, que no se publica.

## Qué entiende el compositor ahora

El compositor ya no solo oye la música (fase 4). Ahora también sabe:
- quién habla, canta o responde como coro, y cuándo;
- cuándo suenan solo los instrumentos;
- cómo se mueve el sonido: golpes, caídas, colas, silencios y crecidas;
- qué figuras tiene la instalación y qué partes reales tiene cada una;
- quién debe interpretar cada voz;
- qué hizo de verdad el render frente a lo que se quería.

Todo es núcleo sin wx (`src-core/composer`). La ventana solo lo muestra y deja corregirlo.

## Audio (`AudioUnderstanding`)

- **Decodifica xLights.** El audio llega por `AudioManager`, el mismo decodificador que usa el
  programa. La lectura de las muestras en bruto pide antes la copia RAW
  (`EnsureFilteredAudioData`); sin ella, `GetRawLeftDataPtr` espera para siempre.
- **Rasgos propios, sin bibliotecas nuevas:**
  - FFT radix-2 de 2048 puntos cada 20 ms, en el centro y en los lados;
  - 48 bandas logarítmicas para el flujo espectral y la separación armónico/percusiva por medianas;
  - parte de la energía entre 300 Hz y 3,4 kHz, medida sobre lo que hay por encima de 150 Hz;
  - parte por debajo de 90 Hz y planitud espectral;
  - YIN sobre la señal filtrada a 150–1800 Hz, para que el bombo y el bajo no tapen el periodo
    de la voz;
  - polifonía por criba de armónicos del tono hallado. Un pico solo cuenta si está cinco veces
    por encima de la mediana, así el ruido no inventa notas.
- **Umbrales adaptativos.** El suelo de silencio es la mediana menos 40 dB, con un mínimo
  configurable. Un ataque exige que el sonido suba: un timbre que tiembla no es un golpe. Las
  duraciones mínimas son: silencio 400 ms, pausa 350 ms y frase de voz 300 ms.
- **Habla o canto.**
  - Sin música, por notas sostenidas: una altura mantenida 160 ms o más, con vibrato incluido.
    El habla se desliza de sílaba en sílaba por plana que sea; el canto sostiene notas.
  - Sobre música, la altura de la voz no se puede seguir desde la mezcla. En canciones reales no
    aparecen notas en frases cantadas, y una narración sobre acordes toma prestada su
    estabilidad. Por eso se supone canto y se marca como dudoso.
  - Lo deciden de verdad una grabación separada, una etiqueta [Narrador] en la letra o una
    corrección de la persona, que vuelve cierto el tramo.
- **Coro.** Varias alturas a la vez y anchura estéreo. Un coro es una sola pista; nunca se
  inventan cantantes.
- **A cappella** es una voz que canta sin acompañamiento. Una voz que habla sola no es a cappella.
- **Sucesos.** Cada suceso es un tramo con tipo, confianza, procedencia e id estable, y lleva
  su propia corrección (`on` = `event`). Los tipos son: ataques y acentos (el 20 % más fuerte),
  sostenidos, decaimientos y colas (≥ 9 dB), crecidas y bajadas en 2,5 s (≥ 6 dB), caídas y
  reentradas (≥ 10 dB, situadas en el paso más brusco) y la resolución final.
- **Grabaciones separadas.** Cada una se lee por su cuenta. Las de voz, coro o narración
  sustituyen a las voces adivinadas en la mezcla y quedan en su propia pista (`stem:<nombre>`).

## Voces (`VocalUnderstanding`)

- **Diarización experimental y nunca separación.** Las voces se distinguen por la altura
  mediana, su dispersión, el brillo y el soplo, y se llaman «Voz A», «Voz B»…
  - Entre frases cantadas la altura pesa la mitad, porque una melodía sube y baja.
  - Un grupo de menos de 2 s se une a la voz más parecida, y los grupos que acaban sonando igual
    se juntan.
  - No se identifica a nadie.
- **La letra manda en los nombres.** Una voz que la letra nombra (con su reparto) toma el nombre
  y el papel. Una línea sincronizada donde no se oyó voz se conserva como intervención de la
  letra. El texto vive solo en memoria: `voices.json` guarda cuántas palabras, nunca cuáles.
- **Relaciones de cada intervención:** sobre música o sola, se solapa con otra, o responde (el
  coro que entra tras otra voz en menos de 1,2 s).
- **Las voces llegan a la letra.** Una frase sin voz toma la voz que se oye en ella, si se oye
  al menos el 30 % de la frase. Esa voz se añade al reparto (`u:<pista>`, papel desconocido) con
  sus figuras como caras, así cada cara canta solo lo suyo.
- **El reparto se completa.** Una voz nombrada sin cara (el narrador y el coro, que el reparto
  antiguo no reparte) toma las figuras propuestas, siempre que xLights pueda dibujarlas.

## Figuras (`FigureGeometry`, `FigureSemantics`)

- **Solo APIs nativas.** Se leen `Model`, `SubModel`, `ModelGroup`, `GetFaceInfo`,
  `GetFaceInfoNodes`, las coordenadas de pantalla y `NodeStartChannel`. No hay ningún lector XML.
- **Cifras distintas para cosas distintas.** Las luces, los canales y los efectos son cifras
  diferentes. Cada luz guarda su lugar en la figura (0-1) y su primer canal. Los submodelos se
  numeran con las luces de su padre.
- **Formas irregulares o desconocidas** se guardan como puntos y lo dicen (`limits`, confianza
  más baja).
- **Una parte solo existe si está unida a luces reales:** una definición de cara, las imágenes
  de una cara, un submodelo o luces que eligió una persona.
  - El nombre de un submodelo («Boca», «Tronco») solo la sugiere, y una boca sugerida no habla.
  - Cada parte tiene su color propio (el tronco marrón, la boca roja), aparte de la paleta de la
    canción.
- **Solo dibuja la cara de xLights.** Para el efecto Faces solo vale una boca de una definición
  de cara o de imágenes. Una boca marcada a mano describe la figura y se dibujará en una fase
  posterior.
- **Las decisiones de la persona** se aplican sobre lo hallado (`ApplyFigureChoices`) sin
  necesidad de tener el show cargado. Así la ventana las enseña al momento.

## Quién interpreta qué (`PerformanceMapping`)

- **Reparto automático:**
  - primero las figuras con controlador y una cara que xLights dibuja;
  - el narrador, a ser posible en un árbol o figura;
  - el solista, en una cara;
  - el coro, en varias caras;
  - el turno de los instrumentos, en el mega árbol o la matriz;
  - la percusión, en los arcos.
- **La persona gana siempre.** Sus elecciones se guardan por canción en `performers.json`.
- **Problemas con nombre:**
  - sin boca;
  - sin controlador;
  - parte que falta;
  - dos voces a la vez en una boca;
  - pocas figuras para el coro;
  - ninguna frase de la letra para esa voz;
  - voz sin cara.
- **Lo previsto es lo que hizo el show.** Tras decidir el reparto, las asignaciones se
  sincronizan con él: las figuras de cada voz son sus caras en la secuencia, activas en sus
  frases de la letra. Una voz sin frases no actúa en ningún sitio, y lo dice.

## Línea de tiempo (`MultimodalTimeline`)

- **Doce carriles:** partes, pulsos, compases, fuerza de la música, golpes, narración, voz
  principal, coro, otras voces, instrumentos solos, letra y figuras.
- **Ids estables.** Cada elemento conserva el id de lo que muestra, no su tiempo.
- **Una edición es una corrección.** Mover, partir, unir, reclasificar, dar a otra voz, quitar o
  confirmar se traducen a una corrección del audio o a una asignación de la persona.
- **Lo que no se edita aquí lo dice:** los pulsos y las partes se corrigen en el análisis
  musical, y las palabras en el editor de la letra.

## Lo que hizo el render (`RenderUnderstanding`)

- **El render es de xLights.** Se lee el `.fseq` figura a figura y parte a parte, cada 100 ms:
  brillo, luces encendidas, movimiento y color. Las figuras cuyas luces no están en el archivo
  (sin controlador) se dejan fuera y se dice.
- **Se compara con lo previsto:**
  - una cara apagada o con la boca quieta donde debía hablar;
  - una boca de xLights que se mueve sin voz (más grave en un instrumental);
  - una voz que parece salir por otra cara que no tiene voz propia en ese momento;
  - una parte de una figura con papel que nunca se enciende;
  - todo apagado bajo la música.
- **No juzga a quien no tiene papel.** Una figura sin nada que hacer puede quedarse a oscuras.
- **Momentos para hacer fotos** (`--composer-snapshots`), espaciados y con los problemas
  primero.
- **No aprueba nada.** `review` = `pending` hasta que lo cambie una persona.

## Ventana (los cinco pasos se mantienen)

1. «Entender el audio»: grabaciones separadas, entender en un proceso aparte con progreso y
   cancelación, y la casilla para usar o no lo que se oye. La transcripción sigue en «Sincronizar
   las palabras con IA (Groq)», que pide consentimiento.
2. «Qué está pasando en la grabación…»: los carriles, la línea roja, las correcciones, volver a
   lo automático y recuperar los cambios.
3. «Conocer mis figuras…» (lista, dibujo de las luces con sus partes y colores, confirmar,
   quitar, añadir y colorear partes, papeles, revisada) y «Asignar voces y personajes…».
4. La generación lleva `--understanding on/off`. El planificador no tiene motor procedimental:
   usa el reparto y las frases.
5. «Qué hicieron las figuras…»: lo previsto frente a lo renderizado y fotos de esos momentos.

## Contratos para la fase 17

Quedan preparados, sin motor procedimental (sin shaders, sin cálculo por píxel, sin
compositor propio):
- `FigureProfile.regions`: cada parte con sus luces reales (índices del modelo), su color
  propio, su procedencia y si una persona la confirmó.
- `FigureGeometry.nodeMap`: posición 0-1, posición en el buffer, cadena y primer canal de cada
  luz.
- `PerformerAssignment`: quién (voz o momento), qué figuras, qué hace, con qué parte (`region`) y
  cuándo (`windows`).
- `Intervention.mouth`: las formas de boca de la letra (nombres de xLights), para animar bocas
  que no sean una cara de xLights.
- `RenderUnderstanding`: lo que se pretendía frente a lo que se vio, como comprobación de lo que
  haga la fase 17.

## Límites conocidos

- **La diarización es experimental.** Dos voces del mismo registro pueden unirse, y una
  cantante con mucho cambio de registro aún puede partirse.
- **Sobre música, hablar o cantar es una suposición marcada.** Hace falta su grabación, una
  etiqueta o una corrección.
- **La polifonía en una mezcla incluye acordes de instrumentos.** «Coro» sobre música es una
  pista, no una certeza.
- **El render se mide por luz, no por imagen.** No sabe si un dibujo o una palabra son los
  correctos, ni si una boca dice el sonido correcto.
- **Solo se ha verificado en Windows** (no hay Mac): ni macOS ni `xLights-iPadLib` se han
  compilado.
