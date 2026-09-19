# VW OS — Sistema Operativo Automotriz Bare-Metal

**Desarrollado por PaolosSoftware**
Puebla, México

---

## ¿Qué es VW OS?

VW OS es un sistema operativo automotriz construido desde cero en C puro y ensamblador ARM64, diseñado para reemplazar Android Automotive OS en vehículos del Grupo Volkswagen.

Sin Linux. Sin Android. Sin Google. Sin obligación de publicar código.

> *"Tú pones el chip, nosotros te quitamos a Google de encima."*

---

## Compatibilidad

El OS se auto-detecta al encender el vehículo. Sin configuración manual.

| Vehículo | Powertrain | Detección |
|---|---|---|
| CUPRA Born | BEV — Eléctrico puro | ✅ Automática |
| VW ID.3 / ID.4 | BEV — Eléctrico puro | ✅ Automática |
| CUPRA Formentor e-Hybrid | PHEV — Híbrido enchufable | ✅ Automática |
| VW Tiguan eHybrid | HEV — Híbrido | ✅ Automática |
| SEAT Ibiza / VW Golf | ICE — Gasolina | ✅ Automática |
| Audi Q4 e-tron | BEV — Eléctrico puro | ✅ Automática |

Un solo binario. Se adapta solo al hardware del vehículo.

---

## Arquitectura

```
┌─────────────────────────────────────────────────────┐
│                    APLICACIONES                      │
│   Agente IA   │   Navegación   │   Media   │   OTA  │
├─────────────────────────────────────────────────────┤
│                  SERVICIOS DEL OS                    │
│  vehicle_bridge │ audio │ net │ vwfs │ ui_launcher  │
├─────────────────────────────────────────────────────┤
│                    KERNEL                            │
│    scheduler │ mmu │ gic │ timer │ framebuffer      │
├─────────────────────────────────────────────────────┤
│                   HARDWARE                           │
│         ARM64 SoC automotriz (Snapdragon Cockpit)   │
└─────────────────────────────────────────────────────┘
```

---

## Módulos

### Kernel
| Módulo | Descripción |
|---|---|
| `boot.S` | Arranque ARM64 — EL1, vectores de excepción, stacks per-core |
| `mmu.c` | Tablas de páginas nivel 1, MAIR/TCR/TTBR0, caches habilitadas |
| `gic.c` | GICv2 — distribuidor, CPU interface, despacho de IRQs |
| `timer.c` | ARM Generic Timer 100Hz |
| `context_switch.S` | Guardado/restaurado de 31 registros + SP + PC + SPSR |
| `scheduler.c` | Round-robin, multitarea real |
| `uart.c` | PL011 bare-metal para debug |
| `framebuffer.c` | 1280×720 @32bpp, primitivas gráficas, fuente bitmap |

### Drivers
| Módulo | Descripción |
|---|---|
| `vehicle_bridge.c` | Auto-detección de powertrain vía CAN bus. Escucha 2 segundos al arranque y determina BEV/ICE/HEV/PHEV automáticamente |
| `touch.c` | Driver multi-chip: VIRTIO/FT5406/GT911, IRQ en GIC |
| `input.c` | Cola circular de eventos, gestos: TAP/LONG_PRESS/SWIPE/MULTITOUCH |
| `audio.c` | Driver I2S 3 capas: micrófono + bocinas, generador de beep (Bhaskara I) |
| `net.c` | Cliente de red minimalista: net_request / net_download / net_available |

### Sistema de archivos
| Módulo | Descripción |
|---|---|
| `vwfs.c` | Sistema de archivos propio: superbloque, journal 64KB, 65536 inodos, SHA256 embebido |

### Agente de IA
| Módulo | Descripción |
|---|---|
| `ai_tokenizer.c` | Tokenizador BPE 32K tokens portado a bare-metal (cero libc) |
| `ai_dispatcher.c` | Ejecuta comandos del OS: batería, velocidad, clima, navegación |
| `ai_agent.c` | Pipeline 5 capas: STT → tokenizer → inference → dispatcher → TTS |
| `ai_vocab_data.c` | vocab.json embebido como array C |
| `ai_merges_data.c` | merges.txt embebido como array C |
| `ai_stubs.c` | Stubs para inference y TTS (pendiente equipo de IA) |

### OTA — Actualizaciones Over The Air
| Módulo | Descripción |
|---|---|
| `ota.c` | Doble partición A/B, SHA256 bare-metal, parser JSON mínimo |

### Navegación — VW OS Maps
| Módulo | Descripción |
|---|---|
| `map_data.c` | Datos de calles reales (OSM). QEMU: Puebla embebido. Producción: tiles desde VWFS |
| `map_render.c` | Renderer vectorial estilo OSM — fondo beige, calles blancas, nombres rotados |
| `map_router.c` | Motor de rutas A* — costo por tipo de vía, ETA calculado |
| `map.c` | Orquestador y API pública |

