# Fase 18 — Dirección cromática, contraste y evolución artística

Decisiones de diseño de la fase. Los resultados medidos y las pruebas reales están en el informe
`JUAILILANDIA_REFERENCE_PACK/REPORTS/FASE18_DIRECCION_CROMATICA.md`, que no se publica.

## El problema medido

El render de F17 de la canción real de prueba confirma lo que vio el desarrollador:
- un solo color (rojo) ocupaba más del 60 % de la luz de la casa el 62 % del tiempo;
- el rojo era el 70 % de toda la luz;
- los gestos se diferenciaban del resto de la casa en solo 9 puntos ΔE.

Hay cuatro causas, todas en el código:
1. Cada escena tenía una sola lista de colores, la misma para todas las figuras; solo se rotaba
   el primero.
2. Las estrofas usaban «principal + principal claro», y la influencia navideña al 85 % los
   dejaba en dos rojos casi iguales.
3. Los gestos copiaban los colores de los efectos principales de su escena, que eran los del
   fondo.
4. Los estribillos repetidos copiaban exactamente su paleta.

En las referencias estudiadas en local, un solo color domina mucho menos tiempo. La
variedad está en las figuras dentro de cada escena, no en cambiar toda la casa. Sus números se
quedan en el informe local, porque derivan de paquetes comerciales.

## Color perceptual (`ColorSpace`)

- OKLab/OKLCH (Ottosson, 2020): pasos iguales parecen igual de distintos.
- La diferencia es ΔE: distancia en OKLab ×100 (1 apenas se nota; 10 se nota claramente; 30 es
  otro color).
- Doce familias de tono de 30° más «blanco» y «gris» para los colores sin croma.
- **Gama:** un color imposible se ajusta bajando solo la croma (se conservan la luz y el tono).
- **Cúspide:** cada tono se elige cerca de la luminosidad donde puede estar más saturado (un azul
  profundo, un amarillo claro), que es donde un LED lo muestra entero. Aclarar un cian o un
  amarillo solo lo blanquea, así que no se hace.
- Los valores de xLights se leen como sRGB. Es una convención y se dice.

## Paletas jerárquicas (`ColorPaletteProfile`)

- **Ocho papeles, una familia:**

  | Papel | Función |
  |---|---|
  | principal (ancla) | da la identidad |
  | secundario | varía |
  | acento | lo importante, luminoso |
  | contraste | separa, en otra luz |
  | transición | a medio camino en OKLCH |
  | neutro | blanco cálido o frío |
  | profundo | sombra |
  | claro | brillo |

- **Siete armonías:** monocromática, análoga, complementaria, complementaria dividida, triádica,
  tetrádica y cálido-frío. Ninguna es un preset: cada una da desplazamientos de tono, y los
  colores relacionados se mueven hacia los que sugiere la canción si están a menos de 36°.
- Los colores sugeridos vienen, en orden, de la persona, del análisis artístico y de la Navidad
  (con el peso del control).
- **Elegir la armonía** es una puntuación, no una tabla:
  - pesa sobre todo el contraste que pide el momento;
  - después, el carácter de la canción o de esa parte;
  - después, la familia de la canción;
  - la variedad penaliza repetir.
- **Monocromo solo a propósito:** silencios, introducciones o finales tranquilos, o una edición.
  Se marca y el monitor de monotonía no lo cuenta.

## El director por escenas (`ChromaticDirector`)

- **Encima del director de escena, sin duplicarlo:** lee sus escenas, papeles y gestos, el arco
  del plan visual y la comprensión del audio. No decide quién actúa, solo de qué color.
- **Evolución:**

  | Momento | Etiqueta |
  |---|---|
  | primera aparición | presentar |
  | estrofas | desarrollar |
  | estribillo y preestribillo | ampliar |
  | puente e instrumental | cambiar de temperatura o familia |
  | final | resolver |
  | parte que vuelve | volver |

  Las partes repetidas vuelven a su motivo: igual, con el acento aclarado, o con los colores
  relacionados del otro lado del círculo. Se parecen sin ser idénticas.
- **Papeles por figura**, por la luz que ocupan (hacia 60-30-10):
  - las familias más grandes llevan el principal, las siguientes el segundo y las pequeñas los
    acentos;
  - los papeles de escena mandan sobre esto (protagonista, respuesta, acento);
  - en escenas vivas, uno de cada dos de una familia numerosa alterna;
  - los grupos son el lienzo: el principal profundo, o el color de su familia.
