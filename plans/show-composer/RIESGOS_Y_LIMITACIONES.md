# Riesgos y limitaciones — Juajilandia Show Composer

Probabilidad e impacto: **A** alto, **M** medio, **B** bajo. La evidencia está en `AUDITORÍA_TÉCNICA.md`.

## 1. Los cinco riesgos principales

1. **La sincronización automática de letras puede no ser suficientemente buena para bocas creíbles.** Sin Whisper y solo con análisis de energía, es probable alcanzar un nivel de frase, no de palabra. Por eso el marcado manual es de primer nivel y la calidad se medirá antes de prometer nada.
2. **Que el resultado sea correcto pero no bonito.** Es el riesgo central del producto y ninguna prueba automática lo detecta. Solo se reduce con revisiones visuales en cada fase y con tus shows como referencia.
3. **Sin `.xsq`, el motor de estilo está sin validar.** Todo el diseño del perfil parte de lo que me pediste y de una foto. Puede necesitar cambios al ver los datos reales.
4. **No se puede verificar macOS ni iPad desde este equipo.** Si la meta es contribuir a upstream, es un bloqueo real.
5. **La traducción automática tiene errores de sentido** que afectarán a una persona que depende del español. Hace falta una revisión humana.

## 2. Sincronización

| ID | Riesgo | P | I | Mitigación |
|---|---|---|---|---|
| S1 | El proveedor por defecto no alcanza precisión de palabra | A | A | Medir con pistas reales (§6 del plan); marcado manual; confianza visible |
| S2 | El filtro `VOCALS` es una extracción del canal central: mezclas con voz fuera del centro funcionan mal | A | M | `STEM_VOCALS` opcional; informe de baja confianza |
| S3 | `SeparateStems` descarga un modelo y tarda en CPU | A | M | Opción explícita con aviso; resultado reutilizable; sin descarga silenciosa |
| S4 | Whisper no está en tu build y reparte palabras por caracteres | A | M | Solo como anclas; proveedor opcional; compilar con CMake |
| S5 | Palabras alargadas ("Navidaaad") y coros | A | M | Reparto ajustado a sílabas y a la energía; la boca permanece abierta en la vocal; revisión manual |
| S6 | Varias voces simultáneas | M | M | MVP con una sola voz; el formato ya lo admite |
| S7 | Letra del usuario distinta de la cantada | M | M | El alineador nunca descarta texto; avisa si la cobertura es baja |
| S8 | Sin letra | — | — | No hay caras sincronizadas ni texto. Nunca se inventa |

### Fonemas

| ID | Riesgo | P | I | Mitigación |
|---|---|---|---|---|
| F1 | El diccionario CMU da pronunciación inglesa para palabras españolas | A | A | Letra `es`: omitir el diccionario |
| F2 | `BreakdownWord` omite palabras sin entrada sin avisar | A | A | Informar cada palabra sin fonemas |
| F3 | `¿`, `¡` y `Upper` por bytes impiden encontrar palabras acentuadas | A | M | Normalización previa |
| F4 | Solo hay 10 formas de boca, y los fonemas ingleses no son los españoles | M | B | Tabla de reglas revisable; es una limitación del sistema de caras |
| F5 | Otros idiomas | B | M | Sin soporte en la primera versión; se informa |

## 3. Audio

| ID | Riesgo | P | I | Mitigación |
|---|---|---|---|---|
| A1 | `DetectTempo` asume tempo constante | A | A | Seguidor nuevo de pulso variable, con pruebas sintéticas |
| A2 | Sin downbeats fiables | A | M | Confianza limitada; corrección manual; sincronización a pulso si dudosa |
| A3 | Estructuras no convencionales | M | M | Tipo `Unknown` válido; repetición de texto como evidencia |
| A4 | Narraciones sin pulso | M | M | `tempoIsStable=false`; el planificador usa frases y energía |
| A5 | Análisis distinto entre plataformas (44,1 kHz / 48 kHz; `-ffast-math`) | M | B | Caché de análisis; la garantía se define en la arquitectura |
| A6 | Reutilizar el `AudioManager` del reproductor rompería la reproducción | M | A | **Regla:** instancia privada |
| A7 | Formatos raros | B | M | Fallo limpio con mensaje |
| A8 | Memoria con canciones muy largas | B | M | Medir en Fase 2 |

## 4. Formatos y persistencia

| ID | Riesgo | P | I | Mitigación |
|---|---|---|---|---|
| D1 | `BuildDocument` borra datos desconocidos del `.xsq` | Seguro | A | **No usar el `.xsq` para metadatos propios** |
| D2 | `Save` no es atómico | B | A | Temporal, validar y renombrar |
| D3 | Sidecar perdido al mover o empaquetar | M | M | La pista de marcadores permanece; la secuencia sigue siendo válida |
| D4 | Parámetros de efectos que cambian entre versiones | M | M | Validar plantillas en cada prueba; `adjustSettings` solo actúa al cargar |
| D5 | Upstream avanza y hay conflictos | A | M | Código nuevo aislado; pocos puntos de contacto; integrar de upstream con regularidad |
| D6 | Layout y `.xsq` de distintas versiones | M | B | Tu layout es 2026.18 y el código 2026.20; las migraciones existentes lo cubren |