### Interfaz de usuario
| Módulo | Descripción |
|---|---|
| `ui_launcher.c` | UI completa: topbar, sidebar, panel vehículo, mapa, media, clima, acciones rápidas, bottombar |
| `ui_font_big.c` | Fuente escalable SM/MD/LG/XL/XXL |

---

## VW OS Maps

Navegación propia basada en OpenStreetMap. Sin Google Maps. Sin regalías por uso. Los mapas viven en el vehículo.

```
Datos OSM (ODbL)  →  net_download()  →  VWFS  →  map_render  →  framebuffer
```

**© OpenStreetMap contributors** — crédito visible en la UI en todo momento, según licencia ODbL.

El código del renderer, el router A* y la integración con el OS son propiedad de PaolosSoftware y no están sujetos a la licencia ODbL.

---

## Auto-detección de Powertrain

El vehicle bridge escucha el CAN bus los primeros 2 segundos del arranque:

```
Solo HW_CAN_ID_BATTERY          →  BEV  (Born, ID.3, ID.4)
BATTERY + FUEL + HYBRID_MODE    →  PHEV (Formentor e-Hybrid)
BATTERY + FUEL                  →  HEV  (híbrido no enchufable)
FUEL + RPM                      →  ICE  (Ibiza, Golf, Formentor VZ)
Sin mensajes                    →  Sin hardware (QEMU/desarrollo)
```

La UI se adapta automáticamente:
- **BEV**: barra de batería, autonomía en km, kW en tiempo real
- **ICE**: barra de combustible, RPM, marchas 1-6, L/100km
- **PHEV/HEV**: ambas barras, modo activo (eléctrico/gasolina/combinado)

---

## Licencias de terceros

| Componente | Licencia | Uso |
|---|---|---|
| Datos de OpenStreetMap | ODbL | Mapas de navegación. Crédito en UI |
| Tokenizador BPE (equipo IA) | Propietario | Portado a bare-metal en `ai_tokenizer.c` |
| Modelo 500M (pendiente) | MIT (Whisper/llama.cpp base) | Solo crédito en documentación |

Todo el código de VW OS es propiedad de PaolosSoftware. No hay dependencias GPL. No hay obligación de publicar código fuente.

---

## Desarrollo y compilación

### Requisitos
```bash
aarch64-linux-gnu-gcc   # toolchain ARM64
qemu-system-aarch64     # emulador para desarrollo
```

### Compilar
```bash
cd kernel
make clean && make all
```

### Correr en QEMU
```bash
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
    -nographic -kernel build/kernel.elf
```

### Portar a hardware real de VW
1. Cambiar `HW_CAN_ID_*` en `hw_config.h` con el DBC real de VW
2. Cambiar `HW_NET_TYPE` y `HW_AUDIO_TYPE` con el SoC del vehículo
3. Implementar las funciones `*_hw_*` en cada driver con el datasheet
4. El resto del OS no se toca

**Un solo archivo de configuración (`hw_config.h`) porta el OS completo a cualquier SoC del Grupo VW.**

---

## Estado del proyecto

```
Kernel ARM64          ✅ Completo
MMU / GIC / Timer     ✅ Completo
Multitarea real       ✅ Completo
Framebuffer 1280x720  ✅ Completo
Sistema de archivos   ✅ Completo
Vehicle bridge        ✅ Auto-detección ICE/BEV/HEV/PHEV
Driver de audio       ✅ Completo (I2S bare-metal)
Driver de red         ✅ Completo (3 capas)
OTA doble partición   ✅ Completo (SHA256 propio)
VW OS Maps (OSM)      ✅ Completo (renderer + router A*)
Tokenizador BPE 32K   ✅ Integrado bare-metal
Dispatcher de IA      ✅ Completo
UI completa           ✅ 1280x720
Motor de inferencia   ⏳ Equipo de IA (modelo 500M)
Sintetizador de voz   ⏳ Equipo de IA
UI por powertrain     ⏳ En desarrollo
```

---

## Roadmap

| Fase | Objetivo | Estado |
|---|---|---|
| Q4 2026 | Demo en VW Puebla | En progreso |
| Q1 2027 | Modelo 500M integrado | Equipo de IA |
| Q2 2027 | Hardware real VW (NDA) | Pendiente VW |
| Q3 2027 | Validación CARIAD | Pendiente |
| Q4 2027 | Producción Grupo VW | Objetivo |

---

## Equipo

**PaolosSoftware** — Puebla, México

100 personas: kernel, drivers, vehicle bridge, UI, IA, apps, OTA/cloud, QA, chip, pitch/legal.

---

*VW OS — Construido desde cero. Sin Google. Sin restricciones.*
*© 2026 PaolosSoftware. Todos los derechos reservados.*
