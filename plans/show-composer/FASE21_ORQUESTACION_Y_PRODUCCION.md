# Fase 21 — Orquestación automática, presencia escénica y producción final

Decisiones de diseño de la fase. Los resultados medidos, las cuatro versiones de la canción real de
prueba y las capturas están en el informe `JUAILILANDIA_REFERENCE_PACK/REPORTS/FASE21_ORQUESTACION_Y_PRODUCCION.md`,
que no se publica.

## El problema

La Fase 20 hacía que la partitura variara de 3 a 45 figuras por escena, pero la luz total de cada escena
apenas cambiaba. La auditoría lo midió sobre la versión completa de la Fase 20, cruzando la partitura con
la coreografía final y con el render:
- **los efectos de grupo**: un efecto sobre un grupo de xLights ilumina a todos sus miembros. El descanso
  de la Fase 17 solo recortaba efectos dirigidos a la propia figura, nunca los de los grupos que la
  contienen. Fue la causa principal;
- **escenas sin descanso**: el descanso solo se aplicaba en escenas de foco (protagonista, dúo, llamada y
  respuesta o foco explícito). En las escenas de grupo, las figuras «en reposo» seguían con sus efectos;
- los efectos de prominencia principal se conservaban siempre, aunque la figura descansara;
- las colas de algunos gestos pasaban a la escena siguiente.

Medido en el render: en la versión completa de la Fase 20, solo poco más de la mitad de las parejas
figura-escena coincidían con lo que pedía la partitura.

Además, las 12 composiciones existían pero nadie las elegía. El `.fseq` de la generación quedaba en la
carpeta del informe y la producción no tenía un render definitivo.

## Niveles (`ArtLevel`)

Sigue habiendo un solo director. Se añaden tres niveles sobre los de la Fase 20, y cada uno añade al
anterior:

| Nivel | Qué añade |
|---|---|
| `full` | la Fase 20 tal cual (mismo `specHash`) |
| `compositions` | cada escena se construye sobre una composición de la biblioteca, elegida por el conjunto de sus circunstancias |
| `presence` | los efectos de cada figura siguen lo que su escena le pide |
| `orchestra` | las composiciones se enlazan: las figuras entran, se recogen y se funden en las uniones |

El nivel por defecto es `orchestra`, o el del espectáculo anterior. Se elige en el paso 1 o con
`--art-direction`. La versión del director pasa a ser la 3.0.0 desde `compositions`.

## Selector de composiciones (`CompositionSelector`)

Está dentro del director, no en una ventana aparte. Para cada escena pesa las 12 composiciones (más las de
la persona) con criterios que suman o restan puntos y dicen por qué:
- **contexto:**
  - la voz o los instrumentos;
  - el tipo de parte;
  - el papel de la escena en la canción;
  - los golpes que hay que marcar;
- **cuánta casa usa frente a cuánta pide la escena:** local, grupo o toda la casa;
- **intensidad:** una parte fuerte puede llevarla toda la casa; en una tranquila basta una figura;
- **continuidad con la idea visual:**
  - al recuperarla, la composición con la que nació;
  - al desarrollarla, una de un nivel mayor;
  - al intensificarla, una de toda la casa;
  - al resolverla, una de menos figuras;
- **momentos propios de la canción:** el punto culminante empieza, la escena prepara el siguiente, la
  canción se cierra;
- **variedad:** se penaliza repetir la composición de la escena anterior, o la misma varias escenas seguidas;
- **preferencias:** las favoritas y las composiciones propias.

Otras reglas descartan una composición, y también lo dicen:
- la persona la excluyó;
- no hay figuras suficientes, o ninguna con la forma que necesita;
- la escena es demasiado corta;
- no hay un golpe para empezarla, ni una voz que llame, ni un punto culminante que se abra, ni nada que se cierre;
- la persona eligió otro modo para la escena.

La elección es determinista. Gana la puntuación más alta. En caso de empate, gana la composición más
cercana a la cantidad de casa que pide la escena, y después el orden de la biblioteca.

Si ninguna llega a un mínimo, la escena sigue a la manera propia del director y lo explica. Las
composiciones imposibles nunca se fuerzan, tampoco la que elige una persona: se le dice por qué no se
puede hacer.

La composición elegida llega de verdad a la partitura:
- **el modo de la escena:**
  - «Uno cuenta» pasa a protagonista;
  - la llamada y respuesta pasa a llamada;
  - una onda de toda la casa pasa a propagación;
- **cuántas figuras participan**, adaptado a la casa: una composición de grupo de 3 a 4 figuras con solo 3
  disponibles usa 3, y lo dice;
- **el gesto principal de la escena;**
- **si las figuras se responden o van al unísono;**
- **el brillo, la velocidad y la duración** que la persona haya fijado;
- **las figuras con las que se hizo** una composición propia, o una figura con la forma que necesita.

Una ráfaga de golpes ya no se queda con la escena si se eligió otra composición. Las composiciones de
momento (barrido antes del clímax, apertura, recogida, eco) las hacen las mismas piezas de la Fase 20, que
ahora saben qué composición realizan.

