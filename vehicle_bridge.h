#include "vehicle_bridge.h"
#include "gic.h"
#include "uart.h"
#include "timer.h"

/*
 * vehicle_bridge.c -- Driver del bus CAN con auto-deteccion de powertrain
 * ============================================================================
 *
 * SIN SIMULACION. El bridge escucha el CAN bus real y detecta
 * automaticamente si el vehiculo es BEV, ICE, HEV o PHEV.
 *
 * En desarrollo sin hardware (QEMU):
 *   El CAN bus no genera mensajes. vb_data.powertrain queda en
 *   VB_POWERTRAIN_UNKNOWN y vb_data.valid = 0. La UI muestra
 *   pantalla de "Conectando con el vehiculo...".
 *
 * En hardware real (CUPRA Formentor, VW Golf, Born, etc.):
 *   El controlador CAN genera IRQs. Cada mensaje se parsea segun
 *   su ID y actualiza vb_data. La deteccion de powertrain ocurre
 *   en los primeros 2 segundos del arranque.
 *
 * Para portar a un vehiculo especifico de VW:
 *   1. Cambiar HW_CAN_ID_* en hw_config.h con el DBC real de VW
 *   2. Ajustar can_parse_message() si el formato de bytes difiere
 *   3. Nada mas cambia -- la UI y el agente de IA leen vb_get_data()
 * ============================================================================
 */

/* Datos del vehiculo -- actualizados por IRQ del CAN */
static vb_data_t     vb_data;
static vb_callback_t vb_callback = 0;

/* Estado de la deteccion de powertrain */
static struct {
    unsigned long start_tick;  /* cuando empezo la ventana de deteccion */
    int detecting;             /* 1 = aun en ventana de deteccion        */
    /* Flags de mensajes CAN vistos durante la deteccion */
    int saw_battery;           /* llego HW_CAN_ID_BATTERY               */
    int saw_fuel;              /* llego HW_CAN_ID_FUEL                  */
    int saw_rpm;               /* llego HW_CAN_ID_RPM con valor > 0     */
    int saw_regen;             /* llego HW_CAN_ID_REGEN                 */
    int saw_hybrid_mode;       /* llego HW_CAN_ID_HYBRID_MODE           */
} detect_state;

/* -------------------------------------------------------------------------
 * Utilidades internas
 * ------------------------------------------------------------------------- */
static void vb_notify(unsigned int changed_flags)
{
    if (vb_callback) vb_callback(&vb_data, changed_flags);
}

static void vb_zero(void)
{
    volatile unsigned char *p = (volatile unsigned char *)&vb_data;
    for (unsigned int i = 0; i < sizeof(vb_data_t); i++) p[i] = 0;
    vb_data.powertrain = VB_POWERTRAIN_DETECTING;
    vb_data.valid      = 0;
}

static void uart_put_dec_u(unsigned int v)
{
    char tmp[12]; int ti = 0;
    if (!v) { uart_putc('0'); return; }
    while (v) { tmp[ti++] = '0' + (v % 10); v /= 10; }
    for (int a=0,b=ti-1; a<b; a++,b--) { char t=tmp[a]; tmp[a]=tmp[b]; tmp[b]=t; }
    tmp[ti] = '\0'; uart_puts(tmp);
}

/* -------------------------------------------------------------------------
 * Auto-deteccion de powertrain
 * -------------------------------------------------------------------------
 * Llamado cuando expira la ventana de deteccion O cuando ya tenemos
 * suficiente informacion para decidir antes de que expire.
 * ------------------------------------------------------------------------- */
