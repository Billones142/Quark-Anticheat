# **Quark Anticheat: análisis de viabilidad técnica e implementación**  **de un sistema anticheat nativo para GNU/Linux**

## 

## Universidad de la Cuenca del Plata

## 

## Facultad de Ingeniería y Tecnología

## 

## Ingeniería en Sistemas de Información

## 

## PROYECTO INTEGRADOR FINAL

# 

**Autores:** Martínez Alarcón, Gabriel Sebastián — Merino De Rui, Stefano Nahuel

**Mayo de 2026**

# 

# **Resumen** {#resumen}

El presente proyecto aborda una limitación técnica que afecta a los usuarios de distribuciones GNU/Linux que utilizan su equipo para jugar videojuegos en línea. Un conjunto significativo de títulos multijugador requiere, como condición obligatoria de ejecución, la presencia de un sistema anticheat2 de nivel de kernel3. Dado que estos sistemas se diseñan principalmente para Windows y operan en el nivel de máximo privilegio (Ring 0), resultan incompatibles con el modelo de kernel de Linux; en esta plataforma el anticheat, cuando existe, opera únicamente en espacio de usuario, lo que impide el acceso a dichos títulos con independencia de que el juego en sí sea compatible.

La solución propuesta consiste en el diseño y desarrollo de un sistema anticheat de nivel de kernel orientado a entornos GNU/Linux, compuesto por tres elementos principales: un módulo de kernel encargado de la supervisión de procesos, el acceso a memoria y la detección de patrones asociados a trampas; un servicio en espacio de usuario responsable de la comunicación con el videojuego y la gestión de políticas de seguridad; y una interfaz de integración (API4/SDK5) que permite incorporar el sistema en videojuegos de forma flexible. El desarrollo adopta un modelo en espiral con prácticas de SCRUM y utiliza Rust como lenguaje principal, dado que ofrece garantías de seguridad de memoria que reducen el riesgo de errores críticos en el contexto de módulos de kernel.

Los resultados esperados incluyen un prototipo funcional integrado con un videojuego de código abierto, la validación del sistema frente a escenarios simulados de trampas y la medición de su impacto en el rendimiento del sistema anfitrión. El proyecto constituye una contribución al estudio de mecanismos de seguridad en GNU/Linux y una demostración de la viabilidad técnica de implementar soluciones anticheat en dicha plataforma.

**Índice**

