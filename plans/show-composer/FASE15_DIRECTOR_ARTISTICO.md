# Fase 15 — Director artístico, recetas, variedad musical y vídeos

Decisiones de diseño de la fase. Los resultados medidos y la prueba real están en el informe
`JUAILILANDIA_REFERENCE_PACK/REPORTS/FASE15_DIRECTOR_ARTISTICO.md`, que no se publica.

## Música (15D)

- **El tempo era la causa de casi todo lo demás.** En Mi Burrito Sabanero el camino de tempo
  seguía el pulso a 105 BPM durante 90 s y luego a 207 BPM, porque la segunda mitad dobla la
  percusión. La rejilla de pulsos perdía la regularidad y ningún compás llegaba a la confianza
  mínima. Chiquetere caía a 64 BPM en los tramos suaves.
- **Un pulso por canción.** Un salto de octava cuesta mucho más en el Viterbi. Los tramos leídos a
  una octava de la lectura mayoritaria se vuelven a leer dentro de esa octava. Un cambio de tempo
  real que no sea una octava sigue como estaba (prueba B2 de 100 a 150 BPM).
- **La duda de la octava no se esconde.** Se guardan la lectura seguida, su mitad y su doble con
  su encaje. Una persona puede decir «el pulso va a la mitad / al doble». La corrección se aplica
  una sola vez aunque las correcciones se apliquen dos veces (caché y generación), gracias a
  `appliedTempoFactor`.
- **Compás.** Medio compás de desfase (acentos en 1 y 3) cuenta la mitad en la consistencia. La
  normalización es de 1,2 desviaciones en vez de 2. Las palmas sin acento siguen sin compás
  (prueba A2), y Chiquetere sigue sin compás porque no lo tiene claro.
- **Partes desde la letra.** Lo que se vuelve a cantar en otro sitio es el estribillo; lo que se
  canta una vez, verso; solo una parte única tardía entre estribillos es puente; una pausa sin
  palabras es interludio. Las líneas se comparan sin acentos ni puntuación, para que dos
  transcripciones algo distintas de la misma frase cuenten como iguales. Sin repeticiones no se
  cambia nada.

## Recetas (15A, 15B)

- **Se leen con el cargador de xLights**, nunca con un lector XML propio, sobre copias con los
  controladores inactivos.
- **Una escena empieza donde arrancan juntas tres o más familias**, como en el estudio de
  referencias. Una receta guarda hasta cinco familias, las que llevan la escena, con dos capas
  como mucho. Así las combinaciones vuelven a aparecer y tienen respaldo: con todas las familias,
  ninguna se repetía.
- **Solo ajustes portables.** Ajustes `E_` comprobados contra los metadatos y curvas activas con
  propiedad. No se llevan:
  - archivos, textos, pistas de tiempo ni caras;
  - un VU Meter que sigue marcas de tiempo, porque necesita una pista que la canción nueva no tiene
    (así lo comprueba xLights);
  - el estilo de búfer de un grupo, que se guarda como evidencia pero no pasa a la plantilla,
    porque la misma plantilla puede caer en una figura sola.
- **Una copia del mismo espectáculo no cuenta dos veces.** La huella es la de sus efectos, no la
  de sus bytes: la copia de Lion King apunta a otro audio.
- **Lo que decide una persona se conserva entre extracciones**: favoritas, apartadas, revisadas y
  corregidas. Una receta corregida no se pisa; solo crece su evidencia.
- **La autoría consta en cada receta.** Ninguna de las referencias es del padre: son paquetes
  comerciales que él usa en Juanjilandia.

## Director (15C) y color (15D)

- El director va encima del plan y de la dirección visual, sin duplicarlos.
- Cada escena tiene un paso del arco, una receta puntuada (energía, contexto, función,
  superficies presentes, confianza, favorita, variedad) con su explicación, y papeles por familia.
- La música que vuelve repite su receta y sus colores. Nunca descansa más de la mitad de la casa.
- **Los colores salen de la canción.** El ancla se toma de los colores que nombra la letra (local o
  Groq), o de la tonalidad sobre el círculo de quintas. Esto último es una convención y se dice así.
- **Dos tonos por canción**, como en las referencias: cada parte cambia cuál manda. El deslizador
  navideño acerca cada color al rojo, verde u oro más cercano.
- **El brief de Groq es opcional:**
  - solo se envía el texto de la letra, con consentimiento;
  - el modelo se elige de la lista que da el servicio;
  - la respuesta se comprueba y se guarda en caché.
  Sin él, el brief local de la letra.

## Vídeos (15E)

- **El render del compositor no era el de xLights.** `--headless` copia del escritorio la
  preferencia de descodificación de vídeo y la de GPU; los modos del compositor no lo hacían, y
  descodificaban con FFmpeg mientras xLights usaba Media Foundation. Ahora usan la misma función.
  Medido en cinco pantallas del layout real: el `.fseq` del compositor y el render de escritorio
  son idénticos.
- **En modo libre, las pantallas que la persona da a las imágenes no reciben la letra.** Antes la
  matriz principal elegida por la persona se quedaba sin vídeos porque la letra la ocupaba
  el 94 %.
- **Imágenes y vídeos en cualquier pantalla que elija una persona** (un submodelo de matriz, otra
  figura), con plantillas generales que solo se usan donde no encaja una específica.

## Evaluación (15F)

- La evaluación del espectáculo entero va en `VisualReview`, junto a la revisión plano a plano.
- Las medidas se muestran al lado, sin mezclarse con la opinión. La nota es subjetiva.
- Solo las observaciones que la persona marca se convierten en reglas, y como cualquier
  preferencia aprendida necesitan dos espectáculos para actuar. Una nota o una palabra sola no
  enseñan nada.