- **Identidad:** un personaje, o una figura cuyos colores fijó una persona, conserva sus colores
  propios. Hay excepciones explícitas: «identidad» y «sin identidad».
- **Gestos:** el color del frente es el de la paleta de la escena (o el acento o contraste de la
  canción) que más se aleja de lo encendido en sus figuras.
  - El blanco cuenta como último recurso.
  - El gesto lleva un segundo color, una cola más profunda, y cómo se reparte la paleta en el
    efecto Stage Field: por distancia (la ola cambia de color al cruzar la casa), por estela
    (frente y cola) o por el campo.
  - Nunca se usan colores aleatorios.
- **Transiciones** sincronizadas con la música:

  | Situación | Transición |
  |---|---|
  | caída, reentrada o clímax | corte |
  | crecimiento | el color siguiente entra antes |
  | cambio grande | cambio del principal figura a figura |
  | cambio pequeño | deslizamiento |
  | vuelta a un motivo | regreso |

  Se hacen con **curvas de color nativas de xLights** en cada hueco de la paleta y avanzan de un
  lado de la casa al otro: no cambia toda la casa a la vez. Entre colores lejanos, el punto medio
  pierde croma para no pasar por un tercer color.
- **Ediciones** en `ShowComposer/color/<audio>.edits.json`:
  - cambiar un color o el acento, fijar una paleta;
  - más o menos contraste, una sola familia;
  - colores de una figura, conservar o no sus colores propios.

  Se aplican al recomponer, se deshacen, y «volver al plan original» las quita todas.

## Medir lo que mostró el render (`ColorCheck`)

- Se lee el `.fseq` luz a luz en el orden de color de cada modelo (RGB, GRB…).
- **Se mide** por escena, por figura y para toda la casa:
  - las familias y su reparto, y la entropía («unas N familias se reparten la luz»);
  - el tiempo con un solo color sobre el 60 % de la casa, y el tramo más largo;
  - el cambio de mezcla entre escenas;
  - la diferencia entre figuras;
  - los acentos minoritarios;
  - para cada gesto, su ΔE frente a lo que había antes en esas mismas luces y frente al resto de
    la casa.
- **Avisos con su porqué:** ninguna métrica es un objetivo. `review` es siempre `not-reviewed`.

## Groq: interpretar, no elegir colores

- **Lectura artística, versión 2:**
  - el carácter de la canción;
  - cada parte con su sentimiento, intensidad y un color;
  - la intensidad y la temperatura del color;
  - cómo contrastan las partes.
- **Lo que recibe Groq:** el título, la letra y un resumen acústico medido aquí (tempo, compás,
  tonalidad y partes con su energía). Nunca el audio.
- El motor local convierte esa lectura en paletas y la cita escena a escena.
- **El modelo** se elige de la lista que ofrece la clave en ese momento, prefiriendo los que
  siguen bien un esquema.
- **`artistic-check` dice exactamente:**
  - el estado: `REAL_REQUEST`, `CACHE_HIT`, `LOCAL_FALLBACK`, `NOT_CONFIGURED` o `ERROR`;
  - de dónde viene la clave, nunca su valor;
  - el modelo, la fecha, la validación, el respaldo local y qué recibió.
- **Origen de cada lectura:** «consultado ahora», «respuesta guardada», «leída de un archivo» o
  «local». Una respuesta guardada nunca se presenta como una consulta.

## Aprendizaje

- Once observaciones de color en la valoración sugieren diez reglas. Solo valen las que marca la
  persona.
- Las contradictorias se rechazan, y un gusto necesita dos espectáculos.
- Mueven el punto de partida de: variedad, contraste, acentos, evolución, transiciones más
  suaves, colores más o menos intensos, más cálidos o más fríos, y figuras con sus colores propios.

## Ventana

Los cinco pasos siguen igual:

| Paso | Qué se añade |
|---|---|
| 1 | colores de la canción, cuatro controles y la comprobación con Groq |
| 2 | un carril de colores en la línea de tiempo |
| 3 | colores de las figuras |
| 4 | el director cromático |
| 5 | los colores del espectáculo: plan frente a render, escena a escena y figura a figura, con la evolución en una franja |

Las métricas avanzadas quedan en el informe.
