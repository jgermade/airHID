# airHID — Arquitectura

Transporte inalámbrico de dispositivos HID (teclado, ratón, gamepad) basado en
ESP32. Sin infraestructura WiFi: enlace directo ESP-NOW. Sin vídeo, solo input.

> Documento vivo. Las decisiones abiertas están marcadas con **❓** y
> resumidas en [Preguntas abiertas](#preguntas-abiertas).

---

## Objetivos

### Principal

- **O1.** Llevar teclado, ratón y gamepad USB a otro equipo sin cables, con
  latencia comparable al cable. ❓ Presupuesto: p. ej. ≤ 5 ms p99 extremo a
  extremo. ❓ Alcance: misma habitación (~10 m).

### Funcionales

| # | Objetivo |
|---|---|
| O2 | **km-client**: capturar teclado + ratón USB reales (Ducky One 2 SF + Logitech Bolt) vía hub |
| O3 | **dongle-server**: aparecer ante el PC como HID composite (teclado + ratón + gamepad) sin drivers |
| O4 | Varios servidores por cliente, cambio de target por hotkey (KVM sin vídeo) |
| O5 | Varios clientes sincronizados: cambiar target en uno los cambia todos ❓ imprescindible o deseable |
| O6 | **gamepad-client**: mandos USB inalámbricos hacia PC o consola |
| O7 | **espnow-retro**: mandos Bluetooth y ESP-NOW hacia consolas retro (equivalente a BlueRetro) |
| O8 | Remapeo configurable por mando y destino, persistente en NVS |

### No funcionales

| # | Objetivo |
|---|---|
| O9 | Seguridad: nadie puede espiar ni inyectar pulsaciones ❓ autenticar o también cifrar |
| O10 | Robustez: fallo seguro (teclas sueltas al perder enlace), reconexión automática, estado visible |
| O11 | Autonomía del km-client con powerbank > 20 h |
| O12 | Configuración mínima: emparejar una vez y funcionar al arrancar |
| O13 | Código modular compartido entre todos los firmwares |

### Secundarios / futuros

O14 pantalla SSD1306 · O15 control RGB del Ducky · O16 ejes analógicos ·
O17 variante nRF24 para entornos congestionados.

### Fuera de alcance

Vídeo/audio, funcionamiento sobre red WiFi o Internet, clientes software.

### MVP

O1 + O2 + O3 (solo teclado y ratón) + O9 + O10 + O12, con un cliente y un
servidor.

---

## Dispositivos

```
[km-client]                  [gamepad-client]             mando Bluetooth
USB Host → teclado/ratón     USB Host → mandos USB        (DS4, Switch Pro, Xbox…)
ESP-NOW TX · BLE pairing     ESP-NOW TX · BLE pairing             │
SSD1306                      LED estado                           │ BT
      │                            │                              │
      └──────────────┬─────────────┴──────────────┐               │
                     │ ESP-NOW                    │ ESP-NOW       │
              [dongle-server]                [espnow-retro] ◄─────┘
              ESP-NOW RX                     ESP-NOW RX + BT
              USB HID composite → PC         → puerto de mando de consola
```

### km-client

| Componente | Función |
|---|---|
| ESP32-S3 Zero | MCU, USB Host, ESP-NOW, BLE |
| Hub USB FE1.1S | Teclado + dongle Bolt simultáneos |
| Powerbank (FM5324HJ1) | 5 V, carga USB-C |
| SSD1306 128x64 | Estado por I2C |
| Adaptador USB-UART | Consola de depuración (ver [Notas de hardware](#notas-de-hardware)) |

### gamepad-client

| Componente | Función |
|---|---|
| **ESP32-S3 Zero** | MCU, USB Host, ESP-NOW, BLE |
| Hub USB FE1.1S | Varios mandos USB |
| LED | Estado |
| Motores de vibración | Rumble (futuro) |

> El ESP32-C3 **no sirve**: no tiene USB OTG (solo USB Serial/JTAG de función
> fija), así que no puede ser USB Host ni con TinyUSB. Alternativa descartada
> por complejidad: C3 + MAX3421E por SPI.

### dongle-server (uno por PC)

| Componente | Función |
|---|---|
| LilyGO T-Dongle S3 | ESP-NOW RX, USB HID composite device, BLE |
| LCD integrada | Estado de conexión, emisores |
| Botón integrado | Pairing / reset |

### espnow-retro

Equivalente a [BlueRetro](https://github.com/darthcloud/BlueRetro) con ESP-NOW
como fuente de entrada adicional: mandos Bluetooth directos y mandos USB a
través de un gamepad-client.

| Componente | Función |
|---|---|
| **ESP32 original** | BT Classic + BLE + ESP-NOW (el único ESP32 con BR/EDR) |
| Hardware de adaptador BlueRetro | Level shifting y conector por consola |

| Origen del mando | Camino |
|---|---|
| Bluetooth (DS4, DualSense, Switch Pro, Wiimote, Xbox) | BT → espnow-retro |
| USB | gamepad-client (S3, USB Host) → ESP-NOW → espnow-retro |

Consecuencias:

- Ningún ESP32 tiene a la vez BT Classic y USB OTG: el USB Host local del
  antiguo retro-server desaparece y el gamepad-client pasa a ser el camino de
  los mandos USB hacia consolas.
- Estrategia: fork de BlueRetro (C, ESP-IDF) + entrada ESP-NOW, en vez de
  reescribir los drivers de consola. ❓ Confirmar licencia y que su
  arquitectura admite una fuente de entrada nueva sin reescritura profunda.
- **Riesgo principal: coexistencia BT + WiFi.** Comparten radio (time
  division); con BT Classic y ESP-NOW activos a la vez la latencia y el
  jitter de ambos empeoran. Medir pronto (spike R2). Plan B: dos chips
  (ESP32 para BT + S3/C3 para ESP-NOW) unidos por UART/SPI.

---

## Arquitectura de software

### Lenguajes y stacks

| Pieza | Lenguaje / stack | Motivo |
|---|---|---|
| Núcleo común (protocolo, HMAC, slots, remapeo, modelo de gamepad) | Rust `no_std` | Testeable en PC, compartido con [anygamepad](https://github.com/anygamepad/anygamepad.github.io) |
| km-client, gamepad-client, dongle-server | Rust `std` sobre ESP-IDF (`esp-idf-svc`) | Un solo toolchain, ESP-NOW y NVS desde `esp-idf-svc`, BLE con NimBLE |
| USB Host y USB Device | Componentes C sobre ESP-IDF + wrappers Rust | El USB Host en Rust puro no está maduro; ver [Wrappers Rust](#wrappers-rust) |
| espnow-retro | C (fork BlueRetro) + núcleo vía C ABI | Reutiliza drivers de consola probados |
| Experimentos de validación de hardware | C, ESP-IDF | Mínimas capas entre nosotros y el hardware |

Notas:

- ESP32, S2 y S3 son Xtensa: requieren el toolchain Rust de Espressif
  (`espup`). C3/C6/H2 (RISC-V) usan Rust estable.
- Se descarta por ahora `esp-hal` (`no_std` bare-metal) para las apps: tendría
  sentido en el dongle-server (no necesita USB Host), pero obligaría a mantener
  dos stacks de radio y BLE. El núcleo `no_std` deja la puerta abierta.

### Estructura del repositorio

```
crates/                       Rust
  airhid-core                 no_std: paquetes, HMAC, anti-replay, slots, remapeo, gamepad
  airhid-core-ffi             C ABI del núcleo (cbindgen) para espnow-retro
  airhid-usb-host             wrapper seguro sobre components/airhid_usb_host
  airhid-usb-device           wrapper seguro sobre components/airhid_usb_device
  airhid-air                  enlace ESP-NOW (esp-idf-svc)
  airhid-ble                  pairing BLE (NimBLE)
components/                   C, componentes ESP-IDF
  airhid_usb_host             capa fina sobre usb_host + usb_host_hid
  airhid_usb_device           capa fina sobre esp_tinyusb (HID composite)
apps/
  km-client                   Rust std
  gamepad-client              Rust std
  dongle-server               Rust std
  espnow-retro                fork de BlueRetro (submódulo)
experiments/
  01-usb-capture              fase 1: captura USB Host en C
docs/
```

Dependencias:

```
airhid-core        → anygamepad-model (núcleo no_std, ver abajo)
airhid-usb-host    → airhid-core, components/airhid_usb_host
airhid-usb-device  → airhid-core, components/airhid_usb_device
airhid-air         → airhid-core
airhid-ble         → airhid-core
km-client          → usb-host, air, ble
gamepad-client     → usb-host, air, ble
dongle-server      → usb-device, air, ble
espnow-retro       → airhid-core-ffi
```

### Relación con anygamepad

anygamepad (mismo autor) define un modelo canónico de gamepad y un formato de
perfiles por dispositivo. airHID lo usa en dos pasos:

1. **Corto plazo:** anygamepad genera código C/Rust para airHID: modelo
   compacto (máscara de bits, ejes `int8`/`uint8`) y tablas de perfiles por
   VID/PID. Los perfiles se escriben con fuentes `RawSource::Hid { page,
   usage }`, que es lo que ve un USB Host.
2. **Objetivo:** núcleo `no_std` sin `alloc` en `anygamepad-model` (tipos de
   punto fijo, decodificadores sobre `&[u8]`) usado directamente por
   `airhid-core`.

airHID aporta a anygamepad un backend nuevo (reporte HID en bruto vía USB
Host) y capturas reales de dispositivos (fase 1) para validar decodificadores.

---

## Wrappers Rust

### Principio

La frontera C ↔ Rust se corta **a nivel de reporte**, no envolviendo la API
completa de la pila USB. La capa C es pequeña (instala la pila, abre
interfaces, encola eventos) y Rust solo ve unas pocas funciones con tipos
copiados por valor: sin punteros que crucen la frontera con vida propia ni
callbacks de C hacia Rust.

Ventajas:

- Superficie `unsafe` mínima y auditable.
- Toda la lógica (parseo de descriptores, remapeo, protocolo) en Rust y
  testeable en PC con un backend simulado.
- La pila USB de debajo es intercambiable sin tocar Rust:

| Backend de `airhid_usb_host` | Uso |
|---|---|
| ESP-IDF `usb_host` + `usb_host_hid` | v1, ESP32-S2/S3/P4 |
| TinyUSB host | futuro: RP2040 (PIO-USB), MAX3421E, STM32 |
| Simulado (Rust, en PC) | tests y reproducción de capturas de fase 1 |

### `airhid_usb_host` (C)

```c
#define AIRHID_USB_REPORT_MAX 64

typedef enum {
    AIRHID_USB_EVT_CONNECTED,
    AIRHID_USB_EVT_DISCONNECTED,
    AIRHID_USB_EVT_REPORT,
} airhid_usb_evt_kind_t;

typedef struct {
    uint8_t addr;    // dirección USB
    uint8_t iface;   // número de interfaz
} airhid_usb_dev_t;

typedef struct {
    airhid_usb_evt_kind_t kind;
    airhid_usb_dev_t dev;
    int64_t  timestamp_us;
    union {
        struct {                     // CONNECTED
            uint16_t vid, pid;
            uint8_t  subclass, proto;
            uint16_t report_desc_len;
        } info;
        struct {                     // REPORT
            uint8_t len;
            uint8_t data[AIRHID_USB_REPORT_MAX];
        } report;
    };
} airhid_usb_evt_t;

typedef struct {
    bool     force_boot_protocol;
    uint16_t queue_len;
} airhid_usb_host_config_t;

esp_err_t airhid_usb_host_init(const airhid_usb_host_config_t *cfg);
// Copia el siguiente evento en *out. false si vence el timeout.
bool      airhid_usb_host_next_event(airhid_usb_evt_t *out, uint32_t timeout_ms);
// Copia el report descriptor en buf (*len: capacidad de entrada, tamaño de salida).
esp_err_t airhid_usb_host_report_descriptor(airhid_usb_dev_t dev, uint8_t *buf, size_t *len);
// LEDs del teclado, rumble…
esp_err_t airhid_usb_host_set_report(airhid_usb_dev_t dev, uint8_t report_id,
                                     const uint8_t *data, size_t len);
// Eventos descartados por cola llena desde el arranque.
uint32_t  airhid_usb_host_dropped(void);
```

### `airhid-usb-host` (Rust)

```rust
pub struct DeviceId { pub addr: u8, pub iface: u8 }

pub enum UsbEvent {
    Connected { dev: DeviceId, vid: u16, pid: u16, proto: Protocol, at: Instant },
    Disconnected { dev: DeviceId, at: Instant },
    Report { dev: DeviceId, at: Instant, data: heapless::Vec<u8, 64> },
}

/// Singleton: la pila USB solo se instala una vez.
pub struct UsbHost { /* !Send si la cola no es segura entre hilos */ }

impl UsbHost {
    pub fn take(config: Config) -> Result<Self, EspError>;
    pub fn next_event(&mut self, timeout: Duration) -> Option<UsbEvent>;
    pub fn report_descriptor(&self, dev: DeviceId, buf: &mut [u8]) -> Result<usize, EspError>;
    pub fn set_report(&self, dev: DeviceId, id: u8, data: &[u8]) -> Result<(), EspError>;
    pub fn dropped(&self) -> u32;
}

/// Lo que consume el resto del firmware: permite un backend simulado.
pub trait HidSource {
    fn next_event(&mut self, timeout: Duration) -> Option<UsbEvent>;
}
```

### `airhid_usb_device` (C) / `airhid-usb-device` (Rust)

Mismo patrón en sentido contrario, sobre `esp_tinyusb`:

```c
typedef enum {
    AIRHID_USB_IF_KEYBOARD,
    AIRHID_USB_IF_MOUSE,
    AIRHID_USB_IF_GAMEPAD,
} airhid_usb_if_t;

esp_err_t airhid_usb_device_init(void);                  // HID composite
bool      airhid_usb_device_ready(airhid_usb_if_t itf);   // host montado y endpoint libre
esp_err_t airhid_usb_device_send(airhid_usb_if_t itf, const uint8_t *report, size_t len);
// Output reports del host (LEDs de teclado, rumble) como eventos copiados.
bool      airhid_usb_device_next_output(airhid_usb_if_t *itf, uint8_t *buf,
                                        size_t *len, uint32_t timeout_ms);
```

```rust
pub struct UsbDevice { /* singleton */ }

impl UsbDevice {
    pub fn take() -> Result<Self, EspError>;
    pub fn send_keyboard(&self, report: &KeyboardReport) -> Result<(), SendError>;
    pub fn send_mouse(&self, report: &MouseReport) -> Result<(), SendError>;
    pub fn send_gamepad(&self, state: &Gamepad) -> Result<(), SendError>;
    pub fn next_output(&mut self, timeout: Duration) -> Option<HostOutput>;
}
```

### Radio y BLE

No necesitan capa C propia:

- **ESP-NOW**: `esp_idf_svc::espnow`. `airhid-air` añade framing, HMAC,
  anti-replay y ACK sobre él.
- **BLE**: NimBLE vía crate Rust (p. ej. `esp32-nimble`). ❓ Validar soporte
  de LE Secure Connections y bonding.

Ambos detrás de traits en `airhid-core` (`AirLink`, `Pairing`) para poder
testear el protocolo en PC.

### Integración en el build

- Los componentes C viven en `components/` y se añaden a cada app Rust con
  `[[package.metadata.esp-idf-sys.extra_components]]` (bindings generados
  con bindgen desde su cabecera pública).
- `airhid-core-ffi` genera `airhid_core.h` con cbindgen y se compila como
  `staticlib` para enlazarlo desde el fork de BlueRetro.

---

## Modelo de gamepad

Nombres por **posición**, alineados con `anygamepad-model` (South = botón
inferior en cualquier mando; la etiqueta impresa la da el perfil):

```c
typedef struct {
    uint32_t buttons;            // máscara AIRHID_BTN_*
    int8_t   lx, ly, rx, ry;     // [-127, 127], Y positivo = arriba
    uint8_t  lt, rt;             // [0, 255]
} airhid_gamepad_t;

enum {
    AIRHID_BTN_SOUTH, AIRHID_BTN_EAST, AIRHID_BTN_WEST, AIRHID_BTN_NORTH,
    AIRHID_BTN_LEFT_SHOULDER, AIRHID_BTN_RIGHT_SHOULDER,
    AIRHID_BTN_LEFT_TRIGGER, AIRHID_BTN_RIGHT_TRIGGER,   // digital además de lt/rt
    AIRHID_BTN_SELECT, AIRHID_BTN_START, AIRHID_BTN_HOME,
    AIRHID_BTN_LEFT_STICK, AIRHID_BTN_RIGHT_STICK,
    AIRHID_BTN_DPAD_UP, AIRHID_BTN_DPAD_DOWN, AIRHID_BTN_DPAD_LEFT, AIRHID_BTN_DPAD_RIGHT,
    AIRHID_BTN_EXTRA_0,          // capture, touchpad, paddles…
};
```

Ocupa 10 bytes (frente a 14 `bool` sin ejes del diseño anterior). Las
consolas con botones fuera del layout estándar (Mode y Z de Mega Drive, C de
Saturn) se resuelven en el mapa consola de espnow-retro.

## Remapeo

Dos etapas separadas:

1. **Mando → modelo de gamepad**: perfil por VID/PID. Base embebida + perfil
   de usuario que sobreescribe reglas (merge), en NVS.
2. **Modelo → destino** (HID gamepad del PC o consola concreta): mapa fijo por
   destino, configurable solo en casos especiales.

Formato fuente: perfiles TOML de anygamepad (`b0`, `+a1`, `a1~`, `h0.1`,
fuentes HID por usage). Se compilan a tablas en build. Regla compilada:

```c
typedef struct {
    uint8_t n_src, n_dst;        // número de entradas válidas (sin centinela ambiguo)
    uint8_t src[4];
    uint8_t dst[4];
    int8_t  threshold;           // para ejes usados como botón
} airhid_remap_rule_t;
```

---

## Protocolo

### Transporte

- ESP-NOW en **un canal global** para todo el sistema, negociado en el primer
  pairing. Canales por servidor obligarían a saltar de canal al cambiar de
  target y romperían la sincronización entre clientes.
- Broadcast sin ACK MAC (sin reintentos de capa 2, sin jitter por
  retransmisión); la fiabilidad es de aplicación. Como todos reciben todo, el
  paquete lleva destino.
- `esp_wifi_set_ps(WIFI_PS_NONE)` en todos los nodos; revisar la tasa de
  ESP-NOW (`esp_wifi_config_espnow_rate`, por defecto 1 Mbps).

### Paquete

```c
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  type;       // INPUT | KEEPALIVE | ACK | OUTPUT | TARGET | CONFIG
    uint8_t  stream;     // flujo lógico: teclado, ratón, gamepad n
    uint8_t  dst;        // id corto del destino (0xFF = todos)
    uint32_t session;    // aleatorio por conexión
    uint32_t counter;    // monotónico por (session, stream): anti-replay
    uint8_t  payload[];  // según type/stream
    // uint8_t mac[8];   // HMAC-SHA256 truncado sobre todo lo anterior
} airhid_packet_t;
```

- **Anti-replay**: el receptor descarta todo `counter` ≤ al último aceptado
  de esa sesión y flujo. Un contador de 8 bits permitía repetir paquetes
  capturados tras 256 cambios.
- **Secuencia por flujo**: teclado, ratón y cada gamepad confirman por
  separado; un ACK de un flujo no puede confirmar otro.
- ACK: `type = ACK`, mismo `stream`, eco del `counter`; payload opcional con
  rumble (gamepad) o LEDs (teclado).

### Payloads

| Flujo | Contenido | Semántica |
|---|---|---|
| Teclado | Reporte completo (modificadores + teclas, NKRO si aplica) | Estado: idempotente |
| Ratón | Botones + **acumuladores** `int16` de X, Y, rueda, pan | El servidor aplica la diferencia con el último acumulador visto: reenvíos y pérdidas no duplican ni pierden movimiento |
| Gamepad | `airhid_gamepad_t` | Estado: idempotente |

### Envío (cliente)

```
Cambio de estado  → reenviar hasta ACK con counter ≥ el enviado
Estado confirmado → KEEPALIVE cada ~100 ms
Sin ACK ~500 ms   → TIMEOUT_WARN
Sin ACK ~2 s      → reconexión
```

### Recepción (servidor)

```
Paquete con MAC válido y counter nuevo → aplicar + ACK
Sin paquetes ~500 ms → estado neutro (todo suelto) + TIMEOUT_WARN
Sin paquetes ~2 s    → DISCONNECTED
```

### Seguridad

- La clave **no viaja en claro**: pairing BLE con LE Secure Connections
  (bonding) o intercambio ECDH sobre BLE, y la clave HMAC se deriva del
  secreto compartido (HKDF).
- HMAC-SHA256 truncado a 8 bytes: suficiente para autenticar.
  ❓ Cifrar además el payload (AES-CCM) si el modelo de amenaza incluye
  escuchas pasivas (pulsaciones = contraseñas).
- Sincronización de target entre clientes: **clave de grupo** del sistema,
  distinta de las claves cliente ↔ servidor.

---

## Pairing (BLE)

```
1. PRIMER ARRANQUE (NVS vacío)
   Cliente anuncia por BLE con el UUID del proyecto; el servidor escanea.

2. HANDSHAKE (LE Secure Connections)
   Intercambio de MAC, identidad, canal global y material de claves.

3. PERSISTENCIA
   Ambos guardan {MAC, clave, nombre, id corto} en NVS.

4. SWITCH
   Se cierra BLE y se activa ESP-NOW en el canal global.

5. ARRANQUES POSTERIORES
   ESP-NOW directo desde NVS; reintento periódico si no hay respuesta.
   BLE solo a petición (botón / hotkey).
```

---

## Múltiples clientes y servidores

- Cada cliente guarda N servidores en NVS; uno activo (target) en cada momento.
- Cambio de target por hotkey interceptada por el cliente. ❓ Combinación: la
  tecla `Fn` del Ducky no se ve por HID.
- Sincronización del target entre clientes por broadcast firmado con la clave
  de grupo; cualquier cliente puede iniciarla.

## Gestión de slots (gamepad-client y espnow-retro)

```c
typedef enum { SLOT_PENDING, SLOT_NEXT, SLOT_1, SLOT_2 } slot_t;
```

```
slot_state(s) =
    original_detected[s] ? BUSY :          // solo si el hardware lo detecta
    any(g.slot == s)     ? ACTIVE :
    IDLE

try_assign(g):
    if slot_state(SLOT_1) == IDLE: g.slot = SLOT_1
    elif slot_state(SLOT_2) == IDLE: g.slot = SLOT_2

promote_next():                           // al liberarse cualquier slot
    g = find(g.slot == SLOT_NEXT)
    if g: try_assign(g)

on_connect(g)     → g.slot = SLOT_PENDING; añadir
on_input(g)       → if g.slot == SLOT_PENDING and none(SLOT_NEXT):
                        g.slot = SLOT_NEXT; try_assign(g)
on_disconnect(g)  → quitar; promote_next()      // USB, BT o timeout ESP-NOW
on_original_connect(s)    → original_detected[s] = true
on_original_disconnect(s) → original_detected[s] = false; promote_next()
```

Cambios respecto al diseño anterior: `on_disconnect` también promueve al mando
en espera, y los mandos remotos generan `on_disconnect` al vencer el timeout
de 2 s.

---

## Estados de conexión

| Estado | Cliente | Servidor |
|---|---|---|
| CONNECTED | LED verde / pantalla normal | LCD normal |
| TIMEOUT_WARN | LED amarillo / aviso | LCD aviso + estado neutro |
| DISCONNECTED | LED rojo / error | LCD error |
| PAIRING | LED azul parpadeando | LCD "pairing…" |

Capa de notificación independiente del hardware:

```c
event_post(EVENT_CONNECTION_WARN);

display_driver_subscribe(events);   // SSD1306 o LCD del T-Dongle
led_driver_subscribe(events);       // LED RGB
buzzer_driver_subscribe(events);    // futuro
```

## dongle-server: HID composite

| Interfaz | Descriptor | Notas |
|---|---|---|
| Teclado | HID estándar | Modificadores; ❓ NKRO o boot 6KRO |
| Ratón | HID estándar | Rueda, pan y botones extra |
| Gamepad | HID genérico (DirectInput) | Sin drivers en Windows, Linux y MiSTer; Steam puede presentarlo como XInput |

Modos alternativos en compilación (XInput, Switch Pro, DS4) fuera del MVP.
Validación del gamepad con el tester de anygamepad (Gamepad API + WebHID).

## Pantalla SSD1306 (km-client)

```
┌────────────────────────┐
│ [2/3] PC-TRABAJO       │
│ ESP-NOW ✓  BAT: ████░  │
└────────────────────────┘
```

Target activo, estado de enlace (`ESP-NOW ✓` / `BLE` / `buscando…` /
`PAIRING`), batería estimada y mensajes de depuración.

---

## Notas de hardware

### USB y consola en el ESP32-S3 Zero

El USB-C del S3 Zero va a GPIO19/20, los mismos pines que usa el USB Host:

- El USB-C solo sirve para flashear, con el hub desconectado.
- La consola va por **UART0** (TX GPIO43, RX GPIO44) a un adaptador USB-UART.
- No conectar el USB-C al PC con el hub cableado a GPIO19/20.

### Alimentación (km-client)

```
Powerbank (FM5324HJ1) 5 V
   ├── 5 V/GND → pads del ESP32-S3 Zero
   └── 5 V/GND → VBUS/GND hub FE1.1S (self powered)
                    D+/D− ← GPIO20/GPIO19
                    ├── Teclado USB
                    └── Dongle Bolt
```

- ❓ Comprobar en el esquemático del S3 Zero si hay diodo entre VBUS y 5 V:
  alimentar por pads y conectar el USB-C a la vez puede retroalimentar el PC.
- ❓ Medir si el FM5324 se apaga solo con poca carga.

| Escenario | Corriente @5 V | Autonomía (10 000 mAh) |
|---|---|---|
| RGB encendido | ~350–400 mA | ~19–22 h |
| RGB apagado | ~150–200 mA | ~37–50 h |

Estimaciones a validar con la radio siempre activa (`WIFI_PS_NONE`).

### USB Host en el ESP32-S3

- Full Speed únicamente.
- Soporte de hubs externos reciente en ESP-IDF (5.5+).
- 8 canales de host: un teclado NKRO y un Bolt con varias interfaces pueden
  agotarlos. Se valida en la fase 1.

## Latencia

| Tramo | Típica | Notas |
|---|---|---|
| Polling del dispositivo de entrada | 1–8 ms | Según `bInterval` del teclado/ratón |
| ESP-NOW | 1–2 ms | Jitter 0,5–1 ms; sube con coexistencia BT |
| USB HID Full Speed hacia el PC | 1 ms | Polling mínimo |
| **Total realista** | **3–6 ms** | El objetivo de 2–3 ms es optimista |

---

## Fases

| Fase | Contenido | Estado |
|---|---|---|
| 1 | Captura USB Host en C: teclado, hub, Bolt, NKRO, intervalos (`experiments/01-usb-capture`) | En curso |
| 2 | `airhid_usb_host` + `airhid-usb-host` + `airhid-core`; km-client en Rust que reproduce la fase 1 | |
| 3 | ESP-NOW + dongle-server (`airhid-usb-device`): teclado y ratón extremo a extremo con HMAC y anti-replay | |
| 4 | Pairing BLE seguro, varios servidores en NVS, cambio de target | |
| 5 | Varios clientes sincronizados, gamepad-client, gamepad en el composite | |
| 6 | espnow-retro: fork de BlueRetro + entrada ESP-NOW | |
| 7 | Pantalla y UX del km-client | |
| 8 | Control RGB del Ducky (protocolo OpenRGB), apagado por inactividad | |

Spikes que pueden adelantarse:

- **R1** — anygamepad: generación de modelo compacto y tablas de perfiles
  para airHID.
- **R2** — Coexistencia BT Classic + ESP-NOW en ESP32 original: latencia y
  jitter de ambos con tráfico simultáneo.
- **R3** — Toolchain Rust Xtensa + `esp-idf-sys` con un componente C propio
  (`extra_components`) compilando en CI.

## Riesgos

| Riesgo | Impacto | Mitigación |
|---|---|---|
| USB Host + hub en S3 (canales, dispositivos Low Speed tras hub) | Bloquea km-client | Fase 1; plan B: un dispositivo por puerto o MAX3421E |
| Coexistencia BT + ESP-NOW | Latencia en espnow-retro | Spike R2; plan B de dos chips |
| Seguridad del pairing | Keylogger / inyección | LE Secure Connections + HKDF; revisión antes de fase 4 |
| Toolchain Rust Xtensa | Fricción de build | Spike R3; las piezas críticas de hardware siguen en C |
| Arquitectura de BlueRetro poco extensible | Más trabajo en fase 6 | Revisar el código antes de comprometer la fase |

---

## Hardware disponible

| Placa | Rol |
|---|---|
| ESP32-S3 Zero | km-client, gamepad-client |
| ESP32-C3 Zero | Sin rol con USB Host; usos sin USB (p. ej. nodo de estado) |
| LilyGO T-Dongle S3 | dongle-server |
| ESP32 original | espnow-retro (pendiente de adquirir si no hay) |
| Raspberry Pi Pico | Alternativa de USB Device o USB Host (PIO-USB) |
| Hub FE1.1S | Teclado + ratón / mandos USB |
| Powerbank FM5324HJ1 | Alimentación km-client |
| Ducky One 2 SF (DKON1967ST) | Teclado objetivo |
| Dongle Logitech Bolt | Receptor de ratón objetivo |

## Proyectos relacionados

- **[anygamepad](https://github.com/anygamepad/anygamepad.github.io)** — modelo
  canónico de gamepad, perfiles por dispositivo y tester web. Fuente del
  modelo y del formato de perfiles de airHID.
- **[BlueRetro](https://github.com/darthcloud/BlueRetro)** — mandos Bluetooth
  a consolas retro. Base de espnow-retro.
- **SDL GameControllerDB** — mapeos de mandos, importados por anygamepad.
- **Variante nRF24** — gamepad-client de latencia ultra baja (~0,1–0,5 ms) en
  canales fuera de la banda WiFi.

---

## Preguntas abiertas

1. ¿La sincronización entre varios clientes (O5) es imprescindible?
2. ¿Autenticar solo, o también cifrar el payload (O9)?
3. Presupuesto de latencia y alcance (O1).
4. ¿Solo el hardware objetivo (Ducky + Bolt) o cualquier HID? ¿NKRO en v1?
5. Hotkey de cambio de target sin `Fn` visible por HID.
6. ¿Cuántos mandos por gamepad-client? ¿Rumble en v1?
7. ¿Cómo se configura el remapeo: BLE desde móvil, web o pantalla?
8. BlueRetro: licencia actual y facilidad para añadir una fuente ESP-NOW.
9. Ubicación de la generación de código de perfiles: anygamepad o airHID.
