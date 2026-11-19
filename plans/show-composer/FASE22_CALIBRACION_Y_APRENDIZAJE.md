# Fase 22 — Calibración artística, fidelidad de grupos y aprendizaje del creador

Decisiones de diseño de la fase. Los resultados medidos, las versiones de la canción real de prueba y las
capturas están en el informe `JUAILILANDIA_REFERENCE_PACK/REPORTS/FASE22_CALIBRACION_Y_APRENDIZAJE.md`,
que no se publica.

## El problema

La auditoría de la versión completa de la Fase 21 encontró cuatro cosas.

1. **Una composición dominaba la canción.** Una sola composición llevaba casi la mitad de las escenas.
   Tenía tres causas:
   - sus contextos eran muy amplios (voz sola, estrofa y grupo), así que no tenía rival en las estrofas;
   - la primera idea visual nació con ella, y cada vez que la idea volvía sumaba puntos de nuevo;
   - la penalización por repetir solo miraba la escena anterior.

   Bajar su peso o añadir azar habría escondido el síntoma sin cambiar la manera de decidir.
2. **Los efectos de grupo se troceaban.** Cuando las figuras de un grupo de xLights tenían papeles
   distintos, la Fase 21 partía el efecto del grupo en un efecto por figura. Así se perdía el dibujo:
   - un radial o un barrido deja de recorrer el grupo;
   - un relleno se convierte en piezas sueltas.
3. **Las luces compartidas no estaban explicadas.** Solo se sabía que dos figuras compartían canales. No se
   sabía:
   - si era a propósito (una figura dentro de otra);
   - si eran las mismas luces descritas dos veces;
   - si se trataba de un error del layout.
4. **Las correcciones se leían mal.** Las correcciones de una persona solo se reconocían por cambios de
   presencia. Un cambio de protagonista, una respuesta añadida o una composición distinta se perdían.

## Selector de composiciones para toda la canción (`PlanCompositions`)

La elección deja de hacerse escena a escena y pasa a ser un plan para toda la canción.

**Puntuación local.** Para cada escena se calcula con el selector de la Fase 21, sin sus términos de
variedad. Por eso la puntuación local no depende del orden.

**Términos de canción.** Se suman al recorrer la canción:

| Situación | Efecto en la puntuación |
|---|---|
| Recuperar una idea con la composición con la que nació | Gratis, pero cada repetición seguida cuesta 1,5: otra forma mantiene viva la idea |
| Misma composición que la escena anterior | −3 |
| Misma estructura de gesto que la escena anterior | −1 |
| Usarla sin recuperar ninguna idea | −1,5 por cada uso anterior de ese tipo |

**Búsqueda.** Es una búsqueda en haz de anchura 256. Es determinista: los empates se resuelven por el
orden de la biblioteca. El plan entra en el selector de cada escena como un criterio más, con su razón:
«elegida en el plan para toda la canción». Una elección de la persona sigue mandando sobre el plan.

**Explicación de cada escena.** Cada escena dice por qué tiene su composición:

| Tipo | Significado |
|---|---|
| `recall` | recupera la idea con su composición de origen |
| `only` | es la única que encaja |
| `best` | también es la mejor por sí sola; un empate cuenta como la mejor |
| `song` | otra puntuaba más por sí sola, pero la canción entera es mejor con esta |
| `person` | la eligió la persona |

Junto al tipo se guardan sus puntos, los de la mejor local y las alternativas con sus puntos en la canción.

**Contexto calibrado.** La composición «vecinos que se responden» también encaja con una voz sola. Esto
solo se activa con el plan para toda la canción, de modo que la versión con solo el cambio de grupos sigue
eligiendo como en la Fase 21.

## Efectos de grupo conservados enteros (`GroupEffectArbitration`)

Se estudiaron cinco opciones:

| Opción | Qué hace | Decisión |
|---|---|---|
| A. Política uniforme | Si todos los miembros tienen la misma política, el efecto queda entero | Ya lo hacía la Fase 21 |
| B. Máscaras | El efecto del grupo queda entero, y cada figura que descansa recibe en su propia fila un efecto `On` blanco con «1 is Mask» | **Elegida** |
| C. Campo compartido | Rehacer el dibujo con el efecto Stage Field | Descartada: cambia el efecto que eligió el motor |
| D. Adaptar el efecto | Cambiar el efecto según la presencia | Descartada: no es general |
| División por figura | La reserva de la Fase 21 | Solo se usa cuando la máscara no puede funcionar, y se dice |

**Cómo funciona la máscara.** Las filas se dibujan en orden y los grupos van delante de sus figuras, así
que la máscara apaga lo que el grupo pintó en esa figura y deja intacto el resto del dibujo. Una figura en
ambiente recibe un `On` gris (el nivel del ambiente) con «Brightness», que multiplica lo que hay debajo.

**Cuándo se divide por figura.** Cuando una figura del grupo comparte luces con otra que toca: su máscara
apagaría también las luces de la otra.

**Lo que se mide.** Se renderiza el mismo efecto de grupo de tres maneras: entero, dividido y con máscara.
Para cada una se compara, luz a luz:
- la forma en las figuras que tocan;
- que las figuras en reposo queden apagadas;
- el brillo y la forma en las figuras en ambiente.