## 5. Rendimiento

| ID | Riesgo | P | I | Mitigación |
|---|---|---|---|---|
| R1 | Muchos efectos en grupos grandes alargan el render | M | M | Presupuestos en el perfil; preferir grupos a efectos por modelo |
| R2 | Vídeo en matrices | M | M | `MediaCompatibility`, avisos existentes de escala |
| R3 | 1.438 submodelos y 245 grupos al calcular capacidades | M | B | Copia en datos planos; medir en Fase 4 |
| R4 | Separar pistas es lento en CPU | A | M | Opcional, con progreso y cancelación ya existentes |
| R5 | Bloquear la interfaz | B | A | Solo la escritura final ocurre en el hilo principal |

## 6. Modelos

| ID | Riesgo | P | I | Mitigación |
|---|---|---|---|---|
| M1 | Roles mal deducidos (Custom = cualquier cosa) | A | M | El usuario confirma; `roles.json` |
| M2 | Efecto en grupo y en miembro a la vez | M | M | Regla en el planificador; verificar con `SupportsModelBlending` |
| M3 | Efectos que no se ven bien en ciertos modelos | A | M | Tabla inicial conservadora; ampliarla solo tras verlo |
| M4 | Modelos `No Controller` | A | B | Excluidos por defecto |
| M5 | Caras de imagen sin archivos en otro equipo | M | M | `CheckEffectSettings` lo informa |
| M6 | Grupos duplicados o heredados de vendedores | A | B | Propuesta de selección; el usuario puede excluir |

## 7. Traducción

| ID | Riesgo | P | I | Mitigación |
|---|---|---|---|---|
| T1 | `--update` elimina traducciones de cadenas retiradas | M | A | Confirmar en git antes; mejora propuesta: opción para conservar las obsoletas |
| T2 | Traducción automática de baja calidad | Seguro | M | Revisión; glosario |
| T3 | Abrir `OtherSettingsPanel` en wxSmith borra el selector de idioma | M | M | Actualizar el `.wxs` (cambio recomendado aparte) |
| T4 | Textos sin `_()` | A | M | Solo cubre el código nuevo; el resto se documenta |
| T5 | Mensajes del núcleo sin traducir | M | M | `ComposerMessage` y `N_()` |
| T6 | Nombres de pistas en español dentro de datos | M | B | IDs internos en sidecar; el núcleo no compara por nombre |

## 8. Recursos y licencias

| ID | Riesgo | P | I | Mitigación |
|---|---|---|---|---|
| L1 | Publicar shows comprados o música en el fork | M | A | **Nunca en el repositorio**; `.gitignore`; fixtures propios |
| L2 | Copiar secuencias enteras de terceros | M | A | Solo estadísticas y parámetros; `licenseNote` |
| L3 | Licencia GPLv3 del fork | Seguro | M | Mantener licencia y avisos; distribuir también el código |
| L4 | Licencias de modelos (HTDemucs, Whisper) | M | M | Revisar antes de empaquetar; no están en el repo |
| L5 | `xlights_rgbeffects.xml` tiene rutas personales | Seguro | B | No subirlo; está fuera del repositorio |
| L6 | Películas y música en vídeos | M | M | Decisión del propietario; el sistema solo referencia archivos |

## 9. Riesgos de romper secuencias

| ID | Riesgo | P | I | Mitigación |
|---|---|---|---|---|
| B1 | Sobrescribir una secuencia | B | A | Bloqueo en el código; prueba específica |
| B2 | Modificar referencias | B | A | Sin llamadas a `Save`; prueba de hash |
| B3 | Cambiar el layout o los controladores | B | A | Sin acceso; prueba de hash |
| B4 | Migraciones que tocan archivos | B | A | **No verificado en ejecución:** la Fase 1 comprueba el hash, y se trabajará sobre copias hasta entonces |
| B5 | Modelos eliminados de la instalación | M | M | `ScanForMissingModels`, ya existente |

## 10. Limitaciones aceptadas en la primera versión

- Una sola voz.
- Solo modelos con píxeles; sin DMX.
- Sin efectos de física, shader ni audio reactivo: lo primero por no ser deterministas.
- Sin interfaz en iPad.
- Español e inglés.
- Sin interpretación del significado de la letra: el sistema no sabe qué es un tren.
- Sin verificación en macOS.

## 11. Lo que no se ha verificado

| Afirmación | Cómo se verificará |
|---|---|
| Calidad de los algoritmos nuevos | Medición en Fases 2 y 3 |
| Efecto real de `AdjustEffectSettingsForVersion` sobre el disco | Prueba de hash en Fase 1 |
| Comportamiento visual de la tabla de fonemas | Revisión en Fase 3 |
| Interacción de grupos y miembros en render | Prueba en Fase 4 |
| Efectos adecuados en cada rol | Revisión en Fase 5 |
| Plugins VAMP | No se dependerá de ellos |
| Rendimiento real | Medición por fase |