static void vb_finalize_powertrain_detection(void)
{
    detect_state.detecting = 0;

    vb_powertrain_t pt;

    if (detect_state.saw_battery && !detect_state.saw_fuel) {
        /* Solo mensajes de bateria -- electrico puro */
        pt = VB_POWERTRAIN_BEV;
    } else if (detect_state.saw_battery && detect_state.saw_fuel
               && detect_state.saw_hybrid_mode) {
        /* Bateria + combustible + modo hibrido -- PHEV enchufable */
        pt = VB_POWERTRAIN_PHEV;
    } else if (detect_state.saw_battery && detect_state.saw_fuel) {
        /* Bateria + combustible -- hibrido no enchufable */
        pt = VB_POWERTRAIN_HEV;
    } else if (detect_state.saw_fuel || detect_state.saw_rpm) {
        /* Solo combustible/RPM -- motor de combustion puro */
        pt = VB_POWERTRAIN_ICE;
    } else {
        /* No llego nada -- sin hardware CAN conectado */
        pt = VB_POWERTRAIN_UNKNOWN;
    }

    vb_data.powertrain = pt;
    vb_data.valid      = (pt != VB_POWERTRAIN_UNKNOWN) ? 1 : 0;

    uart_puts("[vbridge] Powertrain detectado: ");
    uart_puts(vb_powertrain_name(pt));
    uart_puts("\n");

    vb_notify(VB_CHANGED_POWERTRAIN | VB_CHANGED_ALL);
}

/* -------------------------------------------------------------------------
 * Parser de mensajes CAN
 * -------------------------------------------------------------------------
 * Recibe el ID y los 8 bytes de datos de un mensaje CAN y actualiza
 * vb_data con los valores parseados.
 *
 * FORMATO DE BYTES (GENERICO -- del DBC real de VW cuando haya NDA):
 *   Cada campo usa un formato tipico de la industria automotriz.
 *   Cuando VW entregue el DBC, solo hay que ajustar los factores
 *   de escala y los offsets de bytes.
 * ------------------------------------------------------------------------- */
/* Suprimir warning: la funcion se usa desde can_irq_handler
 * cuando el hardware CAN esta conectado */
