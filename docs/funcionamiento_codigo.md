# Funcionamiento del Código y Guía de Pruebas

Este documento detalla la estructura del prototipo de simulación de **Quark Anticheat**, su funcionamiento lógico, protocolo de comunicación y pasos para compilar, ejecutar y validar el sistema.

## 1. Diseño de la Simulación en Espacio de Usuario

Debido a que cargar controladores en el espacio del kernel (Ring 0) requiere privilegios de superusuario (`root/sudo`) y cabeceras de compilación específicas del kernel en ejecución (los cuales no están disponibles en entornos sandbox estándar de integración continua o agentes), este repositorio implementa un **prototipo funcional en espacio de usuario** que replica fielmente el comportamiento y flujo lógico del anticheat:

* **El Daemon de Quark (Rust):** Hace las veces del motor anticheat principal. En lugar de recibir eventos puramente desde el kernel, abre el archivo `/proc/<PID>/mem` del proceso del juego para leer directamente los valores de las variables protegidas en tiempo real.
* **El SDK de Quark (C):** Una librería estática ligera integrada en el videojuego. Realiza la conexión IPC con el Daemon e informa de forma proactiva cada cambio legítimo en las variables.
* **El Videojuego Mock (C):** Simula el cliente del juego. Registra variables críticas (`health` y `score`) y permite al jugador cambiarlas de forma permitida por medio de menús del juego. Para posibilitar la lectura/escritura de su memoria por parte de otros procesos sin requerir privilegios `root` (dado que Yama `ptrace_scope = 1` suele estar activo en Linux), el videojuego hace uso de la llamada del sistema `prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY)` al iniciar.
* **El Cheat (C):** Simula una herramienta externa de modificación de memoria. Abre `/proc/<PID>/mem` del videojuego y sobrescribe forzadamente el valor de la variable de vida para darle ventaja al jugador (ej. 9999 de vida).
* **El Módulo de Kernel (C - Referencia):** Ubicado en `/kernel`, se provee el código fuente C de un módulo de kernel real con hooks LSM y sockets Netlink. Este módulo ilustra cómo se bloquearía esta misma manipulación en producción a nivel Ring 0 interceptando la llamada de seguridad `ptrace_access_check`.

---

## 2. Protocolo de Comunicación (IPC)

La comunicación entre el SDK en el videojuego y el Daemon de Quark se realiza mediante **Unix Domain Sockets** (`/tmp/quark.sock`). Los datos se transfieren usando un protocolo binario simple y eficiente de bajo nivel:

### Estructura del Paquete
Cada mensaje consta de un encabezado de 8 bytes seguido de un payload variable:
1. `command` (4 bytes, Little/Native Endian u32)
2. `payload_len` (4 bytes, Little/Native Endian u32)
3. `payload` (de tamaño `payload_len` bytes)

### Comandos Soportados
* **`CMD_REGISTER_GAME` (1):** Registra el juego recién iniciado.
  * *Payload:* `pid` (4 bytes, `pid_t` / `i32`).
* **`CMD_REGISTER_VAR` (2):** Registra una dirección de memoria a monitorear.
  * *Payload:* `address` (8 bytes, `u64`), `size` (4 bytes, `u32`), `name` (32 bytes, string rellenada con ceros).
* **`CMD_UPDATE_VAR` (3):** Informa de un cambio de valor legítimo hecho por el código interno del juego.
  * *Payload:* `address` (8 bytes, `u64`), `new_value` (8 bytes, `u64`).

---

## 3. Compilación del Proyecto

El proyecto incluye un Makefile raíz que simplifica todo el proceso de construcción utilizando `cargo` para Rust y `gcc` para los componentes C.

Para compilar todo el sistema, simplemente ejecuta:
```bash
make
```

Esto generará:
1. El ejecutable del Daemon de Quark en: `quark_daemon/target/release/quark_daemon`
2. El cliente del Videojuego Mock en: `game_target/game`
3. El programa inyector Cheat en: `cheat/cheat`

Si necesitas limpiar los ejecutables y caché de compilación:
```bash
make clean
```

---

## 4. Guía de Pruebas Paso a Paso

Para validar la efectividad de Quark, necesitarás abrir **tres terminales** independientes:

### Paso 1: Iniciar el Daemon de Quark Anticheat
En la **Terminal 1**, ejecuta el daemon de Quark:
```bash
./quark_daemon/target/release/quark_daemon
```
*Verás un mensaje indicando que el Daemon está activo y escuchando en el socket `/tmp/quark.sock`.*

