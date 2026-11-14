# Fase 17 — Director de escena global, coreografía espacial y composición procedural

Decisiones de diseño de la fase. Los resultados medidos y las pruebas reales están en el informe
`JUAILILANDIA_REFERENCE_PACK/REPORTS/FASE17_COREOGRAFIA_GLOBAL_PROCEDURAL.md`, que no se publica.

## Qué cambia

Hasta la fase 16 cada figura seguía su propio plan. Ahora, antes del detalle de cada figura, un
director decide cómo se usa **toda la instalación** en cada escena: una figura sola, dos, un
grupo, toda la casa o un descanso. También decide qué momentos de la música merecen un gesto que
cruce las figuras. Todo es núcleo sin wx (`src-core/composer`, más el efecto en
`src-core/effects`). La ventana muestra la partitura y deja cambiarla.

## Un solo escenario (`StageSpace`)

- **Cuatro tipos de lugar, separados:**
  1. la luz dentro de su modelo (el espacio del modelo de xLights, el que lleva el búfer de render);
  2. el modelo dentro del layout (unidades del layout, con posición, tamaño y giro);
  3. el escenario: el layout metido en un cuadrado 0-1, la unidad en que se miden los gestos;
  4. distancias reales, solo si una persona dice que el layout está a escala (`stage.json`).
- **El layout es un dibujo, no una medida.** Sin calibración, el escenario es «esquemático» y lo
  dice (`limits`). Nunca se inventan metros.
- **Una transformación exacta por figura** (del espacio del modelo al escenario), ajustada por
  mínimos cuadrados sobre todas sus luces. Se mide su error peor y, si pasa de 0,002, se marca
  como aproximada. Las figuras de dos o tres puntos (líneas, arcos) piden antes
  `PrepareToDraw`.
- **Orígenes con id estable que se comprueba:** `node:<figura>:<n>` (una bombilla),
  `region:<figura>:<parte>`, `figure:` y `center:<figura>`, `group:<grupo>` y `point:x,y`. Un
  origen que ya no existe se dice (`figure-missing`, `light-missing`…) y nunca se adivina.

## Campos de luz (`ProceduralField`) y el efecto «Stage Field»

- **Doce funciones deterministas:** pulso, ola, ondas, expansión, barrido, persecución,
  espiral, flujo, fundido, degradado, decaimiento y eco. Las mismas entradas dan siempre la
  misma luz; el flujo usa una semilla con hash, nunca el azar.
- **Llegada = t0 + d / v.** La distancia se mide en el escenario (radial, lineal o elíptica).
  El frente sube en `attack`, se mantiene `width / speed` y baja en `release`.
- **Un efecto nativo nuevo, «Stage Field».** Dibuja un campo compartido. Cada figura lleva en
  su especificación (`E_TEXTCTRL_StageField_Spec`) el mismo campo, su propia transformación
  (`m=`) y el momento en que se une (`at`). Así una ola cruza modelos de cualquier forma como un
  solo movimiento. Un `at` negativo significa que el efecto empieza antes y queda a oscuras hasta
  su momento (se admite hasta 60 s antes).
- **Luces que no toca** (`only`, `skip`, números de luz de xLights). La boca y los ojos de una
  cara nunca los enciende un gesto.
- **Alfa con mezcla Normal.** Donde el campo está apagado, la luz es transparente y se ve la capa
  de debajo. Es la única mezcla nativa que respeta el alfa.

## El director (`StageDirector`)

- **Escenas = segmentos del plan.** El modo sale de lo que se oye (fase 16) y del plan:
  - descanso donde la música calla;
  - una figura sola en una narración o en un canto suave;
  - llamada y respuesta cuando el coro responde;
  - dúo con dos voces;
  - un grupo en una parte cantada fuerte;
  - toda la casa en el clímax;
  - propagación con instrumentos fuertes y ondas propias con instrumentos tranquilos.
- **Presupuesto de atención.** Cada papel cuesta: protagonista 2, acompaña 1, responde 1,
  acento 0,5, fondo 0,25. Un descanso tiene 0,5, una escena normal 3–6 y el clímax todo. Entra
  primero quien lleva más tiempo descansando: variedad sin azar.
- **No todo golpe es un gesto.** Como máximo uno a cuatro por escena, separados 1,5 s y solo en
  momentos fuertes. La cascada exige una ráfaga de 3 a 5 golpes fuertes con silencio antes y
  después: un pulso regular no es una ráfaga. En un grupo, el gesto principal rota entre
  destello, ondas propias y ola desde el cantante, y la misma música (su grupo de repetición)
  empieza igual.