[**Resumen	2**](#resumen)

[**CAPÍTULO I — DEFINICIÓN DEL PROYECTO	8**](#capítulo-i-—-definición-del-proyecto)

[1.1. Origen del Proyecto	8](#1.1.-origen-del-proyecto)

[1.2. Misión, Visión y Objetivos	8](#1.2.-misión,-visión-y-objetivos)

[1.3. Necesidad o Problema que responde	10](#1.3.-necesidad-o-problema-que-responde)

[1.4. ODS asociadas y Diferenciales	10](#1.4.-ods-asociadas-y-diferenciales)

[1.5. Descripción breve del Sistema	11](#1.5.-descripción-breve-del-sistema)

[1.6. Descripción detallada del Sistema	12](#1.6.-descripción-detallada-del-sistema)

[**CAPÍTULO II — RELEVAMIENTO E INVESTIGACIÓN DE MERCADO	12**](#capítulo-ii-—-relevamiento-e-investigación-de-mercado)

[**CAPÍTULO III — ENTORNO Y DOMINIO DEL SISTEMA	13**](#capítulo-iii-—-entorno-y-dominio-del-sistema)

[3.1. Entorno del Sistema	13](#3.1.-entorno-del-sistema)

[3.2. Dominio del Sistema (Variables)	14](#3.2.-dominio-del-sistema-\(variables\))

[3.3. Alcance (del Sistema y del Proyecto)	15](#3.3.-alcance-\(del-sistema-y-del-proyecto\))

[3.4. Límites y Restricciones	16](#3.4.-límites-y-restricciones)

[**CAPÍTULO IV — MODELO DE NEGOCIOS DEL PROYECTO	17**](#capítulo-iv-—-modelo-de-negocios-del-proyecto)

[**CAPÍTULO V — PLANIFICACIÓN DEL PROYECTO	17**](#capítulo-v-—-planificación-del-proyecto)

[5.1. Definición de Etapas	17](#5.1.-definición-de-etapas)

[5.2. Entregables por etapa	18](#5.2.-entregables-por-etapa)

[5.3. Organización del Equipo	19](#5.3.-organización-del-equipo)

[5.4. Cronograma (Gantt)	20](#5.4.-cronograma-\(gantt\))

[5.5. Análisis PERT/CPM	22](#5.5.-análisis-pert/cpm)

[5.6. Estimación de Tamaño y Esfuerzo	23](#5.6.-estimación-de-tamaño-y-esfuerzo)

[Estimación SLOC por componente	23](#estimación-sloc-por-componente)

[Story Points del módulo de kernel	24](#story-points-del-módulo-de-kernel)

[Ajuste de complejidad COCOMO II	24](#ajuste-de-complejidad-cocomo-ii)

[Estimación del esfuerzo	25](#estimación-del-esfuerzo)

[5.7. Gestión de Riesgos	27](#5.7.-gestión-de-riesgos)

[5.8. Hitos de Control	29](#5.8.-hitos-de-control)

[5.9. Descripción del MVP	30](#5.9.-descripción-del-mvp)

[**CAPÍTULO VI — METODOLOGÍAS DE GESTIÓN	31**](#capítulo-vi-—-metodologías-de-gestión)

[6.1. Técnicas y Prácticas de Ingeniería de Software	31](#6.1.-técnicas-y-prácticas-de-ingeniería-de-software)

[6.2. Trazabilidad del Proyecto	31](#6.2.-trazabilidad-del-proyecto)

[6.3. Gestión de la Configuración	32](#6.3.-gestión-de-la-configuración)

[6.4. Testing del Proyecto	32](#6.4.-testing-del-proyecto)

[6.5. Indicadores de Seguimiento	32](#6.5.-indicadores-de-seguimiento)

[**CAPÍTULO VII — MARKETING DEL PROYECTO	33**](#capítulo-vii-—-marketing-del-proyecto)

[**CAPÍTULO VIII — PROPIEDAD INTELECTUAL	34**](#capítulo-viii-—-propiedad-intelectual)

[**CAPÍTULO IX — DISEÑO DE LA SOLUCIÓN	34**](#capítulo-ix-—-diseño-de-la-solución)

[**CAPÍTULO X — RECURSOS DEL PROYECTO	35**](#capítulo-x-—-recursos-del-proyecto)

[**CAPÍTULO XI — OPORTUNIDADES DEL PROYECTO	36**](#capítulo-xi-—-oportunidades-del-proyecto)

[**CAPÍTULO XII — LECCIONES APRENDIDAS DEL PROYECTO	36**](#capítulo-xii-—-lecciones-aprendidas-del-proyecto)

[**CAPÍTULO XIII — ENTREGABLES	36**](#capítulo-xiii-—-entregables)

[**Conclusiones del Proyecto	37**](#conclusiones-del-proyecto)

[**Bibliografía	38**](#bibliografía)

[**Anexos	40**](#anexos)

[**Notas al pie de página	40**](#notas-al-pie-de-página)

# 

# 

# 

# **CAPÍTULO I — DEFINICIÓN DEL PROYECTO** {#capítulo-i-—-definición-del-proyecto}

## **1.1. Origen del Proyecto** {#1.1.-origen-del-proyecto}

El proyecto surge a partir de la identificación de una limitación concreta que afecta a los usuarios de distribuciones GNU/Linux que utilizan sus equipos para jugar videojuegos en línea. Una parte significativa de los títulos multijugador más populares implementa sistemas anticheat de nivel de kernel como dependencia obligatoria de ejecución. Estos sistemas operan en el espacio del kernel (Ring 0), el nivel de máximo privilegio del procesador, lo que les otorga acceso amplio a los recursos del sistema para detectar modificaciones no autorizadas (Wikipedia, 2026a). Como se diseñaron principalmente para Windows, los usuarios de Linux quedan excluidos: en esta plataforma el anticheat, cuando se habilita a través de capas de compatibilidad como Proton, funciona en espacio de usuario y no mediante un controlador de kernel (GamingOnLinux, 2026).

El contexto de aplicación es a la vez técnico y social: técnico, porque la brecha existe por una limitación arquitectónica real entre los modelos de kernel de ambos sistemas operativos; social, porque afecta a una comunidad de usuarios en crecimiento que ha optado por plataformas libres y abiertas. El proyecto se enmarca en el contexto académico de la carrera de Ingeniería en Sistemas de Información de la Universidad de la Cuenca del Plata, donde representa una oportunidad de aplicar conocimientos de sistemas operativos, seguridad y desarrollo de software de bajo nivel sobre un problema con impacto real y verificable.

## **1.2. Misión, Visión y Objetivos** {#1.2.-misión,-visión-y-objetivos}

**Misión.** Demostrar, mediante el diseño y desarrollo de un prototipo funcional, la viabilidad técnica de un sistema anticheat de nivel de kernel nativo para GNU/Linux que preserve la integridad del entorno de ejecución de videojuegos sin degradar la experiencia del usuario.

**Visión.** Contribuir a reducir la brecha que excluye a los usuarios de GNU/Linux del ecosistema de videojuegos multijugador competitivos, aportando una base técnica abierta sobre mecanismos de seguridad a nivel de kernel en esta plataforma.

**Objetivo general.** Analizar, diseñar y desarrollar un sistema anticheat de nivel de kernel para distribuciones GNU/Linux, capaz de detectar y prevenir comportamientos maliciosos que comprometan la integridad del entorno de ejecución de videojuegos, evaluando su viabilidad técnica, su impacto en el rendimiento del sistema anfitrión y los riesgos de seguridad asociados al uso de privilegios de kernel.

**Objetivos específicos.**

1. Definir la arquitectura del sistema propuesto, produciendo un documento de diseño que cubra el módulo de kernel, el servicio en espacio de usuario y la interfaz de integración.  
2. Identificar y documentar un mínimo de cinco vectores de ataque comunes utilizados por software de trampas en videojuegos multijugador.  
3. Implementar una interfaz de integración (API/SDK) funcional, verificada mediante su incorporación en un videojuego de código abierto seleccionado como caso de prueba.  
4. Validar el sistema desarrollado mediante la ejecución de casos de prueba basados en los vectores de ataque identificados, registrando tasas de detección y falsos positivos para cada uno.  
5. Medir el impacto del sistema en el rendimiento del equipo anfitrión, cuantificando el overhead introducido en CPU y memoria RAM durante la ejecución del videojuego.

## **1.3. Necesidad o Problema que responde** {#1.3.-necesidad-o-problema-que-responde}

Los sistemas anticheat de nivel de kernel disponibles en el mercado se desarrollaron principalmente para Windows. Esta decisión de diseño los hace incompatibles con las distribuciones GNU/Linux, ya que dependen de interfaces internas del kernel de Windows que no tienen equivalente directo en Linux; las diferencias en los requisitos de firma de controladores, las políticas de módulos de kernel y la naturaleza descentralizada de las distribuciones complican el despliegue de controladores propietarios en Linux frente al modelo centralizado de Windows (Wikipedia, 2026a). Como resultado, los videojuegos que incorporan estos sistemas como dependencia obligatoria resultan inaccesibles para los usuarios de Linux, sin que exista actualmente una solución técnica equivalente para dicha plataforma.

El problema es de naturaleza técnica. Su causa radica en una incompatibilidad arquitectónica entre los mecanismos de kernel sobre los que se construyen los anticheats existentes y el modelo de kernel de Linux. No existe un componente organizacional o institucional que lo origine; es consecuencia directa de decisiones de desarrollo tomadas por la industria del videojuego orientadas al ecosistema Windows. El impacto, no obstante, trasciende lo técnico: la exclusión resultante afecta a un segmento de usuarios que ve limitado su acceso a productos de entretenimiento por razones ajenas a la capacidad de su hardware o a la compatibilidad del software en sí.

## **1.4. ODS asociadas y Diferenciales** {#1.4.-ods-asociadas-y-diferenciales}

**ODS asociadas.** El proyecto se vincula principalmente con el Objetivo de Desarrollo Sostenible 9 (Industria, Innovación e Infraestructura), en tanto promueve la innovación tecnológica y el desarrollo de infraestructura de software abierta y resiliente. De forma secundaria se vincula con el ODS 10 (Reducción de las desigualdades), al reducir la brecha de acceso que excluye a una comunidad de usuarios de una plataforma libre. *(Asociación propuesta, a validar en la cátedra de Proyecto Final de Grado.)*

**Diferenciales.** El sistema constituiría, en su categoría, una propuesta de anticheat de nivel de kernel concebido de forma nativa para GNU/Linux, frente al estado actual en el que las soluciones existentes operan en espacio de usuario sobre capas de compatibilidad (GamingOnLinux, 2026). Su segundo diferencial es la adopción de Rust como lenguaje principal, que aporta garantías de seguridad de memoria en un dominio históricamente dominado por C (Ojeda, 2025). El tercero es su orientación abierta y académica, frente al carácter cerrado y propietario de los anticheats comerciales.

## **1.5. Descripción breve del Sistema** {#1.5.-descripción-breve-del-sistema}

Un sistema anticheat es una solución de seguridad cuyo propósito es detectar y prevenir el uso de software de trampas en videojuegos, preservando la integridad del entorno de ejecución y garantizando condiciones de juego equitativas. El presente sistema constituye una implementación de este concepto a nivel de kernel, orientada a distribuciones GNU/Linux. Se compone de tres elementos que operan de forma coordinada: un módulo de kernel, un servicio en espacio de usuario y una interfaz de integración. El sistema opera en tiempo real durante la ejecución del videojuego, con privilegios elevados que le permiten acceder a información del sistema no accesible desde el espacio de usuario convencional.

## **1.6. Descripción detallada del Sistema** {#1.6.-descripción-detallada-del-sistema}

El **módulo de kernel** supervisa los procesos activos del sistema, los accesos a memoria y los patrones de comportamiento asociados a software de trampas. Al operar con privilegios de kernel, resiste técnicas de evasión que explotan las limitaciones del espacio de usuario. Ante la detección de comportamiento anómalo, genera eventos que comunica al servicio en espacio de usuario. Esta capacidad de observación se sustenta en interfaces del propio kernel: el framework de Linux Security Modules (LSM), que expone ganchos de mediación sobre operaciones internas como el acceso a inodos, tareas, archivos e IPC (The kernel development community, 2024a), y eBPF, una máquina virtual interna del kernel que ejecuta programas verificados para observabilidad y seguridad con bajo overhead (New Relic, 2025).

El **servicio en espacio de usuario** recibe los eventos generados por el módulo de kernel, aplica las políticas de seguridad definidas y gestiona la comunicación con el proceso del videojuego. Es el componente responsable de traducir los eventos de bajo nivel en acciones concretas, como la notificación al juego o el registro del incidente.

La **interfaz de integración** expone una API/SDK que permite a los desarrolladores de videojuegos incorporar el sistema en sus títulos de forma flexible, sin requerir conocimiento de los detalles internos del módulo de kernel ni del servicio subyacente.

# **CAPÍTULO II — RELEVAMIENTO E INVESTIGACIÓN DE MERCADO** {#capítulo-ii-—-relevamiento-e-investigación-de-mercado}

*Capítulo a desarrollar durante el cursado de Proyecto Final de Grado, conforme al ítem 24.3.2 del instructivo. Se prevén las siguientes secciones, con un relevamiento inicial parcial ya disponible.*

* **Fuentes de datos utilizadas, instrumentos y alcance.** Pendiente de instrumentación (encuestas a la comunidad de usuarios de GNU/Linux y a desarrolladores de videojuegos de código abierto).  
* **Presentación de datos y variables de análisis.** Pendiente.  
* **Análisis preliminar del panorama actual.** El relevamiento técnico inicial confirma que proveedores como Easy Anti-Cheat y BattlEye introdujeron soporte para Proton hace varios años, pero su funcionamiento en Linux se limita al espacio de usuario y no al nivel de kernel; además, la adopción por parte de los desarrolladores de juegos es baja porque el segmento de jugadores Linux se percibe como reducido (GamingOnLinux, 2026). Esto evidencia un nicho sin una solución nativa equivalente.  
* **Conclusiones.** Pendiente de completar tras el relevamiento formal.

# **CAPÍTULO III — ENTORNO Y DOMINIO DEL SISTEMA** {#capítulo-iii-—-entorno-y-dominio-del-sistema}

## **3.1. Entorno del Sistema** {#3.1.-entorno-del-sistema}

El sistema opera en equipos de escritorio o portátiles con distribuciones GNU/Linux instaladas, destinados a la ejecución de videojuegos en línea. El entorno de ejecución es heterogéneo: las distribuciones varían en versión de kernel, configuración de módulos habilitados, gestor de paquetes y políticas de seguridad activas. El sistema no asume una configuración uniforme del entorno.

Para el desarrollo y las pruebas se toma como distribución de referencia Nobara Linux, derivada de Fedora, que incorpora ajustes orientados al rendimiento en gaming. La validación en entornos alternativos se realiza mediante virtualización con QEMU/KVM6. El sistema opera de forma simultánea con el proceso del videojuego y con otros procesos del sistema operativo, por lo que el impacto en el rendimiento del equipo constituye una variable relevante del entorno que el diseño considera.

## **3.2. Dominio del Sistema (Variables)** {#3.2.-dominio-del-sistema-(variables)}

**Variables endógenas** (internas, bajo control directo del proyecto):

* **Arquitectura del módulo de kernel:** decisiones de diseño sobre qué interfaces del kernel se utilizan (LSM7, eBPF8, Netfilter9) y cómo se estructuran los mecanismos de detección.  
* **Políticas de seguridad:** criterios definidos por el sistema para clasificar un comportamiento como anómalo y determinar la acción correspondiente.  
* **Rendimiento del sistema:** overhead introducido por el módulo de kernel y el servicio en espacio de usuario, ajustable mediante decisiones de implementación.  
* **Cobertura de detección:** conjunto de técnicas de trampa que el sistema es capaz de identificar, determinado por los patrones implementados en el módulo de kernel.  
* **Interfaz de integración:** diseño y contrato de la API/SDK expuesta a los videojuegos, definido por el equipo de desarrollo.

**Variables exógenas** (externas, sin control directo del proyecto):

* **Versión y configuración del kernel de Linux del equipo anfitrión:** determina qué interfaces están disponibles y con qué comportamiento.  
* **Distribución GNU/Linux utilizada por el usuario:** afecta la disponibilidad de herramientas, módulos y configuraciones de seguridad por defecto.  
* **Evolución del ecosistema de soporte de Rust para módulos de kernel:** el nivel de madurez de los bindings disponibles puede variar durante el desarrollo. El soporte de Rust se incorporó al kernel principal en la versión 6.1 y dejó de considerarse experimental en 2025, aunque su cobertura de abstracciones internas sigue ampliándose de forma progresiva (Ojeda, 2025).  
* **Técnicas de trampa utilizadas por el software adversario:** el proyecto identifica y cubre un conjunto acotado; pueden surgir nuevas técnicas fuera del alcance.  
* **Videojuego seleccionado como caso de prueba:** sus características técnicas condicionan la forma de implementar y validar la interfaz de integración.

## **3.3. Alcance (del Sistema y del Proyecto)** {#3.3.-alcance-(del-sistema-y-del-proyecto)}

**El sistema incluye:**

* Un módulo de kernel para GNU/Linux que supervisa procesos, accesos a memoria y patrones de comportamiento asociados a trampas.  
* Un servicio en espacio de usuario que gestiona la comunicación entre el módulo de kernel y el videojuego, recolecta eventos y aplica políticas de seguridad.  
* Una interfaz de integración (API/SDK) que permite incorporar el sistema en videojuegos.  
* Un conjunto de casos de prueba ejecutados sobre un videojuego de código abierto.  
* Documentación del diseño, la implementación y los resultados obtenidos.

**Quedan fuera del alcance del proyecto:**

* El desarrollo de un videojuego propio.  
* La compatibilidad con sistemas operativos distintos de GNU/Linux.  
* La detección de trampas mediante hardware externo o modificaciones de firmware.  
* La distribución o comercialización del sistema (el resultado es un prototipo funcional con fines académicos e investigativos).  
* La integración con plataformas de distribución de videojuegos o con anticheats de terceros.

## **3.4. Límites y Restricciones** {#3.4.-límites-y-restricciones}

* **Técnicas:** el sistema opera sobre el kernel de Linux. Las interfaces disponibles para módulos de kernel (LSM, eBPF, Netfilter) establecen límites sobre los eventos interceptables y su granularidad. El soporte de Rust para módulos de kernel, si bien forma parte activa del ecosistema, presenta una madurez menor a la de C, y ciertas APIs internas pueden no contar con bindings estables (Ojeda, 2025). Las pruebas con privilegios elevados requieren entornos virtualizados para preservar la integridad de los equipos de desarrollo.  
* **Económicas:** el proyecto se desarrolla sin financiamiento externo. Los recursos disponibles son los equipos personales de los integrantes y herramientas de software libre o de acceso gratuito. No se contempla la contratación de infraestructura en la nube ni la adquisición de licencias.  
* **Temporales:** el proyecto está acotado a los plazos del plan de estudios para el Proyecto Integrador Final, lo que delimita la profundidad alcanzable en algunas funcionalidades.  
* **Organizacionales:** el equipo está compuesto por dos integrantes, lo que limita el trabajo paralelo y exige una distribución de tareas precisa. El proyecto está sujeto a las evaluaciones periódicas de los docentes de Gestión de Proyectos y Proyecto Final de Grado, que constituyen puntos de control obligatorios.

# **CAPÍTULO IV — MODELO DE NEGOCIOS DEL PROYECTO** {#capítulo-iv-—-modelo-de-negocios-del-proyecto}

*Capítulo a desarrollar conforme al ítem 24.3.4 del instructivo. Dado que el proyecto tiene fines académicos e investigativos y no contempla comercialización (ver Capítulo III), el modelo de negocios se planteará en clave de adopción y sostenibilidad del proyecto de código abierto.* Secciones previstas:

* **Definición de negocios** (actividades, mercado general y tecnología).  
* **Definiciones estratégicas:** visión y misión (ver Capítulo I).  
* **Análisis de rivalidad amplificada:** competencia actual (Easy Anti-Cheat, BattlEye, VAC y equivalentes), competidores potenciales y sustitutos, proveedores y clientes. *Relevamiento inicial en Capítulo II.*  
* **Mapeo de competencia.** Pendiente.

# **CAPÍTULO V — PLANIFICACIÓN DEL PROYECTO** {#capítulo-v-—-planificación-del-proyecto}

## **5.1. Definición de Etapas** {#5.1.-definición-de-etapas}

El proyecto se organiza en cuatro etapas secuenciales:

* **Etapa 1: Análisis técnico (meses 5 y 6).** Estudio de las interfaces de programación del kernel de GNU/Linux orientadas a la telemetría y monitorización de procesos en tiempo real. Identificación y clasificación de técnicas de trampa y vectores de ataque comunes en videojuegos multijugador. Modelización del sistema real para comprender las variables exógenas que afectan el rendimiento del juego, garantizando que la monitorización a nivel de kernel no introduzca latencia perceptible.  
* **Etapa 2: Diseño arquitectónico (meses 6 y 7).** Definición formal de la arquitectura del módulo, incluyendo la especificación de los hooks de seguridad y los puntos de intercepción dentro del espacio del kernel. Diseño del modelo de comunicación entre el espacio de usuario y el kernel. Elaboración de los diagramas de arquitectura que documenten dependencias entre componentes, interfaces expuestas y restricciones de seguridad.  
* **Etapa 3: Desarrollo del módulo (meses 7, 8, 9 y 10).** Codificación en Rust del módulo de kernel, el servicio en espacio de usuario y la interfaz de integración (API/SDK). Ejecución de pruebas unitarias y de integración en entornos controlados y aislados mediante QEMU/KVM, verificando el comportamiento ante escenarios de carga, condiciones de carrera y entradas malformadas.  
* **Etapa 4: Validación y soporte (meses 10 y 11).** Ejecución de pruebas funcionales del sistema completo sobre el videojuego de código abierto seleccionado, evaluando la eficacia en la detección y el impacto sobre el rendimiento en condiciones reales. Los resultados alimentan el ciclo siguiente del modelo en espiral, permitiendo identificar vectores no contemplados y ajustar los mecanismos de detección. Incluye la preparación de la documentación final y la defensa del PIF.

## **5.2. Entregables por etapa** {#5.2.-entregables-por-etapa}

| Etapa | Entregable principal |
| ----- | ----- |
| 1 — Análisis técnico | Documento de diseño preliminar (E1.4): evaluación de bindings de Rust por API, vectores de ataque clasificados, videojuego de prueba seleccionado y justificado |
| 2 — Diseño arquitectónico | Diagramas de arquitectura (E2.3): hooks, modelo de comunicación kernel↔usuario, dependencias e interfaces |
| 3 — Desarrollo del módulo | Módulo de kernel compilado y estable, servicio de espacio de usuario, API/SDK y suite de pruebas (E3.1–E3.4) |
| 4 — Validación y soporte | Informe de validación (tasas de detección y falsos positivos), medición de overhead de CPU/RAM, documentación final y defensa (E4.1–E4.4) |

## 

**Hitos**

| Hito | Descripción | Fecha estimada | Entregable asociado |
| :---- | :---- | :---- | :---- |
| H1 | Cierre de Etapa 1: análisis técnico completo | Fin de junio 2026 | Documento de diseño preliminar (E1.4) |
| H2 | Cierre de Etapa 2: arquitectura definida | Fin de julio 2026 | Diagramas de arquitectura (E2.3) |
| H3 | Módulo de kernel funcional en entorno virtualizado | Fin de agosto 2026 | Módulo kernel compilado y estable (E3.1) |
| H4 | Sistema completo integrado y probado | Fin de octubre 2026 | Suite de pruebas ejecutada (E3.4) |
| H5 | Validación sobre videojuego y documentación final | Mediados de noviembre 2026 | Informe final y defensa (E4.4) |

**Criterios de aprobación por hito.** H1: documento de diseño preliminar aprobado por la cátedra; ICC ≥ 80 %. H2: diagramas completos y revisados por ambos integrantes; desvío de esfuerzo ≤ 20 %. H3: el módulo carga sin errores en QEMU/KVM y registra al menos un evento de supervisión; 0 kernel panics no controlados. H4: al menos el 75 % de los casos de prueba aprobados; velocidad de SP sostenida. H5: integración con videojuego funcionando, documentación completa y defensa realizada en fecha.

## **5.3. Organización del Equipo** {#5.3.-organización-del-equipo}

El equipo está integrado por dos personas con roles diferenciados; ambas participan en todas las fases con distinto grado de responsabilidad según el área.

* **Martínez Alarcón, Gabriel Sebastián — líder de proyecto y analista.** Responsable de la planificación estratégica, el seguimiento del avance y la gestión de riesgos, además de participar activamente en el desarrollo.  
* **Merino De Rui, Stefano Nahuel — analista y desarrollador.** Responsable de la arquitectura técnica del módulo de kernel y de la implementación del código fuente, además de participar en la investigación y el relevamiento del dominio.

El equipo establece un régimen de comunicación asincrónica para el desarrollo y reuniones semanales de seguimiento para la revisión de hitos de control.

## **5.4. Cronograma (Gantt)** {#5.4.-cronograma-(gantt)}

El cronograma se estructura en cuatro etapas distribuidas entre mayo y noviembre de 2026, con un horizonte total de siete meses. Las etapas de análisis técnico y diseño arquitectónico se solapan parcialmente en junio. El desarrollo del módulo concentra la mayor parte del esfuerzo, de julio a octubre, con las pruebas corriendo en paralelo. La validación se inicia en octubre solapada con el cierre del desarrollo, dejando noviembre para pruebas finales, medición de rendimiento, documentación y defensa.

**Tabla I. Cronograma Gantt del proyecto**

| N° | Actividad | Dependencias | May | Jun | Jul | Ago | Sep | Oct | Nov |
| :---- | :---- | :---- | :---- | :---- | :---- | :---- | :---- | :---- | :---- |
| E1 | Etapa 1: Análisis técnico |  |  |  |  |  |  |  |  |
| E1.1 | Estudio de API Kernel | \- |  |  |  |  |  |  |  |
| E1.2 | Identificación y clasificación de técnicas de trampa | \- |  |  |  |  |  |  |  |
| E1.3 | Modelización del sistema y variables exógenas | E1.1, E1.2 |  |  |  |  |  |  |  |
| E1.4 | Producción del documento de diseño preliminar | E1.3 |  |  |  |  |  |  |  |
| E2 | Etapa 2: Diseño arquitectónico |  |  |  |  |  |  |  |  |
| E2.1 | Especificación de hooks de seguridad e intercepción | E1 |  |  |  |  |  |  |  |
| E2.2 | Diseño del modelo de comunicación kernel/usuario | E2.1 |  |  |  |  |  |  |  |
| E2.3 | Elaboración de diagramas de arquitectura | E2.2 |  |  |  |  |  |  |  |
| E3 | Etapa 3: Desarrollo del módulo |  |  |  |  |  |  |  |  |
| E3.1 | Implementación del módulo de kernel (Rust) | E2 |  |  |  |  |  |  |  |
| E3.2 | Implementación del servicio en espacio de usuario | E3.1 |  |  |  |  |  |  |  |
| E3.3 | Implementación de la API/SDK de integración | E3.2 |  |  |  |  |  |  |  |
| E3.4 | Pruebas unitarias e integración (QEMU/KVM) | E3.1 |  |  |  |  |  |  |  |
| E4 | Etapa 4: Validación y soporte |  |  |  |  |  |  |  |  |
| E4.1 | Integración con videojuego open source | E3 |  |  |  |  |  |  |  |
| E4.2 | Ejecución de casos de prueba y registro de resultados | E4.1 |  |  |  |  |  |  |  |
| E4.3 | Medición de impacto en rendimiento (CPU/RAM) | E4.1 |  |  |  |  |  |  |  |
| E4.4 | Documentación final y preparación de defensa | E4.2, E4.3 |  |  |  |  |  |  |  |

## **5.5. Análisis PERT/CPM** {#5.5.-análisis-pert/cpm}

El análisis PERT identifica trece actividades distribuidas en cuatro etapas, con duraciones estimadas mediante la ponderación de tiempos optimistas, más probables y pesimistas. El camino crítico recorre las actividades E1.1/E1.2 → E2.1/E2.2 → E3.1 → E4.1 → E4.2 → E4.4, con una duración total esperada de 19 semanas. Las actividades E1.3, E2.3 y E4.3 presentan holgura y no condicionan el plazo final. La actividad de mayor varianza es E3.1 (desarrollo del módulo de kernel, Var \= 2,25), lo que la identifica como el principal factor de riesgo sobre el cronograma.

**Tabla II. Tabla de tiempos PERT del proyecto** (tiempos en semanas)

| ID | Precedente | T. optimista | T. medio | T. pesimista | T. esperado | Varianza |
| :---- | :---- | :---- | :---- | :---- | :---- | :---- |
| E1.1 | \- | 1 | 2 | 4 | 2,2 | 0,25 |
| E1.2 | \-  | 1 | 2 | 4 | 2,2 | 0,25 |
| E1.3 | E1.1, E1.2 | 0,5 | 1 | 2 | 1,1 | 0,06 |
| E2.1 | E1.1, E1.2 | 1 | 2 | 4 | 2,2 | 0,25 |
| E2.2 | E1.1, E1.2 | 1 | 2 | 3 | 2,0 | 0,11 |
| E2.3 | E1.3 | 0,5 | 1 | 2 | 1,1 | 0,06 |
| E3.1 | E2.1, E2.2 | 5 | 8 | 14 | 8,5 | 2,25 |
| E3.2 | E2.1, E2.2 | 2 | 4 | 7 | 4,2 | 0,69 |
| E3.3 | E3.2 | 2 | 4 | 7 | 4,2 | 0,69 |
| E4.1 | E3.1, E3.3, E2.3 | 1 | 2 | 3 | 2,0 | 0,11 |
| E4.2 | E4.1 | 1 | 2 | 4 | 2,2 | 0,25 |
| E4.3 | E4.1 | 1 | 1,5 | 3 | 1,7 | 0,11 |
| E4.4 | E4.2, E4.3 | 1 | 2 | 3 | 2,0 | 0,11 |

**Figura 1\. Grafo PERT con CPM del proyecto.** *(Insertar el diagrama como lámina/figura numerada; el proyecto dura 19 semanas. El grafo original se incorpora como figura dentro de este capítulo.)*

## **5.6. Estimación de Tamaño y Esfuerzo** {#5.6.-estimación-de-tamaño-y-esfuerzo}

### **Estimación SLOC por componente** {#estimación-sloc-por-componente}

Para proyectos de complejidad similar en el ecosistema Linux, los rangos típicos son:

**Tabla IV. Estimación de SLOC por componente**

| Componente | Descripción | SLOC estimado |
| :---- | :---- | :---- |
| Módulo de kernel (C \+ Rust) | Supervisión de procesos, memoria, eBPF, hooks LSM | 3.000 – 4.500 |
| Servicio en espacio de usuario | Daemon, comunicación kernel↔juego, políticas | 2.000 – 3.000 |
| API/SDK de integración | Interfaz para videojuego, protocolo, bindings | 1.000 – 1.500 |
| Suite de pruebas | Casos de prueba, scripts, mocks | 800 – 1.200 |
| Total |  | 6.800 – 10.200 |

Valor central adoptado: **8.000 SLOC** (punto medio conservador, apropiado para un equipo de dos personas sin experiencia previa en el dominio).

### **Story Points del módulo de kernel** {#story-points-del-módulo-de-kernel}

Para el componente de kernel, que no se mide bien con SLOC puro, se aplica estimación por Story Points:

**Tabla V. Estimación en Story Points del módulo de kernel**

| Historia / Tarea | Complejidad | SP |
| :---- | :---- | :---- |
| Configuración del entorno de desarrollo de módulos kernel | Media | 5 |
| Implementación de hooks LSM básicos | Alta | 13 |
| Supervisión de procesos con eBPF | Muy alta | 21 |
| Detección de accesos anómalos a memoria | Muy alta | 21 |
| Canal de comunicación kernel ↔ userspace (netlink/ioctl) | Alta | 13 |
| Manejo de errores y estabilidad del módulo | Alta | 13 |
| Integración con Rust for Linux | Alta | 13 |
| Total módulo de kernel |  | 99 SP |

### **Ajuste de complejidad COCOMO II** {#ajuste-de-complejidad-cocomo-ii}

El modelo COCOMO II utiliza factores de escala y multiplicadores de esfuerzo (cost drivers) para ajustar la estimación (Boehm et al., 2000). Dado el perfil del proyecto, los factores más influyentes son:

**Tabla VI. Factores de costo COCOMO II**

| Factor | Descripción | Nivel | Multiplicador |
| :---- | :---- | :---- | :---- |
| RELY | Fiabilidad requerida | Alto (fallo \= kernel panic) | 1,10 |
| CPLX | Complejidad del producto | Muy alto (desarrollo de kernel) | 1,34 |
| ACAP | Capacidad del equipo | Bajo | 1,19 |
| PCAP | Experiencia en el dominio | Muy bajo | 1,29 |
| AEXP | Experiencia en la plataforma | Bajo | 1,13 |
| PLEX | Experiencia en el entorno | Bajo | 1,09 |
| LTEX | Experiencia en lenguaje/herramienta | Bajo (Rust for Linux, eBPF) | 1,20 |
| TOOL | Uso de herramientas de desarrollo | Nominal | 1,00 |
| SCED | Restricción de cronograma | Nominal | 1,00 |

**Factor de ajuste compuesto (EAF):**

EAF \= 1,10 × 1,34 × 1,19 × 1,29 × 1,13 × 1,09 × 1,20 × 1,00 × 1,00 ≈ **3,76**

Este valor refleja que el proyecto opera en un dominio de alta complejidad técnica con un equipo sin experiencia previa, lo que incrementa el esfuerzo respecto a un proyecto de software convencional.

### **Estimación del esfuerzo** {#estimación-del-esfuerzo}

Fórmula aplicada: **Esfuerzo \= A × (KSLOC)^B × EAF**, con A \= 3,0, KSLOC \= 8,0, B \= 1,12 y EAF \= 3,76.

Esfuerzo \= 3,0 × (8,0)^1,12 × 3,76 \= 3,0 × 10,27 × 3,76 ≈ **116 personas-mes**.

El modelo en su forma pura está calibrado para equipos industriales, por lo que para un proyecto académico de alcance acotado (prototipo) se aplica un factor de reducción de 0,15:

* Esfuerzo ajustado \= 116 × 0,15 ≈ **17,4 personas-mes**.  
* En horas (mes académico de 80 h/persona): 17,4 × 80 ≈ **1.392 horas-persona** totales.  
* Para 2 integrantes: ≈ **696 horas por persona**.  
* En 8 meses: ≈ 87 horas/persona/mes → aproximadamente **22 horas semanales por integrante**.

*Nota metodológica: las constantes A \= 3,0 y B \= 1,12 corresponden a la calibración del modo semi-detached del COCOMO intermedio (Boehm, 1981). La versión COCOMO II reemplaza el exponente fijo por uno derivado de cinco factores de escala y emplea constantes nominales A \= 2,94 y B \= 0,91 (Boehm et al., 2000). Se mantiene el cálculo original del proyecto y se deja constancia de esta distinción para una eventual recalibración.*

**Tabla VII. Resumen de estimación de esfuerzo**

| Métrica | Valor |
| :---- | :---- |
| SLOC total estimado | 8.000 |
| Story Points (módulo kernel) | 99 SP |
| EAF (factor de ajuste) | 3,76 |
| Esfuerzo total ajustado | ≈ 1.392 horas-persona |
| Esfuerzo por integrante | ≈ 696 horas |
| Duración del proyecto | 8 meses |
| Carga semanal estimada | ≈ 22 h/semana por integrante |

## **5.7. Gestión de Riesgos** {#5.7.-gestión-de-riesgos}

Se identifican siete riesgos relevantes, clasificados según naturaleza, probabilidad e impacto.

**Tabla III. Identificación de riesgos**

| ID | Riesgo | Categoría | Prob. | Impacto | Prioridad |
| :---- | :---- | :---- | :---- | :---- | :---- |
| R1 | Inestabilidad del módulo de kernel durante el desarrollo (kernel panics frecuentes) | Técnico | Alta | Alto | Crítica |
| R2 | Madurez insuficiente de los bindings de Rust for Linux para las APIs requeridas (LSM, eBPF) | Técnico | Alta | Alto | Crítica |
| R3 | Subestimación del esfuerzo en el módulo de kernel, afectando el cronograma | Planificación | Media | Alto | Alta |
| R4 | Incompatibilidad con versiones de kernel distintas a la de referencia (Nobara) | Técnico | Media | Medio | Media |
| R5 | Disponibilidad reducida de algún integrante por causas académicas o personales | Organizacional | Media | Alto | Alta |
| R6 | Limitaciones técnicas del videojuego open source que dificulten la integración con la API/SDK | Técnico | Baja | Medio | Media |
| R7 | Cambios en interfaces internas del kernel que invaliden implementaciones existentes | Externo | Baja | Alto | Media |

**Planes preventivos y contingentes**

* **R1 — Inestabilidad del módulo de kernel.** *Preventivo:* todo el desarrollo y prueba se realiza en entornos virtualizados (QEMU/KVM); se adopta una estrategia de incrementos mínimos y ninguna funcionalidad pasa a la siguiente iteración con kernel panics no controlados. *Contingente:* si la tasa de panics impide el avance, se reduce el alcance al conjunto mínimo de hooks (LSM básico) y se posponen las funcionalidades eBPF de mayor complejidad o se documentan como trabajo futuro.  
* **R2 — Madurez insuficiente de Rust for Linux.** *Preventivo:* en la Etapa 1 se evalúa explícitamente el estado de los bindings de Rust por API (LSM, eBPF, Netfilter), determinando qué se implementa en Rust y qué requiere C. Este enfoque es coherente con la naturaleza progresiva del soporte de Rust en el kernel, cuyas abstracciones se amplían de forma incremental (Ojeda, 2025). *Contingente:* los componentes sin bindings estables se implementan en C, manteniendo Rust donde sus garantías de seguridad de memoria son aplicables; la decisión por componente se documenta en E1.4.  
* **R3 — Subestimación del esfuerzo.** *Preventivo:* la estimación de 99 SP se revisa al cierre de cada iteración de la Etapa 3; si la velocidad sostenida cae por debajo de 6 SP/semana durante dos semanas consecutivas, se activa una revisión de alcance. *Contingente:* se define un conjunto de funcionalidades prescindibles (detección avanzada vía eBPF) que pueden eliminarse del prototipo sin comprometer la demostración de viabilidad.  
* **R4 — Incompatibilidad entre versiones de kernel.** *Preventivo:* el sistema declara la versión mínima de kernel soportada y prioriza interfaces más estables entre versiones (LSM) para maximizar la portabilidad. *Contingente:* las incompatibilidades detectadas en entornos alternativos se documentan como limitaciones conocidas; la validación sobre la distribución de referencia es suficiente para los objetivos académicos.  
* **R5 — Disponibilidad reducida de un integrante.** *Preventivo:* la distribución de tareas garantiza que ambos tengan conocimiento funcional de todos los componentes; las reuniones semanales reducen la dependencia crítica. *Contingente:* se renegocia el alcance con los docentes en la revisión de hito más próxima, priorizando el núcleo funcional (módulo de kernel y servicio de usuario).  
* **R6 — Limitaciones del videojuego seleccionado.** *Preventivo:* en la Etapa 1 se evalúan al menos dos candidatos según criterios técnicos (accesibilidad del código, arquitectura de proceso, builds de desarrollo, comunidad activa). *Contingente:* se evalúa un candidato alternativo y, como última instancia, se desarrolla un cliente de prueba mínimo para validar la interfaz.  
* **R7 — Cambios en interfaces internas del kernel.** *Preventivo:* el desarrollo se ancla a una versión de kernel específica declarada en la documentación, evitando interfaces experimentales o inestables. *Contingente:* si una actualización rompe funcionalidades, el entorno se fija en la versión estable al inicio de la Etapa 3 y no se actualiza hasta el cierre.

  ## **5.8. Hitos de Control** {#5.8.-hitos-de-control}

**Tabla VIII. Hitos de control**

## **5.9. Descripción del MVP** {#5.9.-descripción-del-mvp}

El Producto Mínimo Viable es un prototipo funcional del sistema anticheat de nivel de kernel para GNU/Linux compuesto por el módulo de kernel, el servicio en espacio de usuario y la interfaz de integración, capaz de operar end-to-end integrado con un videojuego de código abierto. El MVP demuestra la detección de al menos los vectores de ataque identificados y reporta el overhead introducido en CPU y memoria, validando la hipótesis central de viabilidad técnica.

# **CAPÍTULO VI — METODOLOGÍAS DE GESTIÓN** {#capítulo-vi-—-metodologías-de-gestión}

## **6.1. Técnicas y Prácticas de Ingeniería de Software** {#6.1.-técnicas-y-prácticas-de-ingeniería-de-software}

Se adopta el **modelo en espiral** como metodología principal, complementado con prácticas de **SCRUM** integradas en cada iteración. El modelo en espiral, propuesto por Boehm (1988), organiza el desarrollo en ciclos sucesivos guiados por el riesgo; cada ciclo atraviesa las fases de determinación de objetivos, identificación y resolución de riesgos, desarrollo y validación, y planificación de la siguiente iteración.

La elección responde a la naturaleza del dominio. El desarrollo de un módulo de kernel exige el rigor estructural y la trazabilidad que los enfoques puramente ágiles no garantizan, mientras que la naturaleza evolutiva de las amenazas en ciberseguridad hace inviable un modelo lineal como el cascada, cuyos requisitos deben estar completamente definidos desde el inicio. El enfoque de Boehm es especialmente adecuado para proyectos complejos en los que los requisitos evolucionan y los desafíos técnicos son poco predecibles (Boehm, 1988). La incorporación de prácticas de SCRUM —marco de trabajo iterativo e incremental para el desarrollo de productos complejos (Schwaber & Sutherland, 2020)— en la organización interna de cada iteración agrega adaptabilidad y control sobre el avance, conformando un enfoque híbrido que balancea rigor y flexibilidad.

## **6.2. Trazabilidad del Proyecto** {#6.2.-trazabilidad-del-proyecto}

El repositorio del proyecto (gestionado por Merino De Rui) actúa como registro continuo del avance real, vinculando los commits con las actividades del Gantt y permitiendo auditar el progreso en cualquier momento. El registro semanal de avance de cada integrante contrasta horas y tareas completadas contra el cronograma, alimentando los indicadores de cumplimiento de cronograma y desvío de esfuerzo.

## **6.3. Gestión de la Configuración** {#6.3.-gestión-de-la-configuración}

La gestión de la configuración se apoya en el sistema de control de versiones del repositorio, que conserva el historial de cambios del módulo de kernel, el servicio de usuario, la API/SDK y la suite de pruebas. El desarrollo se ancla a una versión de kernel específica declarada en la documentación técnica, evitando interfaces experimentales o inestables, lo que preserva la reproducibilidad del entorno (ver R7, Capítulo V).

## **6.4. Testing del Proyecto** {#6.4.-testing-del-proyecto}

Las pruebas se ejecutan en entornos controlados y aislados mediante QEMU/KVM. Se aplican pruebas unitarias y de integración durante la Etapa 3 (E3.4), verificando el comportamiento del módulo ante escenarios de carga, condiciones de carrera y entradas malformadas; y pruebas funcionales del sistema completo durante la Etapa 4 (E4.2) sobre el videojuego de código abierto, registrando tasas de detección y falsos positivos para cada vector de ataque. El uso de eBPF como tecnología de observación facilita la instrumentación con bajo overhead durante estas pruebas (New Relic, 2025).

## **6.5. Indicadores de Seguimiento** {#6.5.-indicadores-de-seguimiento}

Se definen cinco indicadores distribuidos en las dimensiones de tiempo, costo y calidad.

1. **Cumplimiento de Cronograma (ICC) — Tiempo.** ICC \= (Actividades completadas en fecha / Actividades planificadas) × 100\. Meta: ≥ 80 % en cada punto de control. Frecuencia: al cierre de cada etapa.  
2. **Velocidad de Desarrollo en Story Points — Tiempo/Esfuerzo.** Velocidad \= SP completados / semanas transcurridas en E3. Meta: ≥ 6 SP/semana promedio. Frecuencia: semanal durante la Etapa 3\.  
3. **Desvío de Esfuerzo (DE) — Costo.** DE \= ((Horas reales acumuladas − Horas planificadas acumuladas) / Horas planificadas acumuladas) × 100\. Meta: ≤ ±20 % en cada punto de control. Frecuencia: mensual.  
4. **Tasa de Detección en Pruebas (TDP) — Calidad.** TDP \= (Casos de prueba aprobados / Casos ejecutados) × 100\. Meta: ≥ 75 % al cierre de E4.2. Frecuencia: por suite de pruebas.  
5. **Estabilidad del Módulo de Kernel — Calidad.** Cantidad de kernel panics o fallos críticos durante las sesiones de prueba en QEMU/KVM. Meta: 0 kernel panics no controlados al cierre de E3.4. Frecuencia: por sesión de prueba.

El seguimiento se complementa con una reunión de sincronización semanal y una revisión formal por hito, en la que se miden todos los indicadores, se comparan contra las metas y se decide continuar, ajustar o recuperar el plan. Los resultados de cada revisión se documentan como insumo para el Capítulo XII (Lecciones Aprendidas).

# **CAPÍTULO VII — MARKETING DEL PROYECTO** {#capítulo-vii-—-marketing-del-proyecto}

*\[A completar\] conforme al ítem 24.3.7. Por el carácter académico y de código abierto del proyecto, las secciones se orientarán a difusión y adopción comunitaria:* descripción de producto/experiencia; dinámicas de intercambio por perfil de interesado (usuarios de GNU/Linux, desarrolladores de videojuegos open source, comunidad de seguridad); estrategias de difusión e iniciativas de engagement (publicación del repositorio, documentación técnica, divulgación en comunidades de gaming en Linux); y canales a utilizar. *Pendiente de desarrollo.*

# **CAPÍTULO VIII — PROPIEDAD INTELECTUAL** {#capítulo-viii-—-propiedad-intelectual}

*\[A completar\] conforme al ítem 24.3.8.* Secciones previstas: definición de marca y activos a proteger (el nombre "Quark Anticheat" y el logotipo asociado); definición de las clases internacionales aplicables; y resultados de las búsquedas en la plataforma del INPI. Dado el enfoque de código abierto, se evaluará además la selección de una licencia de software libre compatible con el desarrollo de módulos de kernel (por ejemplo, GPL, requerida por el ecosistema del kernel de Linux). *Pendiente de desarrollo.*

# **CAPÍTULO IX — DISEÑO DE LA SOLUCIÓN** {#capítulo-ix-—-diseño-de-la-solución}

*\[A completar\] profundizar durante la Etapa 2 (diseño arquitectónico). Se dispone del esquema arquitectónico general; el diseño detallado, el modelo de datos y la documentación de interfaces son entregables de E2.3.*

* **Arquitectura del sistema.** El sistema se estructura en tres componentes coordinados (ver Capítulo I): un módulo de kernel, un servicio en espacio de usuario y una interfaz de integración (API/SDK). El módulo de kernel se apoya en interfaces nativas del kernel de Linux para la observación y mediación de operaciones sensibles: el framework LSM, que provee ganchos sobre operaciones internas del kernel y es la base de mecanismos de control de acceso como SELinux y AppArmor (The kernel development community, 2024a); y eBPF, que permite ejecutar programas verificados en el espacio del kernel para monitoreo de llamadas al sistema y actividad de procesos con bajo overhead (New Relic, 2025).  
* **Modelo de comunicación.** El canal entre el espacio de kernel y el espacio de usuario se diseña sobre mecanismos estándar (netlink/ioctl), determinando los formatos de transferencia de datos y señalización (entregable E2.2).  
* **Entornos de trabajo.** Desarrollo y pruebas sobre Nobara Linux como distribución de referencia, con validación en entornos virtualizados QEMU/KVM. *Modelo de datos, infraestructura y diseño detallado: pendientes de E2.3.*

# **CAPÍTULO X — RECURSOS DEL PROYECTO** {#capítulo-x-—-recursos-del-proyecto}

* **Recursos humanos.** Dos integrantes con roles diferenciados (ver Capítulo V, sección 5.3). El esfuerzo estimado asciende a ≈ 696 horas por integrante a lo largo de 8 meses (≈ 22 h/semana), según la estimación COCOMO II ajustada (sección 5.6).  
* **Recursos físicos y materiales.** Equipos personales de los integrantes (escritorio/portátil) destinados al desarrollo y a la ejecución de máquinas virtuales de prueba.  
* **Recursos financieros.** El proyecto se desarrolla sin financiamiento externo. No se contempla la contratación de infraestructura en la nube ni la adquisición de licencias (ver restricciones económicas, Capítulo III).  
* **Recursos tecnológicos.** Lenguaje principal Rust, con C para los componentes sin bindings estables; interfaces del kernel LSM, eBPF y Netfilter; virtualización QEMU/KVM; distribución de referencia Nobara Linux; sistema de control de versiones para la trazabilidad y la gestión de la configuración. Todas las herramientas son de software libre o de acceso gratuito.

# **CAPÍTULO XI — OPORTUNIDADES DEL PROYECTO** {#capítulo-xi-—-oportunidades-del-proyecto}

*\[A completar\] conforme al ítem 24.3.11.* Secciones previstas: **CANVAS del proyecto** (a completar) y **oportunidades futuras de escalabilidad e innovación**. Entre estas últimas se anticipan: la ampliación de la cobertura de detección a nuevos vectores de ataque; el soporte multi-distribución más allá de la distribución de referencia; la migración progresiva de componentes de C a Rust a medida que maduran los bindings del kernel (Ojeda, 2025); y la exploración de detección basada en comportamiento del lado del servidor como complemento a la observación a nivel de kernel.

# **CAPÍTULO XII — LECCIONES APRENDIDAS DEL PROYECTO** {#capítulo-xii-—-lecciones-aprendidas-del-proyecto}

*\[A completar\] al cierre del proyecto, con insumos de cada revisión por hito (ver sección 6.5).* Estructura prevista: **aspectos positivos** (decisiones de diseño acertadas, prácticas de gestión que funcionaron) y **aspectos a mejorar** (estimaciones, organización, dependencias técnicas). *Pendiente de desarrollo.*

# **CAPÍTULO XIII — ENTREGABLES** {#capítulo-xiii-—-entregables}

**Código fuente del proyecto.** Módulo de kernel (C \+ Rust), servicio en espacio de usuario, interfaz de integración (API/SDK) y suite de pruebas, alojados en el repositorio del proyecto.

**Documentación.**

* Documento de diseño preliminar (E1.4), incluyendo la evaluación de bindings de Rust por API, los vectores de ataque clasificados y la justificación del videojuego de prueba.  
* Diagramas de arquitectura (E2.3).  
* Prototipo funcional integrado con un videojuego de código abierto, verificando la operación end-to-end (MVP).  
* Informe de validación con tasas de detección y falsos positivos por vector de ataque.  
* Medición del impacto en el rendimiento del equipo anfitrión (overhead de CPU y memoria RAM).  
* Documentación final y material de defensa del PIF.

# **Conclusiones del Proyecto** {#conclusiones-del-proyecto}

*A completar al cierre del proyecto.* Recogerá los resultados de la validación del prototipo frente a la hipótesis de viabilidad técnica, la valoración del impacto en el rendimiento del sistema anfitrión y una reflexión sobre los riesgos de seguridad asociados al uso de privilegios de kernel, en línea con el objetivo general (Capítulo I).

# **Bibliografía** {#bibliografía}

Boehm, B. W. (1981). *Software engineering economics*. Prentice-Hall.

Boehm, B. W. (1988). A spiral model of software development and enhancement. *Computer, 21*(5), 61–72. [https://doi.org/10.1109/2.59](https://doi.org/10.1109/2.59)

Boehm, B. W., Abts, C., Brown, A. W., Chulani, S., Clark, B. K., Horowitz, E., Madachy, R., Reifer, D. J., & Steece, B. (2000). *Software cost estimation with COCOMO II*. Prentice Hall.

GamingOnLinux. (2026, mayo). *Anti-cheat check — which competitive games actually work on Linux?* [https://www.gamingonlinux.com/guides/view/anticheat-check-which-competitive-games-actually-work-on-linux/](https://www.gamingonlinux.com/guides/view/anticheat-check-which-competitive-games-actually-work-on-linux/)

Kaspersky. (2020, 11 de diciembre). *The secret world of malware-like cheats in video games.* Kaspersky Daily. [https://www.kaspersky.co.uk/blog/malware-like-cheats/16906/](https://www.kaspersky.co.uk/blog/malware-like-cheats/16906/)

New Relic. (2025, 3 de junio). *What is eBPF, and why does it matter for observability?* [https://newrelic.com/blog/observability/what-is-ebpf](https://newrelic.com/blog/observability/what-is-ebpf)

Ojeda, M. (2025, 12 de diciembre). *rust: conclude the Rust experiment* \[Mensaje en lista de correo\]. Linux Kernel Mailing List. [https://lkml.iu.edu/hypermail/linux/kernel/2512.1/06411.html](https://lkml.iu.edu/hypermail/linux/kernel/2512.1/06411.html)

Schwaber, K., & Sutherland, J. (2020). *The Scrum Guide: The definitive guide to Scrum — The rules of the game.* [https://scrumguides.org/](https://scrumguides.org/)

The kernel development community. (2024a). *Linux Security Modules: General security hooks for Linux.* The Linux Kernel documentation. [https://www.kernel.org/doc/html/latest/security/lsm.html](https://www.kernel.org/doc/html/latest/security/lsm.html)

The kernel development community. (2024b). *Rust.* The Linux Kernel documentation. [https://www.kernel.org/doc/html/latest/rust/](https://www.kernel.org/doc/html/latest/rust/)

Wikipedia. (2026a). *Kernel-level anti-cheat.* [https://en.wikipedia.org/wiki/Kernel-level\_anti-cheat](https://en.wikipedia.org/wiki/Kernel-level_anti-cheat)

Wikipedia. (2026b). *Easy Anti-Cheat.* [https://en.wikipedia.org/wiki/Easy\_Anti-Cheat](https://en.wikipedia.org/wiki/Easy_Anti-Cheat)

# 

# **Anexos** {#anexos}

*Las hojas de los anexos se numeran de forma independiente, indicando "página X de Y".*

* **Anexo I — Datos relevados.** *Pendiente (relevamiento del Capítulo II).*  
* **Anexo II — Documentos del entorno o dominio del Proyecto.** Incluye, entre otros, el grafo PERT con CPM (Figura 1\) y las tablas de detalle del cronograma.  
* **Anexo III — Otros Anexos.** *Según corresponda.*

  # **Notas al pie de página**  {#notas-al-pie-de-página}

1. GNU/Linux es un sistema operativo libre basado en el kernel Linux, creado por Linus Torvalds en 1991 e integrado con herramientas del proyecto GNU. Su código abierto permite acceder, modificar y distribuir el software. ↩

2. Un sistema anticheat es un conjunto de herramientas diseñadas para detectar y prevenir trampas en videojuegos competitivos, como bots, aimbots y modificaciones no autorizadas. ↩

3. El kernel es el núcleo central del sistema operativo que gestiona la comunicación entre el hardware y las aplicaciones de software. ↩

4. Una API (Interfaz de Programación de Aplicaciones) define las reglas para que diferentes software se comuniquen entre sí. ↩

5. Un SDK (Kit de Desarrollo de Software) proporciona herramientas, librerías y documentación para implementar esa comunicación, facilitando la integración de funcionalidades externas en nuevas aplicaciones. ↩

6. QEMU/KVM es una solución de virtualización que combina QEMU (emulador de código abierto) con KVM (aceleración por hardware del kernel Linux). Permite ejecutar múltiples máquinas virtuales simultáneamente con rendimiento cercano al nativo. ↩

7. LSM (Linux Security Modules) es un framework de seguridad que proporciona ganchos en el kernel para insertar módulos de control de acceso, base de implementaciones como SELinux y AppArmor. ↩

8. eBPF (Extended Berkeley Packet Filter) es una máquina virtual dentro del kernel que ejecuta bytecode verificado para monitoreo, análisis de tráfico y seguridad, cargable en tiempo de ejecución. ↩

9. Netfilter es un framework del kernel que intercepta y procesa paquetes de red, base de firewalls como iptables y nftables. ↩  
   