# Fase 19 — Dirección artística, armonía cromática y motivos visuales

Decisiones de diseño de la fase. Los resultados medidos, las seis versiones de la canción real de
prueba y las capturas están en el informe `JUAILILANDIA_REFERENCE_PACK/REPORTS/FASE19_DIRECCION_ARTISTICA.md`,
que no se publica.

## El problema medido

La Fase 18 consiguió que la casa no fuera de un solo color, pero lo hizo con tonos lejanos entre sí
a la vez. La auditoría midió el render de las fases 14, 17 y 18 de la canción de prueba en las
mismas escenas:
- en la Fase 18 la dispersión del tono (0 = un tono, 1 = tonos opuestos repartidos) era más del doble
  que en la 14 y en la 17;
- había más de cuatro familias de color encendidas a la vez de media, y cada figura llevaba varias;
- la riqueza de la Fase 14 y de las referencias más cohesionadas (medidas en local) no venía de más
  tonos, sino de un tono dominante con blanco, luz y sombra.

La conclusión de diseño: **variedad dentro de una gama, no más gamas**. «Más colores que la
Fase 18» no se acepta como mejora.

## Medir la cohesión (`ColorCheck`)

Nuevas señales, siempre medidas en el render y nunca un juicio:

| Señal | Qué mide |
|---|---|
| `familiesAtOnce` | familias de color encendidas en el mismo instante |
| `topTwoShare` | luz que se llevan las dos familias principales |
| `hueSpread` | 1 − |resultante del tono pesada por la croma| |
| `minorLight` | luz en familias que no llegan al 10 % cada una |
| `colorsPerFigure` | familias que lleva una figura a la vez |
| `overloadedScenes` | escenas con más de cuatro familias a la vez |

Avisos: demasiados colores a la vez, colores dispersos (`minorLight` > 0,35) y figuras con muchos
colores (> 2,2).

## La gramática de la paleta

La paleta de la Fase 18 tiene ocho papeles; ahora cada escena **usa solo los que necesita**
(`used`):
- siempre: el principal, su versión clara, su sombra y el neutro;
- los papeles del mismo tono (a menos de 40° del principal) entran sin coste;
- cada **gama de tono** distinta cuesta una unidad del presupuesto de la escena.

## El presupuesto de tono

`hueBudget = 1 + floor(importancia × (0,8 + 2 × estilo) + 0,1)`, entre 1 y 3.

- La **importancia** sale de la evolución de la escena: descanso 0; establecer o resolver 0,25;
  desarrollar, mantener o volver 0,45; abrirse o girar 0,7; clímax 1. Un estribillo que vuelve
  cuenta como abrirse.
- Con menos de cuatro figuras participando, como mucho 2.
- «Reducir colores» y «monocromo» lo bajan.
- La parte del acento en los tiempos fuertes también depende de la importancia y del estilo
  (como mucho el 60 %); «Menos acentos» la reduce.

## Estilo: contenido ↔ expresivo

Un solo control (0,4 por defecto) que cambia **qué se elige y cómo se reparte**, no la saturación:
- el presupuesto de tono de cada escena;
- la luz que se lleva el principal (de 70 % a 45 %);
- si las figuras decorativas van en blanco o en un tono vecino;
- si los acentos duran o son puntuales, y si las figuras alternan colores;
- si un desarrollo puede desplazar un poco el tono (solo muy expresivo).

Lo aprendido como `color.cohesion` mueve el estilo por defecto.

## Motivos cromáticos

Un motivo es una idea de color con nombre (`A`, `B`…) que la canción presenta, desarrolla,
recupera, intensifica, transforma y resuelve:
- **identidad** (`A`): se presenta en la primera escena musical; las estrofas la desarrollan;
- **contraste** (`B`): la identidad con un contraste, cuando la música se abre o en el clímax;
- **transformado**: la identidad girada hacia su segundo color (como mucho 70°) cuando la música
  gira a otro sitio;
- una parte que se repite recupera el motivo de su primera vez; lo intensifica si llega con más
  energía, en el clímax o en la última repetición;
- el final resuelve a la identidad.

No hay variación gratuita: un motivo solo cambia cuando cambia la música (sección, repetición,
giro) o lo pide una persona. La lectura de Groq versión 3 puede decir qué partes **mantener** (no
se introduce ni se intensifica) y cuáles **cambiar** (se permite transformar).

## Jerarquía entre figuras

- El protagonista del director de escena lleva el acento cuando la escena es de protagonista, dúo
  o pregunta-respuesta; en el resto, el principal.
- Las figuras que el director de escena deja descansando en esas escenas pasan a fondo y se
  nombran como **discretas**.
- Lo aprendido de «esta figura debería destacar / descansar» sube o baja su papel.
- Los colores propios de una figura y lo que fijó una persona siempre van primero.

## Gestos con intención

| Intención | Cuándo | Color |
|---|---|---|
| reforzar | lo normal | el principal o su luz, si se distingue de lo encendido (ΔE ≥ 18) |
| acento | escenas fuertes, pocos | el acento |
| transformar | en el último cuarto, si la escena siguiente cambia de motivo | prepara el color siguiente |
| resolver | en las escenas que cierran | la identidad |

Un gesto ya no busca siempre el máximo contraste.

## Recetas y capas

Cada efecto toma el color según su función: el acento en los golpes (dentro de la parte de
acento), el apoyo rota los colores de su papel, el descanso usa la sombra. Cada papel de figura
tiene un solo tono: principal (principal, luz, sombra), complemento (el segundo tono o un vecino),
acento, respuesta, fondo, neutro, decorativo e identidad.

## Evaluar sin notas (`ArtEvaluation`)

`composition-eval.json` lee la composición en diez señales con palabras (cohesión, variedad,
contraste, jerarquía, colores por figura, sobrecarga, gestos, continuidad, motivos, identidad) y
su porqué. **No hay porcentaje de calidad**: `score` es siempre `null` y nada queda aprobado.

## Aprender de la versión corregida (`SequenceComparison`)

`--composer-compare` compara la secuencia generada con la que la persona corrigió en xLights,
efecto a efecto con las APIs nativas de lectura: colores (por gamas de tono), efectos, capas,
intensidad, tiempos y figuras. Ningún cambio se aprende solo: la persona marca cuáles son un
gusto. Un gusto necesita dos espectáculos y se puede deshacer; la corrección cuenta como el mismo
espectáculo que su valoración.

## Observaciones con su lugar

Diez observaciones con escena y figura. Se separan en dos clases:
- **decisiones de esta canción** (mantener una combinación, recuperar un motivo): cambian su plan
  de color si la persona lo confirma, y nunca otras canciones;
- **gustos** (demasiados colores, acentos excesivos, transición poco elegante, que una figura
  destaque o descanse…): enseñan a los próximos espectáculos.

## La ventana

- Paso 1: «Estilo de color: contenido ↔ expresivo».
- Paso 2: la idea de color de cada escena en la línea de tiempo.
- Paso 4: «Dirección artística», cada escena en palabras, sus acciones y «Comparar escenas».
- Paso 5: la composición en palabras, «Ver esta escena en la casa», «Comparar con mi versión
  corregida» y «Observaciones con su lugar».

## Límites

- Las gamas de tono se cuentan con una regla fija (40°); dos tonos cercanos con nombres distintos
  (verde y amarillo verdoso) cuentan como una gama.
- Las medidas son señales. Ningún espectáculo queda aprobado sin que una persona lo vea en xLights.
- Verificado solo en Windows.