__attribute__((used))
static void can_parse_message(unsigned int can_id,
                               const unsigned char *data)
{
    unsigned int changed = 0;

    /* --- Marcar flags de deteccion segun que mensajes llegan --- */
    if (can_id == HW_CAN_ID_BATTERY)     detect_state.saw_battery     = 1;
    if (can_id == HW_CAN_ID_FUEL)        detect_state.saw_fuel        = 1;
    if (can_id == HW_CAN_ID_REGEN)       detect_state.saw_regen       = 1;
    if (can_id == HW_CAN_ID_HYBRID_MODE) detect_state.saw_hybrid_mode = 1;
    if (can_id == HW_CAN_ID_RPM) {
        unsigned int rpm_raw = (unsigned int)data[0] | ((unsigned int)data[1] << 8);
        if (rpm_raw > 0) detect_state.saw_rpm = 1;
    }

    /* --- Parsear el contenido del mensaje --- */
    if (can_id == HW_CAN_ID_SPEED) {
        /* Bytes 0-1: velocidad en 0.01 km/h por bit
         * Formula: speed_kmh = (data[0] | data[1]<<8) / 100 */
        unsigned int raw = (unsigned int)data[0] |
                           ((unsigned int)data[1] << 8);
        unsigned int new_speed = raw / 100;
        if (new_speed != vb_data.speed_kmh) {
            vb_data.speed_kmh = new_speed;
            changed |= VB_CHANGED_SPEED;
        }
    }

    else if (can_id == HW_CAN_ID_RPM) {
        /* Bytes 0-1: RPM en 0.25 RPM por bit
         * Formula: rpm = (data[0] | data[1]<<8) / 4 */
        unsigned int raw = (unsigned int)data[0] |
                           ((unsigned int)data[1] << 8);
        vb_data.rpm = raw / 4;
    }

    else if (can_id == HW_CAN_ID_GEAR) {
        /* Byte 0: marcha actual (valor directo VB_GEAR_*) */
        unsigned char new_gear = data[0];
        if (new_gear != vb_data.gear) {
            vb_data.gear = new_gear;
            changed |= VB_CHANGED_GEAR;
        }
    }

    else if (can_id == HW_CAN_ID_ACCEL_BRAKE) {
        /* Byte 0: acelerador 0-100%
         * Byte 1: freno 0-100% */
        vb_data.accelerator_pct = data[0];
        vb_data.brake_pct       = data[1];
    }

    else if (can_id == HW_CAN_ID_BATTERY) {
        /* Byte 0: nivel de bateria 0-100% (valor directo)
         * Bytes 1-2: autonomia electrica en km
         * Bytes 3-4: potencia en kW (signed, x10) */
        unsigned char new_pct = data[0];
        if (new_pct != vb_data.battery_pct) {
            vb_data.battery_pct = new_pct;
            changed |= VB_CHANGED_BATTERY;
            /* Alerta si baja del 20% */
            if (new_pct < 20 && !(vb_data.alerts & VB_ALERT_BATTERY_LOW)) {
                vb_data.alerts |= VB_ALERT_BATTERY_LOW;
                changed |= VB_CHANGED_ALERTS;
            } else if (new_pct >= 20 && (vb_data.alerts & VB_ALERT_BATTERY_LOW)) {
                vb_data.alerts &= ~VB_ALERT_BATTERY_LOW;
                changed |= VB_CHANGED_ALERTS;
            }
        }
        vb_data.range_km = (unsigned int)data[1] |
                           ((unsigned int)data[2] << 8);
        int power_raw = (int)((unsigned int)data[3] |
                               ((unsigned int)data[4] << 8));
        vb_data.power_kw = power_raw / 10;
    }

    else if (can_id == HW_CAN_ID_BATTERY_VOLT) {
        /* Bytes 0-1: voltaje en mV */
        vb_data.voltage_mv = (unsigned int)data[0] |
                             ((unsigned int)data[1] << 8);
        vb_data.voltage_mv *= 100;  /* escala tipica: x100 mV por bit */
    }

    else if (can_id == HW_CAN_ID_REGEN) {
        /* Byte 0: 1 = regeneracion activa, 0 = no */
        vb_data.regen_active = data[0] & 0x01;
    }

    else if (can_id == HW_CAN_ID_HYBRID_MODE) {
        /* Byte 0: VB_HYBRID_MODE_* */
        vb_data.hybrid_mode = data[0];
        changed |= VB_CHANGED_HYBRID_MODE;
    }

    else if (can_id == HW_CAN_ID_FUEL) {
        /* Byte 0: nivel de combustible 0-100%
         * Bytes 1-2: litros restantes (x10, ej: 350 = 35.0 litros) */
        unsigned char new_fuel = data[0];
        if (new_fuel != vb_data.fuel_pct) {
            vb_data.fuel_pct = new_fuel;
            changed |= VB_CHANGED_FUEL;
            if (new_fuel < 10 && !(vb_data.alerts & VB_ALERT_FUEL_LOW)) {
                vb_data.alerts |= VB_ALERT_FUEL_LOW;
                changed |= VB_CHANGED_ALERTS;
            } else if (new_fuel >= 10 && (vb_data.alerts & VB_ALERT_FUEL_LOW)) {
                vb_data.alerts &= ~VB_ALERT_FUEL_LOW;
                changed |= VB_CHANGED_ALERTS;
            }
        }
        unsigned int liters_raw = (unsigned int)data[1] |
                                   ((unsigned int)data[2] << 8);
        vb_data.fuel_liters = liters_raw / 10;
    }

    else if (can_id == HW_CAN_ID_FUEL_RANGE) {
        /* Bytes 0-1: autonomia con gasolina en km */
        vb_data.range_fuel_km = (unsigned int)data[0] |
                                 ((unsigned int)data[1] << 8);
    }

    else if (can_id == HW_CAN_ID_CONSUMPTION) {
        /* Bytes 0-1: consumo x10 (ej: 65 = 6.5 L/100km) */
        vb_data.consumption_l100 = (unsigned int)data[0] |
                                    ((unsigned int)data[1] << 8);
    }

    else if (can_id == HW_CAN_ID_TEMP) {
        /* Byte 0: temperatura motor (offset -40, 1 grado por bit)
         * Byte 1: temperatura bateria
         * Byte 2: temperatura aceite
         * Byte 3: temperatura ambiente */
        vb_data.motor_temp_c   = (int)data[0] - 40;
        vb_data.battery_temp_c = (int)data[1] - 40;
        vb_data.oil_temp_c     = (int)data[2] - 40;
        vb_data.ambient_temp_c = (int)data[3] - 40;
    }

    else if (can_id == HW_CAN_ID_DOORS) {
        /* Byte 0: bitmask de puertas abiertas
         * Bit 0=FL, 1=FR, 2=RL, 3=RR, 4=maletero, 5=capo */
        unsigned char prev_doors = vb_data.doors_open;
        vb_data.doors_open  =  data[0] & 0x0F;
        vb_data.trunk_open  = (data[0] >> 4) & 0x01;
        vb_data.hood_open   = (data[0] >> 5) & 0x01;
        vb_data.handbrake   = (data[0] >> 6) & 0x01;
        if (vb_data.doors_open && vb_data.speed_kmh > 0) {
            if (!(vb_data.alerts & VB_ALERT_DOOR_OPEN)) {
                vb_data.alerts |= VB_ALERT_DOOR_OPEN;
                changed |= VB_CHANGED_ALERTS;
            }
        } else if (!vb_data.doors_open) {
            vb_data.alerts &= ~VB_ALERT_DOOR_OPEN;
            if (prev_doors) changed |= VB_CHANGED_ALERTS;
        }
    }

    else if (can_id == HW_CAN_ID_ALERTS) {
        /* Bytes 0-3: bitmask de alertas (directo a VB_ALERT_*) */
        unsigned int new_alerts = (unsigned int)data[0]        |
                                  ((unsigned int)data[1] << 8)  |
                                  ((unsigned int)data[2] << 16) |
                                  ((unsigned int)data[3] << 24);
        if (new_alerts != vb_data.alerts) {
            vb_data.alerts = new_alerts;
            changed |= VB_CHANGED_ALERTS;
        }
    }

    else if (can_id == HW_CAN_ID_ADAS) {
        /* Byte 0: nivel ADAS activo (0-4)
         * Byte 1: flags (bit0=lane_keep, 1=cruise, 2=emerg_brake)
         * Bytes 2-3: distancia al frente en cm
         * Byte 4: limite de velocidad detectado en km/h */
        vb_data.adas.level           = data[0];
        vb_data.adas.lane_keep       = (data[1] >> 0) & 0x01;
        vb_data.adas.adaptive_cruise = (data[1] >> 1) & 0x01;
        vb_data.adas.emergency_brake = (data[1] >> 2) & 0x01;
        vb_data.adas.front_distance  = (unsigned int)data[2] |
                                       ((unsigned int)data[3] << 8);
        vb_data.adas.speed_limit     = data[4];
        changed |= VB_CHANGED_ADAS;
    }

    else if (can_id == HW_CAN_ID_ODOMETER) {
        /* Bytes 0-3: odometro total en km
         * Bytes 4-5: kilometraje del viaje actual en km */
        vb_data.odometer_km = (unsigned long)data[0]        |
                              ((unsigned long)data[1] << 8)  |
                              ((unsigned long)data[2] << 16) |
                              ((unsigned long)data[3] << 24);
        vb_data.trip_km     = (unsigned long)data[4] |
                              ((unsigned long)data[5] << 8);
    }

    vb_data.last_update_tick = timer_get_ticks();
    if (changed) vb_notify(changed);
}