**Regla.** Nunca se enciende una figura que debe descansar para conservar la forma. Si el render dice que
la forma no se conserva, no se afirma que se conserve.

**La prueba en grupos reales** (`--composer-group-fidelity --controlled`) hace lo mismo sobre los grupos de
un layout, con una escena controlada:
- elige los grupos por una regla, nunca por nombre: de 5 a 8 figuras con luces y sitio, ninguna con luces
  compartidas;
- en cada grupo, la última figura descansa, la penúltima queda en ambiente y las demás tocan;
- prueba Shockwave, Pinwheel, Bars y Wave con un movimiento claro;
- un caso donde el dibujo no enciende o no se mueve en las figuras que tocan se informa como no medible, no
  como aprobado;
- un grupo con una cara comprueba que una cara que canta, un vídeo y un efecto bloqueado se ven igual que
  solos.

Las máscaras llevan la intención `presence-mask` y la procedencia `stage:presence-mask`. El validador no
las cuenta como efectos sin luz. La cobertura y la comprobación de color las dejan aparte.

## Luces compartidas

Cada pareja de figuras con canales en común se describe con estos datos:
- los canales;
- el controlador de cada figura;
- las luces de cada una;
- los nodos compartidos y los propios;
- dónde están en el layout.

A partir de ellos se clasifica:

| Clase | Qué significa |
|---|---|
| `containment` | una está dibujada dentro de la otra, a propósito |
| `duplicate` | las mismas luces descritas dos veces |
| `partial` | parte de las luces en común, en el mismo sitio |
| `misconfigured` | los mismos canales dibujados en sitios distintos: probablemente un error de cableado o de canal inicial |
| `unknown` | el layout no dice dónde están |

**Cómo se mide la distancia.** Se toma la distancia mediana entre los nodos emparejados y se divide por la
separación habitual entre nodos. Más de dos separaciones indica sitios distintos.

**Lo que puede decidir la persona.** Para cada pareja:
- quién gana en toda la canción;
- quién gana solo en una escena;
- que es intencionado («shared-ok»).

**Cómo se aplica.** La figura que cede conserva sus luces propias. En las compartidas recibe un Stage Field
con «1 is Mask» limitado a esos nodos, en su capa 0. Su fila se dibuja antes que la de la otra.

**Límites.**
- Si la prioridad taparía una cara que canta o un texto, ese conflicto se protege y se dice.
- Nunca se cambia el layout ni se inventa un arreglo de cableado.
- La comprobación de producción avisa de las parejas mal configuradas cuando las dos figuras suenan.

## Aprendizaje de correcciones reales

La comparación con una copia corregida da a cada cambio una naturaleza:

| Naturaleza | Qué cambió |
|---|---|
| `technical` | algo técnico |
| `musical` | el tiempo musical |
| `chromatic` | el color |
| `recipe` | el efecto |
| `protagonism` | el protagonista |
| `presence` | quién toca |
| `composition` | la composición |

Hay tres tipos nuevos:
- `protagonist-changed`: otra figura protagoniza la escena;
- `response-added`: una figura que no tocaba responde;
- `composition-changed`: la persona hizo responderse o tocar juntas a varias figuras.

Las decisiones posibles son:
- aprender para la canción, que se escribe como una edición de escena;
- general;
- guardar como composición;
- ignorar;
- rechazar.

Sin una corrección real del dueño del espectáculo, las pruebas usan correcciones simuladas y lo dicen.

## Regeneración local

«Rehacer solo esta escena» genera el espectáculo entero como siempre. Después toma solo la ventana de la
escena de la generación nueva y todo lo demás, exactamente, de la secuencia anterior. Un efecto que cruza
el borde se corta allí.

El resultado:
- es una secuencia nueva, que nunca se escribe encima de la anterior;
- se valida y se renderiza como cualquier otra.

Si la secuencia anterior no se puede leer, se hace el espectáculo entero y se dice
(`local-regeneration-refused`).

## Producción

La comprobación gana estos avisos:
- figuras sin controlador;
- máscaras de presencia, y máscaras que no quedan encima de lo que tapan (`group-mask-order`);
- luces compartidas confirmadas;
- luces compartidas mal configuradas.

Nunca aprueba nada por sí sola, y el render definitivo anterior se guarda aparte.

## Niveles

`calibrated` (director 4.0.0) es el nuevo valor por defecto. Con estas opciones se separa cada mejora para
compararlas:

| Opción | Valores |
|---|---|
| `--composition-plan` | `song` o `local` |
| `--group-presence` | `mask` o `split` |

Los niveles anteriores siguen dando lo mismo.

## Ventana

- **Paso 1:** ofrece ocho niveles, con «Calibrado» primero.
- **Paso 4:**
  - cada escena explica su plan;
  - la biblioteca dice dónde lleva cada composición la canción, dónde era posible y dónde no encajaba;
  - se pueden decidir las luces compartidas para toda la canción, para una escena o como intencionadas;
  - hay un botón «Rehacer solo esta escena».
- **Paso 5:** la comparación muestra la naturaleza de cada cambio.

Todo corre como trabajo aparte. Los resultados que llegan juntos se aplican de uno en uno, para que la
ventana siga respondiendo.
