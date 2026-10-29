# Fase 1 — Captura USB Host

Firmware de validación para ESP32-S3: enumera dispositivos HID (directos o
detrás del hub FE1.1S) y vuelca cada reporte por UART con marca de tiempo.

## Hardware

```
Fuente 5V ──┬── 5V/GND ESP32-S3 Zero
            └── VBUS/GND hub FE1.1S (self-powered)

ESP32-S3 Zero GPIO19 (D−) / GPIO20 (D+) ── D−/D+ upstream del hub

ESP32-S3 Zero GPIO43 (TX) ── RX adaptador USB-UART (3.3V) ── PC
ESP32-S3 Zero GND         ── GND adaptador
```

⚠️ El USB-C del S3 Zero comparte GPIO19/20 con el USB Host. Úsalo solo para
flashear (con el hub desconectado) y lee los logs por el adaptador UART.

## Compilar y flashear

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash          # por el USB-C, hub desconectado
idf.py -p /dev/ttyUSB0 -b 921600 monitor   # por el adaptador UART
```

Opciones en `idf.py menuconfig` → *airHID captura USB*:
- volcar report descriptors (por defecto sí)
- forzar boot protocol (por defecto no)

## Salida

```
I (512) capture: conectado a=2 i=0 subclass=1 proto=keyboard
D a=2 i=0 n=63
    05 01 09 06 a1 01 ...
R t=8123456 dt=0 a=2 i=0 p=1 n=8 | 00 00 04 00 00 00 00 00
R t=8201220 dt=77764 a=2 i=0 p=1 n=8 | 00 00 00 00 00 00 00 00
```

`a` = dirección USB, `i` = interfaz, `p` = protocolo (1 teclado, 2 ratón),
`dt` = µs desde el reporte anterior de esa interfaz.

## Plan de pruebas

| # | Montaje | Criterio de éxito |
|---|---|---|
| 1 | Teclado directo (sin hub) | Enumera, cada tecla genera reporte de pulsar y soltar |
| 2 | Teclado detrás del hub | Igual que 1; el hub enumera sin errores |
| 3 | Teclado + Bolt en el hub | Ambos a la vez, sin errores de transferencia |
| 4 | Ratón moviéndose rápido | `dt` estable (≈1000 µs a 1 kHz), sin reportes perdidos |
| 5 | Ducky en NKRO | Descriptor volcado; pulsar >6 teclas se refleja |
| 6 | Desconectar/reconectar en caliente | Se re-enumera sin reiniciar |

Guarda los descriptores volcados (`D ...`) en `docs/descriptors/`: son la
base para el parser HID de las fases siguientes.

Nota: imprimir cada reporte a 921600 baudios cuesta ~0,7 ms por línea. Con
el ratón a 1 kHz la consola puede ser el cuello de botella; si ves `dt`
irregulares, compáralo con el teclado solo antes de culpar al USB.