Cada escena guarda la composición elegida, sus razones, cómo se adaptó y todas las candidatas con su
veredicto. La persona puede cambiarla en el paso 4 (edición `composition`).

## Presencia escénica (`StagePresence`)

Cada figura tiene en cada escena un estado y una política:

| Estado | Política | Qué se hace con sus efectos |
|---|---|---|
| protagonista, complementaria, respuesta | normal | se mantienen |
| ambiente | reducida | se atenúan con el brillo nativo del efecto (`C_SLIDER_Brightness`) |
| descanso | suspendida | se detienen durante la escena |
| protegida | protegida | intactos: una persona los bloqueó o editó |

El director no apaga la casa sin más. Cada escena decide cuánto fondo necesita:
- nada al presentar, preparar y cerrar;
- algo en una narración;
- una parte cuando una idea se desarrolla o vuelve;
- toda la casa en el punto culminante.

El ambiente lo llevan primero las figuras grandes. Nunca una cara que no canta, y nunca una figura que no
tiene nada propio que mostrar en esa escena. Una figura sin luz propia, sin gesto y sin capa de identidad
en una escena figura como «descansa» aunque su papel fuera acompañar: la partitura dice lo que el render
va a mostrar. La persona elige el brillo del ambiente (paso 1, `--ambient-level`, por defecto el 40 %), y
sus correcciones pueden enseñarlo (`stage.ambience`).

La presencia se aplica a todos los efectos que generan los demás subsistemas, después del color y antes de
los gestos del director:
- **efectos de modelo y de submodelo:** siguen la política de su figura;
- **efectos de grupo cuyos miembros reciben políticas distintas:** dentro de la escena se dividen en un
  efecto por cada figura que sigue tocando, atenuado si va en ambiente. Se colocan bajo los efectos propios
  de la figura y nunca comparten capa. Los miembros que no son figuras del escenario conservan el efecto;
- **siempre intactos:** caras, textos, imágenes, vídeos y lo que una persona bloqueó o editó;
- **gestos y capas de identidad:** son del propio director y van después.

La prioridad, de mayor a menor:
1. bloqueos y ediciones de la persona;
2. funciones protegidas (cara que canta, letra, medios);
3. capas de identidad;
4. presencia escénica;
5. composición;
6. efectos ambientales.

Cuando la presencia deja a una figura sin ningún efecto, el informe de cobertura lo dice.

### Luces compartidas

Dos figuras conectadas a los mismos canales muestran lo mismo. Se detectan los pares en el escenario y su
relación (una dentro de otra, iguales o solapamiento parcial). En cada escena en que las dos tocarían:
- si una persona eligió cuál gana (edición `priority`, paso 4), la otra descansa;
- si una está dentro de la otra, cede la que la escena pide menos, y sale también de los gestos de esa escena;
- en un solapamiento parcial no se apaga una figura grande por unas pocas luces: se informa de cuál se ve
  (la fila que xLights dibuja después) y se invita a elegir.

La comprobación de producción informa de cada par:
- cuántas luces comparte;
- cuánto tiempo tocan a la vez las dos;
- cuál se ve;
- si se turnan sin conflicto.

## Transiciones entre composiciones (`CompositionTransition`)

Cada unión entre escenas recibe un tipo, sacado de las composiciones, del motivo visual y de cuántas
figuras tocan: continuación, variación, respuesta, propagación, recogida, transformación o resolución.

Las uniones se materializan en los efectos:
- **las figuras que entran:**
  - en una propagación o transformación, entran escalonadas por su distancia al punto donde empieza la
    idea siguiente (hasta dos pulsos), así la luz se extiende en vez de reiniciarse;
  - entran con un fundido de medio pulso;
- **las figuras que salen:**
  - en una recogida o resolución salen primero las más lejanas y las cercanas las últimas;
  - nunca alargan la luz dentro de un silencio;
  - siempre salen con un fundido, sin pisar su efecto siguiente.

Cada unión queda en la partitura con quién entra, quién sale y qué se comprobó:
- cortes bruscos que quedan;
- saltos de intensidad;
- dos efectos en la misma capa a la vez, que no debe haber ninguno.

## Partitura frente a render

`CheckStage` mide ahora, en cada escena y para cada figura, lo que se le pidió (tocar, ambiente o descanso)
y lo que se vio. Una figura cuenta como encendida si en promedio tiene encendida una vigésima parte de sus
luces, o si tuvo al menos el 30 % encendido durante un cuarto de segundo, para que un gesto breve también
cuente.

Da la coincidencia y separa:
- las figuras en descanso que se vieron encendidas, y cuántas de ellas por compartir luces con otra que
  toca, que es un límite físico del montaje;
- las figuras que debían tocar y quedaron a oscuras;
- el brillo medio de las que tocan, de las de ambiente y de las que descansan.

## Gestos en el tiempo y en el espacio