### Paso 2: Iniciar el Videojuego Mock
En la **Terminal 2**, inicia el videojuego:
```bash
./game_target/game
```
*El juego se conectará a Quark, registrará las variables de `health` y `score` imprimiendo sus direcciones de memoria virtual, y mostrará el menú del juego con su PID actual.*

### Paso 3: Interactuar de manera legítima
En la **Terminal 2** (el juego), presiona:
* `h` para recibir daño. Observarás que la vida disminuye.
* `s` para aumentar score.
* `p` para verificar el estado de las variables.

*Revisa la **Terminal 1** (Quark Daemon). Verás cómo Quark lee los valores iniciales y recibe las actualizaciones sin generar alertas, ya que el SDK está notificando los cambios legítimos en tiempo real.*

### Paso 4: Ejecutar el Cheat (Intento de Hack)
Identifica en la **Terminal 2** (juego) el **PID** y la **dirección de memoria de la variable `health`** (ej. address: `0x55555555c010`, PID: `12345`).

En la **Terminal 3**, ejecuta el programa de trampas pasándole el PID, la dirección de memoria en hexadecimal y el nuevo valor deseado:
```bash
./cheat/cheat <PID> <HEALTH_ADDRESS> <NEW_VALUE>
```
Ejemplo:
```bash
./cheat/cheat 12345 0x55555555c010 9999
```

### Paso 5: Analizar la Respuesta de Quark
* **En la Terminal 3 (Cheat):** Verás un mensaje indicando que la memoria se sobrescribió exitosamente: `[CHEAT] SUCCESS! Wrote 4 bytes.`
* **En la Terminal 1 (Quark Daemon):** En menos de 50 milisegundos, el hilo de monitoreo detectará que el valor en memoria de `health` es `9999` pero el valor esperado en su registro interno es `80` (o el último valor legítimo). Saltará la alerta de Quark y se ejecutará la acción de mitigación de inmediato:
  ```text
  ⚡⚡ [QUARK ALERT] TAMPERING DETECTED! ⚡⚡
  Variable Name:   health
  Memory Address:  0x55555555C010
  Expected Value:  80
  Actual Value:    9999
  ==================================================
  [QUARK ACTION] Terminating target process 12345 immediately...
  ```
* **En la Terminal 2 (Juego):** Verás que el juego se cierra abruptamente con el mensaje `Killed` o `Terminado (killed)`, impidiendo que la trampa surta efecto o continúe ejecutándose.

Esto demuestra de manera práctica y elegante cómo funciona la verificación de coherencia de memoria del sistema Quark Anticheat.

---

## 5. Pruebas Automatizadas (Scripts)

Para agilizar las verificaciones y no tener que abrir 3 terminales manualmente, se proveen dos scripts bash automatizados:

### A. Prueba Con Protección de Quark
El script [run_test.sh](file:///home/stefano/Quark-Anticheat/run_test.sh) automatiza el flujo completo descrito en la sección 4:
1. Compila todo.
2. Inicia el Daemon en segundo plano.
3. Inicia el juego en segundo plano.
4. Extrae la dirección de memoria dinámica de `health`.
5. Ejecuta el `cheat` para escribir `9999` en esa dirección.
6. Comprueba que el juego ha sido finalizado con éxito (`SIGKILL`) por Quark.
7. Muestra los logs del Daemon.

**Ejecución:**
```bash
./run_test.sh
```

### B. Prueba Sin Protección de Quark (Intento de Cheat Exitoso)
El script [run_test_no_quark.sh](file:///home/stefano/Quark-Anticheat/run_test_no_quark.sh) simula el ataque cuando **no** está activo el Daemon de Quark:
1. Compila todo.
2. Inicia el juego en segundo plano en modo desprotegido.
3. Extrae la dirección de memoria dinámica de `health`.
4. Ejecuta el `cheat` para escribir `9999` en esa dirección.
5. Envía un comando de impresión (`p`) simulado al juego.
6. Comprueba que el juego **sigue ejecutándose** y que el valor de la vida en memoria fue vulnerado exitosamente a `9999`.

**Ejecución:**
```bash
./run_test_no_quark.sh
```
*Verás en el log final del juego cómo la variable 'health' pasa a valer 9999 en memoria virtual, confirmando el éxito del hack en ausencia de Quark.*

