# Quark Anticheat: sistema anticheat nativo para GNU/Linux

Prototipo funcional de un sistema anticheat nativo de nivel de kernel (Ring 0) diseñado específicamente para distribuciones GNU/Linux. El proyecto analiza y demuestra la viabilidad técnica de interceptar y prevenir vectores de ataque comunes en videojuegos multijugador competitivos sin comprometer la estabilidad del sistema ni la experiencia del usuario.

## Componentes del sistema

El sistema está estructurado en tres elementos principales que operan de forma coordinada:
* **Módulo de kernel**: desarrollado en Rust y C, se encarga de la monitorización en tiempo real de llamadas al sistema, accesos anómalos a memoria y supervisión de procesos activos mediante el framework LSM y programas eBPF.
* **Servicio en espacio de usuario**: actúa como un demonio (daemon) que gestiona la comunicación bidireccional con el kernel a través de canales estándar (`netlink`/`ioctl`), procesa los eventos de seguridad y aplica las políticas de mitigación.
* **Interfaz de integración (API/SDK)**: biblioteca orientada a desarrolladores que permite acoplar las capacidades de supervisión del anticheat en videojuegos de código abierto de manera flexible.

## Tecnologías y herramientas clave

* **Lenguajes de programación**: Rust (utilizando las abstracciones de *Rust for Linux*) y C (para componentes específicos sin bindings estables).
* **Mecanismos de seguridad del kernel**: Linux Security Modules (LSM) para el control de acceso y ganchos de mediación internos.
* **Telemetría y observabilidad**: eBPF (Extended Berkeley Packet Filter) para la ejecución segura de bytecode verificado en espacio de kernel.
* **Comunicación interna**: sockets netlink y llamadas ioctl para el intercambio de datos entre espacio de kernel y espacio de usuario.
* **Entorno de referencia**: Nobara Linux (distribución optimizada para gaming basada en Fedora).
* **Aislamiento y pruebas**: virtualización avanzada mediante QEMU/KVM para la ejecución segura y el debugging de código con privilegios elevados.

## Estructura del repositorio

El repositorio se organiza bajo una arquitectura modular de monorepo:
* `/kernel`: código fuente del módulo de kernel en Rust y C, incluyendo hooks LSM y especificaciones de telemetría eBPF.
* `/userspace`: implementación del servicio/demonio que orquesta las políticas de seguridad y la comunicación de eventos.
* `/sdk`: interfaces de programación, bindings y contratos de la API para la integración en videojuegos de prueba.
* `/tests`: suite de pruebas funcionales, simuladores de vectores de ataque (trampas comunes) y scripts de medición de overhead.

## Requisitos previos

Para configurar el entorno de desarrollo y compilación se requiere:
* un kernel moderno (versión 6.1 o superior con soporte estable para Rust).
* herramientas de compilación para el kernel (`make`, `gcc`, `clang`, `llvm`).
* cadena de herramientas de Rust configurada para compilación en espacio de kernel.
* entorno QEMU/KVM configurado para el despliegue seguro de imágenes de prueba.

## Autores

Proyecto desarrollado como Proyecto Integrador Final (PIF) en la carrera de Ingeniería en Sistemas de Información de la Universidad de la Cuenca del Plata:
* Martínez Alarcón, Gabriel Sebastián
* Merino De Rui, Stefano Nahuel