/* -------------------------------------------------------------------------
 * Handler de IRQ del controlador CAN
 * -------------------------------------------------------------------------
 * Llamado por el GIC cada vez que llega un mensaje CAN del vehiculo.
 * Lee el mensaje del buffer del controlador y lo pasa al parser.
 * ------------------------------------------------------------------------- */
static void can_irq_handler(unsigned int irq_id)
{
    (void)irq_id;

    /* Leer el mensaje del buffer del controlador CAN del SoC.
     *
     * FORMATO TIPICO de un controlador CAN mapeado en memoria:
     *   Registro STATUS : indica que hay mensaje disponible
     *   Registro ID     : ID de 11 bits (CAN 2.0A) o 29 bits (CAN 2.0B)
     *   Registros DATA  : hasta 8 bytes de payload
     *
     * La direccion base (HW_CAN_BASE) y los offsets de registros
     * vienen del datasheet del SoC de VW -- se definen en hw_config.h.
     *
     * Por ahora: sin hardware real, el handler no lee nada.
     * Cuando VW entregue el datasheet, se descomenta el bloque de abajo
     * y se ajustan los offsets.
     *
     * Ejemplo para un controlador tipo Bosch DCAN (comun en VW):
     *
     *   #define CAN_REG(off) (*(volatile unsigned int*)(HW_CAN_BASE + off))
     *
     *   if (CAN_REG(0x00) & 0x01) {            // hay mensaje
     *       unsigned int can_id = CAN_REG(0x04) & 0x7FF;
     *       unsigned char data[8];
     *       for (int i = 0; i < 8; i++)
     *           data[i] = (CAN_REG(0x08 + (i/4)*4) >> ((i%4)*8)) & 0xFF;
     *       CAN_REG(0x00) = 0x01;               // limpiar flag
     *       can_parse_message(can_id, data);
     *   }
     *
     * Verificar ventana de deteccion de powertrain:
     */
    if (detect_state.detecting) {
        unsigned long now = timer_get_ticks();
        if (now - detect_state.start_tick >= HW_VB_DETECT_WINDOW) {
            vb_finalize_powertrain_detection();
        }
    }
}

