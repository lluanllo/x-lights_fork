# Fase 20 — Dirección artística adaptativa, motivos visuales y preparación de producción

Decisiones de diseño de la fase. Los resultados medidos, las cuatro versiones de la canción real de
prueba y las capturas están en el informe `JUAILILANDIA_REFERENCE_PACK/REPORTS/FASE20_DIRECCION_Y_PRODUCCION.md`,
que no se publica.

## El problema

La Fase 19 hizo coherente el color, pero sus motivos eran identidades cromáticas: un color que vuelve.
La auditoría del director de escena (Fase 17) encontró por qué el espectáculo seguía sonando a escenas
sueltas:
- cada escena elegía sus figuras por rotación («la que más ha descansado») y su gesto por el modo de la
  escena; un estribillo que vuelve recuperaba los colores, pero no las figuras ni el origen de su idea;
- las ondas locales y los destellos de varias figuras arrancaban todos a la vez, en cualquier escena;
- nada preparaba el punto culminante ni cerraba la canción; y una canción sin una parte marcada como
  clímax no tenía punto culminante;
- en los modos de grupo, ola u orquesta completa las figuras que no participaban seguían encendidas;
- `keep` de la lectura de Groq bloqueaba a la vez introducir, intensificar y transformar;
- los gestos se medían frente a la casa con un único umbral, fuera cual fuera su papel.

## Niveles (`ArtLevel`)

No hay un director nuevo: el mismo `DirectStage` toma un nivel, y cada nivel añade al anterior.

| Nivel | Qué añade |
|---|---|
| `f19` | nada: decide exactamente como en la Fase 19 (mismo `specHash`) |
| `direction` | un papel de cada escena en la canción, figuras que se responden, gestos tan visibles como su papel, la nueva semántica de `keep` |
| `motifs` | motivos visuales: una idea completa que se presenta y vuelve |
| `full` | el desarrollo: participación que crece y se reduce, punto culminante preparado, final que cierra |

Por defecto `full`, o el del espectáculo anterior. Se elige en el paso 1 y con `--art-direction`.

## El arco de la canción

Cada escena recibe un papel a partir de la estructura que ya existe (secciones, grupos de repetición,
prioridad, energía), nunca de una plantilla:
- la primera escena que suena presenta; la última cierra (si hay al menos tres);
- una parte que vuelve recupera; la parte que más vuelve es la que lleva la idea principal (un
  estribillo, si lo hay); la segunda parte que vuelve trae la idea de contraste;
- una parte marcada como clímax es el punto culminante; si la canción no marca ninguno, lo es el último
  bloque de la parte que vuelve, salvo que suene claramente más flojo que la primera vez;
- la escena anterior al punto culminante lo prepara; un instrumental o un puente transforma;
- `change` de la lectura de Groq permite transformar una parte, y `keep` lo evita: orientan, no mandan.

Una canción sin repeticiones no tiene nada que recuperar, y una sin punto culminante no lo prepara.

## Motivos visuales (`VisualMotif`)

Una idea, no un color: quién la lleva (protagonista y acompañantes), con qué gesto, desde qué punto del
escenario, en qué orden en el tiempo, con qué efecto en cada figura cuando se presenta, qué motivo de color
la viste, qué momentos la disparan, cómo entra y sale, sus restricciones (caras que cantan, figuras
bloqueadas) y cada escena que la usa con lo que hace con ella.

Operaciones: presentar, mantener, desarrollar (más figuras cada vez), intensificar (todas), reducir (a su
protagonista, antes del punto culminante), transformar (el mismo origen, otro gesto: unas ondas locales se
convierten en una onda que cruza la casa), repartir (unas llaman, otras responden), recuperar (con las
figuras que tuvo esa parte la primera vez) y resolver (pocas figuras, la luz se recoge).

Recuperar no es copiar: conserva la idea y puede cambiar participantes, gesto o intensidad.

## Atención y tiempo

- **Participación**: presentar con 3 figuras, preparar con 2, cerrar con 3, el contraste con 4, desarrollar
  suma una figura cada vez (hasta tres más), recuperar vuelve a las de la primera vez, y el punto
  culminante y la transformación llevan toda la casa. En las escenas con la atención en pocas (`focus`)
  las demás descansan de verdad.
- **No todo a la vez**: las figuras que comparten un golpe responden a la más cercana medio pulso
  después (como mucho tres pasos), salvo en el punto culminante, donde ir juntas es la intención. El
  orden sale del escenario y el tiempo de la música: no hay retardos al azar.
- **Punto culminante**: la escena anterior deja libres sus dos últimos compases para un barrido que crece
  hacia él; el punto culminante abre con una expansión desde donde nació la idea; la última escena la
  recoge, figura a figura, con poca luz.
- Cada gesto dice cómo se siguen sus figuras (`relation`: `unison`, `answer`, `propagation`,
  `sequence`, `overlap`, `continuation`, `anticipation`, `progressive-end`) y cuánto debe verse
  (`presence`).

## Color al servicio de la idea

