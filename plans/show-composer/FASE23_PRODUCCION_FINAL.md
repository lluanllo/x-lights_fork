# Fase 23 — Producción final y congelación

Última fase del Show Composer. No añade funciones: produce un espectáculo completo con lo construido en las
fases 1–22 y congela el desarrollo funcional. El espectáculo, sus evidencias y el informe están fuera del
repositorio, en `JUAILILANDIA_REFERENCE_PACK/FASE23_PRODUCCION/` y `REPORTS/FASE23_PRODUCCION_FINAL.md`.

## Cómo se produce un espectáculo final

Todo se hace con las palancas que ya existen. Ninguna cambia el motor:

| Palanca | Para qué |
|---|---|
| Nivel `calibrated` | el valor por defecto de la Fase 22 |
| `--song-colors` | los colores que una persona pide para la canción: el primero es el ancla |
| `--ambient-level` | cuánto se ve la parte de la casa que acompaña |
| Correcciones musicales (`--corrections`, las mismas del paso 2) | por ejemplo, una sección que el análisis leyó mal |
| Ediciones de color por escena (`lock-palette`, las mismas de la ventana) | qué familia de color lleva cada escena |
| Ediciones de escena (`composition`, `include`, `exclude`) | qué composición, qué protagonista, qué figura queda fuera de los gestos |

Cada edición lleva una nota que dice quién la decidió, para no confundir la dirección del desarrollador con
las decisiones del dueño del espectáculo.

**Comprobaciones de la producción:**
- la secuencia se publica con el escritor nativo y se vuelve a guardar igual (*round trip*);
- el render definitivo se escribe donde xLights lo reproduce y se compara fotograma a fotograma con el render
  de la generación;
- la comprobación de producción prepara el paquete;
- un manifiesto lista cada archivo con su huella.

Nada se aprueba sin una persona.

## Defectos del motor encontrados y no corregidos

Corregirlos cambiaría el resultado de todos los niveles anteriores para las mismas entradas. Por eso se
documentan y se rodean con las palancas de arriba, como haría una persona:

1. **Un silencio inicial muy corto se come la introducción.** Si el audio empieza con un silencio más corto
   que un tramo mínimo, el planificador lo une al tramo siguiente. Ese tramo toma la sección del silencio
   (`SectionAt(start)` en `CompositionPlan.cpp`), así que toda la introducción queda marcada como silencio y la
   casa se apaga aunque haya música.
   - Arreglo de producción: una corrección de sección del paso 2 que empiece la introducción en 0 s.
   - Arreglo de código pendiente: usar la sección con más solape.
2. **Los gestos de toda la casa pueden tapar la letra.** En las composiciones de toda la casa, los gestos de
   Stage Field de una matriz van en capas por encima de su texto de letra.
   - Arreglo de producción: `exclude` de esa matriz en esas escenas.
   - Arreglo de código pendiente: tratar una figura con texto de letra como las caras que cantan, que nunca
     reciben un gesto encima.

## Lo que queda en manos de una persona

- **La revisión visual:** nada está aprobado hasta que una persona lo vea entero y lo diga.
- **La primera corrección real** del dueño del espectáculo, que se compara con el sistema de aprendizaje.
- **Las luces compartidas mal configuradas**, que se revisan físicamente en la instalación. Una prioridad de
  software no corrige el cableado.