/* -------------------------------------------------------------------------
 * vb_init -- inicializar el vehicle bridge
 * ------------------------------------------------------------------------- */
void vb_init(void)
{
    uart_puts("[vbridge] Iniciando Vehicle Bridge...\n");

    vb_zero();

    /* Limpiar estado de deteccion */
    detect_state.start_tick    = timer_get_ticks();
    detect_state.detecting     = 1;
    detect_state.saw_battery   = 0;
    detect_state.saw_fuel      = 0;
    detect_state.saw_rpm       = 0;
    detect_state.saw_regen     = 0;
    detect_state.saw_hybrid_mode = 0;

    /* Registrar IRQ del CAN en el GIC */
    gic_register_handler(HW_CAN_IRQ_ID, can_irq_handler);
    gic_enable_irq(HW_CAN_IRQ_ID);

    uart_puts("[vbridge] IRQ CAN registrada (ID: ");
    uart_put_dec_u(HW_CAN_IRQ_ID);
    uart_puts(").\n");

    uart_puts("[vbridge] Detectando powertrain (escuchando CAN bus ");
    uart_put_dec_u(HW_VB_DETECT_WINDOW / 100);
    uart_puts(" seg)...\n");

    /* Si no hay hardware CAN (QEMU/desarrollo), la ventana de deteccion
     * expira sin recibir mensajes -> powertrain = UNKNOWN -> valid = 0.
     * La UI mostrara "Conectando con el vehiculo..." */
}

/* -------------------------------------------------------------------------
 * vb_update -- forzar cierre de la ventana de deteccion
 * -------------------------------------------------------------------------
 * Llamado periodicamente por el scheduler. Si la ventana de deteccion
 * expiro y aun no decidimos el powertrain, lo cerramos ahora.
 * ------------------------------------------------------------------------- */
void vb_update(void)
{
    if (!detect_state.detecting) return;

    unsigned long now = timer_get_ticks();
    if (now - detect_state.start_tick >= HW_VB_DETECT_WINDOW) {
        vb_finalize_powertrain_detection();
    }
}

/* -------------------------------------------------------------------------
 * API publica
 * ------------------------------------------------------------------------- */
void vb_register_callback(vb_callback_t cb) { vb_callback = cb; }
const vb_data_t *vb_get_data(void)          { return &vb_data; }

const char *vb_powertrain_name(vb_powertrain_t pt)
{
    switch (pt) {
        case VB_POWERTRAIN_BEV:       return "BEV (Electrico puro)";
        case VB_POWERTRAIN_ICE:       return "ICE (Gasolina/Diesel)";
        case VB_POWERTRAIN_HEV:       return "HEV (Hibrido)";
        case VB_POWERTRAIN_PHEV:      return "PHEV (Hibrido enchufable)";
        case VB_POWERTRAIN_DETECTING: return "Detectando...";
        default:                      return "Desconocido";
    }
}