- Un motivo visual se viste con el motivo de color de la escena donde nació: la idea puede extenderse sin
  cambiar de paleta.
- **Presencia de los gestos**: el color del gesto debe separarse de la mezcla de la casa según su papel:
  fondo 8 ΔE, respuesta 15, protagonista 22, punto culminante 28. En los dos últimos se prueban antes los
  colores con peso de la escena (acento, contraste, secundario) que su luz o el blanco; el blanco nunca
  va primero.
- **`keep`**: desde el nivel `direction` mantener la identidad mantiene el color principal, no congela la
  escena: una parte puede intensificarse o traer un contraste junto a su color. Para congelar más hay
  decisiones de la persona: `keep-main-colour` (el color principal de la canción se queda),
  `no-new-colours` (ninguna gama más que la escena anterior) y el bloqueo de paleta que ya existía.

## Composiciones reutilizables

La biblioteca «Composiciones y gestos» gana composiciones completas en tres niveles: una figura, un grupo
y toda la casa. Cada una conoce dónde encaja (contextos de uso, no plantillas), cuántas figuras necesita,
su gesto y su relación en el tiempo, los papeles de paleta que usa, su brillo, y de dónde viene. Están
hechas con los gestos y campos que ya existen: no hay motor nuevo. Son genéricas: ninguna sale de una
secuencia comercial.

La persona puede marcar favoritas, excluir, cambiar el brillo y el máximo de figuras, guardar una idea
visual de una canción como composición propia, y guardar como composición lo que construyó en su versión
corregida.

## Correcciones reales

`--composer-compare` clasifica cada cambio (cuestión de gusto, ajuste de tiempo, algo que no debía
encenderse), dice dónde empieza su escena, distingue los gestos añadidos o quitados, y propone como
composición las figuras que la persona hizo trabajar juntas (el mismo efecto en tres figuras o más en un
segundo, donde el espectáculo no lo tenía). Por cada cambio la persona elige:
- **aprenderla**: una señal de gusto (como una valoración: actúa con un segundo espectáculo);
- **solo esta canción**: una decisión de su escenario o de sus colores (dejar una figura fuera de una
  escena, meterla, quitar los gestos de una escena, el color de una figura);
- **preferencia general**: su propio valor desde ya, que se deshace como cualquier otro;
- **ignorarla**, o **rechazarla**, y entonces no se vuelve a proponer.

Nada se aprende sin elegirlo. No hay modelos entrenados: estructuras, estadísticas y preferencias
reversibles.

## Producción

`CheckProduction` (`--composer-production`) comprueba, sin encender controladores ni cambiar archivos:
- la canción y su duración, la letra, las partes y el montaje para el que se hizo;
- las figuras (pares que comparten luces), los controladores, las imágenes y vídeos (si existen y si
  viajan con el espectáculo);
- los gestos, recetas y paletas, los efectos (validador y efectos más allá del final) y los bloqueos;
- la revisión, un render nuevo con el renderizador de xLights y un guardado de vuelta por xLights que se
  vuelve a leer igual.

Cada hallazgo tiene un nivel: impide el espectáculo, puede hacer que salga mal, para revisar, o sugerencia
artística. Una advertencia estética nunca impide generar.

La carpeta de producción lleva:
- la secuencia y sus notas, el `.fseq`;
- las imágenes y vídeos que no están donde estará el espectáculo; los demás solo se nombran;
- la letra y la comprobación;
- la descripción del montaje (no el montaje), la versión de xLights, el hash de cada archivo y el estado
  de revisión.

Nunca se escribe sobre una carpeta anterior. El audio se nombra con su hash; no se copia salvo que se pida.

Los estados que ve la persona van separados: generación técnica correcta, render correcto, recursos
completos, guardado de nuevo por xLights sin cambios, y la revisión visual, que solo cambia una persona.

## La ventana

- **Paso 1**: «Desarrollo de las ideas» (completo, ideas que vuelven, solo dirección de escena, como en la
  fase 19).
- **Paso 2**: «Ideas visuales y su desarrollo»: cada idea, dónde aparece y qué hace cada escena con ella.
- **Paso 3**: «Composiciones y gestos» e «Ideas visuales».
- **Paso 4**: cada escena dice su papel en la canción, su idea, cuántas figuras la llevan, quién recibe
  los acentos, qué cambia y cómo sigue; se cambia y se deshace como antes.
- **Paso 5**: «Comprobar el espectáculo antes de sacarlo», en segundo plano (la ventana no se congela),
  con su informe y la carpeta de producción; y la versión corregida con una decisión por cambio.

## Límites

- Las recetas de cada figura dentro de un motivo se registran; su recuperación la hacen las recetas de la
  repetición (Fase 15), no el motivo.
- La lectura de Groq solo orienta qué partes transforman o mantienen; no se le pide más que en la
  versión 3.
- El reparto en la casa se decide por número de figuras y orden en el escenario; no se valora qué figura
  se ve mejor desde la calle.
- Nada de esto está aprobado: todo sigue `not-reviewed` hasta que una persona lo vea en xLights.