Cada gesto se lee en el render:
- **en el tiempo:** antes, su momento de más luz, después y su decaimiento;
- **su subida:** cuánto subió frente a cuánto cambió el resto de la casa;
- **el orden de llegada:** frente al orden pedido (Spearman);
- **la coherencia del tiempo:** R² entre llegadas vistas y pedidas;
- **en el espacio:** el recorrido y el mayor hueco;
- **en una respuesta:** si la figura que empieza se encendió primero y si las respuestas se distinguen.

No se exige a ningún gesto un ΔE ni un umbral: se mide la intención, sin imponer una estética. Una onda de
toda la casa no puede destacar en color frente a la propia casa, pero sí en el tiempo y en el recorrido.

## Aprendizaje de correcciones humanas

La comparación de la Fase 20 lee ahora también la partitura de la versión generada (`--stage-score`).
Escena por escena detecta:
- figuras que la persona apagó o quitó, distinguiendo si eran el fondo o las que llevaban la escena;
- figuras que añadió;
- figuras que añadió escalonadas, que se responden;
- la idea de una figura convertida en la de un grupo;
- las figuras de una escena anterior recuperadas.

Los efectos de grupo cuentan para sus miembros. Cada cambio se puede:
- aprender, para los próximos espectáculos cuando dos coincidan;
- aplicar solo a esta canción (incluir o excluir esas figuras en esa escena);
- convertir en preferencia general;
- guardar como composición propia (las figuras con su orden en el tiempo);
- ignorar;
- rechazar: no se vuelve a ofrecer.

Nada se aprende sin elegirlo. Las pruebas usan una corrección **simulada**, marcada como tal: todavía no
existe una versión corregida por el creador del espectáculo, y la validación real queda pendiente.

## Producción

**Render definitivo** (`RenderFinal`, `--composer-final-render`, paso 5):
1. Se abre la secuencia como la abre xLights y se renderiza con el renderizador nativo.
2. Se escribe donde xLights reproduce el render: la carpeta de renders configurada en el show o, si no hay
   ninguna, la del show, igual que el escritorio al guardar.
3. Antes de poner el archivo en su sitio se vuelve a leer y se comprueba:
   - cabecera, fotogramas, duración de cada fotograma y canción, frente a la secuencia;
   - cada fotograma, frente a lo renderizado;
   - que ningún canal que el archivo deja fuera (como hace el escritorio con los rangos sin modelo) tenga
     luz.
4. Un render anterior de la misma secuencia se guarda aparte en `ShowComposer/renders-anteriores`. Nunca se
   sobrescribe, y un render que falla una comprobación no se pone en su sitio.
5. No se enciende ningún controlador.

**La comprobación de producción:**
- busca el render donde xLights lo reproduce;
- lo compara fotograma a fotograma con un render nuevo, en vez de comparar los archivos, que incrustan cosas
  distintas;
- analiza las luces compartidas.

**El paquete** lleva:
- el render comprobado;
- la descripción del montaje (huella, modelos, canales, controladores y pares de luces compartidas), nunca
  el montaje;
- todos los estados.

**Los estados están separados:**
- generado;
- validado;
- renderizado;
- medios completos;
- guardado de nuevo por xLights e igual;
- render definitivo comprobado;
- paquete preparado;
- revisión;
- listo para la instalación real.

Solo está listo para la instalación real cuando una persona lo ha aprobado y además pasan las
comprobaciones. Pasar las pruebas nunca basta.

## Interfaz

Los cinco pasos de siempre:
- **Paso 1:** el nivel, con la versión activa del director, y el brillo del ambiente.
- **Paso 2:** «Ideas visuales y su desarrollo» dice con qué composición nació cada idea y cuál la lleva en
  cada escena.
- **Paso 3:** las composiciones, sus valores y sus participantes, como en la Fase 20.
- **Paso 4: orquestación de la instalación.**
  - Para cada escena: su composición, cuántas figuras tocan y cuántas van en ambiente.
  - En el detalle: por qué se eligió la composición y qué otras eran posibles, quién protagoniza,
    acompaña, responde, va en ambiente o descansa, cómo enlaza con la escena anterior y qué pares de luces
    ceden.
  - «Cambiar la composición...» y «Figuras que comparten luces...».
  - Se aplica con «Aplicar los cambios» (regenerar).
- **Paso 5:** «Hacer el render definitivo...» corre como tarea aparte, sin congelar la ventana. La
  comprobación muestra todos los estados, y en la versión corregida se puede «Guardarla como composición».

## Lo que queda

- **Corrección real:** validar el aprendizaje con una versión corregida por el creador.
- **Regeneración:** cambiar una composición regenera el espectáculo entero, aprovechando lo que ya está en
  caché; no hay una regeneración solo de escena.
- **Luces compartidas en solapamiento parcial:** solo se informa; no se recortan efectos luz a luz.
- **Selector:** sus pesos son una primera calibración, sin revisión visual.
- **Verificado solo en Windows:** no se ha compilado `xLights-iPadLib`.