- **La velocidad la pone la música:** la ola cruza a sus figuras en unos dos pulsos (0,15–4
  unidades por segundo).
- **Nunca sobre una cara que canta.** Una figura cuya cara canta en ese momento no recibe gestos.
  Tampoco los recibe un modelo cableado a los mismos canales: un layout puede describir un objeto
  dos veces, y una ventana y una cara pueden ser las mismas luces. Una cara que canta llama con
  su propio canto y las demás le responden con luz un pulso después.
- **Una figura alcanzada después de que acabe su escena no participa,** porque recibiría un
  efecto que nunca se enciende. Una figura que es casi todo cara (≥ 80 % de boca y ojos) no
  recibe gestos.
- **Sus propios colores (capa de identidad).** Una figura protagonista o que acompaña mantiene,
  debajo de todo, el color de sus partes (ramas, copa, contorno…) al 55 %. Solo valen partes
  seguras: un submodelo, un nombre, una definición de cara o la confirmación de una persona;
  nunca una suposición por la forma. En una cara que canta solo se hace si canta con la misma
  definición de la que se leyó su boca.
- **Cambios de una persona** (`ShowComposer/stage/<id del audio>.edits.json`): modo, meter,
  sacar, sin gestos, añadir un gesto, origen, velocidad, sus colores sí o no. Se aplican antes de
  decidir los papeles y los gestos de su escena, y siempre ganan. Un gesto que pone una persona
  se hace aunque esté excluido de la biblioteca. Un cambio cuya escena ya no existe se dice.

## Realización (`RealizeStage`)

- Un efecto «Stage Field» por figura y gesto. Empieza 100 ms antes de su llegada, con los tiempos
  en fotogramas, porque xLights los guarda así y la comprobación de lo escrito compara al
  milisegundo.
- **Capas limpias por figura:** arriba los gestos, después sus efectos tal como estaban y abajo
  su identidad. Nada comparte capa.
- En las escenas de una figura sola, dúo, llamada y respuesta o descanso, se quita el fondo de las
  figuras que descansan. Se conservan sus caras, letras, imágenes y efectos principales.
- **Las filas importan.** xLights dibuja una fila sobre la anterior (mezclada, con la mezcla de
  modelos activa). Los grupos que contienen una figura con gestos pasan delante de ella, así el
  gesto queda encima del efecto del grupo y, donde está apagado, el grupo se ve.

## Lo que hizo el render (`StageCheck`)

- Se lee el `.fseq` con el lector de xLights: qué parte de la instalación se encendió, cuántas
  figuras a la vez, cuánto se concentra la luz (las tres más brillantes) y cuánto cambian las
  escenas.
- Para cada figura de cada gesto, cuándo sube su luz frente a cuándo dice la partitura. En un
  show completo es aproximado: una figura ya brillante por sus otros efectos no muestra subida.
- Son medidas, nunca una aprobación (`review` = `not-reviewed`). Con `--composer-stage-measure`
  se mide cualquier versión de un show, también las hechas sin director.

## Ventana (los cinco pasos se mantienen)

- Paso 2: los gestos de la instalación, en su propio carril de la línea de tiempo.
- Paso 4, «Director de escena»: dirigir o no la instalación como un escenario; las escenas con su
  modo, cuántas figuras participan y cuántos gestos hay; el porqué en palabras, sin ecuaciones ni
  coordenadas; los cambios, deshacer y aplicar (se vuelve a preparar la composición aparte, la
  ventana sigue respondiendo).
- «Coreografía de la instalación»: cada luz en su sitio del layout, reproducida como prevé la
  partitura o como la dibujó xLights, con barra de tiempo, reproducción lenta y «Gesto
  siguiente».
- «Capas de esta figura»: lo que tiene una figura de arriba abajo en una escena, y sus propios
  colores sí, no o como decida el director.
- «Composiciones y gestos»: favoritos, excluidos y composiciones propias. Las recetas de las
  referencias se usan, pero solo se cuentan: siguen en este equipo.
- Paso 5, «La instalación entera»: el mapa con el render real del show generado.

## Límites conocidos

- El escenario es esquemático salvo calibración. «Lejos» significa lejos en el dibujo.
- Las llegadas medidas en un show completo son aproximadas.
- Los colores de un gesto son los de los efectos principales de su escena. Si toda la casa ya es
  de ese color, el gesto se nota poco: lo juzga una persona.
- Sin ventana en el iPad. El efecto se renderiza allí porque el núcleo es compartido.
