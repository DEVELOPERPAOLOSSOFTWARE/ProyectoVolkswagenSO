#include "mmu.h"
#include "hw_config.h"
#include "uart.h"

/*
 * mmu.c -- Implementacion de la MMU (mapeo identidad, granulo 4KB)
 * ============================================================================
 *
 * ESTRATEGIA: mapeo identidad (direccion virtual == direccion fisica).
 * Es la forma mas simple y segura de arrancar una MMU -- el kernel sigue
 * viendo las mismas direcciones de siempre, pero AHORA con permisos y
 * atributos de cache reales aplicados por hardware. Cuando el proyecto
 * necesite espacios de memoria separados por proceso, esto se reemplaza
 * por tablas por-proceso -- pero esa es una fase posterior.
 *
 * Usamos bloques de NIVEL 1 (1GB cada uno) directamente como raiz de la
 * tabla de paginas. Esto simplifica todo: en vez de 3-4 niveles de tablas
 * (nivel 0 -> 1 -> 2 -> 3, bajando hasta paginas de 4KB), solo necesitamos
 * UN nivel con bloques gigantes, porque las regiones que mapeamos
 * (HW_DEVICE_REGION y HW_RAM_REGION) son grandes y no necesitan
 * granularidad fina todavia.
 *
 * ============================================================================
 * TABLA DE PAGINAS -- FORMATO DE UNA ENTRADA DE BLOQUE (nivel 1, 1GB)
 * ============================================================================
 * Bit 0    : Valid       (1 = entrada valida)
 * Bit 1    : Type        (0 = block descriptor, no table descriptor)
 * Bits 4:2 : AttrIndx    (indice al registro MAIR_EL1 -- que atributo usar)
 * Bit 5    : NS          (Non-secure, no aplica en nuestro caso -- 0)
 * Bits 7:6 : AP[2:1]     (permisos de acceso: 00 = RW solo EL1)
 * Bits 9:8 : SH          (shareability: 10 = Outer Shareable)
 * Bit 10   : AF          (Access Flag -- DEBE ser 1 o cada acceso genera fault)
 * Bit 54   : UXN         (Unprivileged Execute Never)
 * Bit 53   : PXN         (Privileged Execute Never -- 1 en la region device,
 *                          nunca se debe poder EJECUTAR codigo desde ahi)
 */

#define PTE_VALID           (1UL << 0)
#define PTE_BLOCK           (0UL << 1)   /* nivel 1/2 block descriptor */
#define PTE_AF              (1UL << 10)  /* Access Flag -- obligatorio */
#define PTE_SH_OUTER        (3UL << 8)   /* Outer Shareable */
#define PTE_AP_RW_EL1       (0UL << 6)   /* RW, solo EL1 (kernel) */
#define PTE_PXN             (1UL << 53)  /* Privileged Execute Never */
#define PTE_UXN             (1UL << 54)  /* Unprivileged Execute Never */

#define MAIR_IDX_NORMAL     0
#define MAIR_IDX_DEVICE     1

#define PTE_ATTR_NORMAL     (MAIR_IDX_NORMAL << 2)
#define PTE_ATTR_DEVICE     (MAIR_IDX_DEVICE << 2)

/* Bloque de RAM: ejecutable (ahi vive el kernel), cacheable */
#define PTE_RAM_BLOCK \
    (PTE_VALID | PTE_BLOCK | PTE_ATTR_NORMAL | PTE_AP_RW_EL1 | \
     PTE_SH_OUTER | PTE_AF | PTE_UXN)   /* sin PXN: el kernel SI ejecuta aqui */

/* Bloque de dispositivos (UART, GIC): NUNCA ejecutable, sin cache */
#define PTE_DEVICE_BLOCK \
    (PTE_VALID | PTE_BLOCK | PTE_ATTR_DEVICE | PTE_AP_RW_EL1 | \
     PTE_AF | PTE_PXN | PTE_UXN)

/*
 * Tabla de nivel 1: 512 entradas x 8 bytes = 4KB, alineada a 4KB
 * (requisito de hardware -- TTBR0_EL1 exige alineacion a 4KB minimo).
 * Cada entrada cubre 1GB de espacio de direcciones.
 */
static unsigned long page_table_l1[512] __attribute__((aligned(4096)));

static void map_region_1gb_blocks(unsigned long base, unsigned long size,
                                   unsigned long block_attrs)
{
    unsigned long index_start = base / 0x40000000UL;          /* 1GB por entrada */
    unsigned long num_blocks  = size  / 0x40000000UL;

    for (unsigned long i = 0; i < num_blocks; i++) {
        unsigned long block_base = (index_start + i) * 0x40000000UL;
        page_table_l1[index_start + i] = block_base | block_attrs;
    }
}

void mmu_init(void)
{
    uart_puts("[mmu] Iniciando configuracion de MMU...\n");

    /* -----------------------------------------------------------------
     * PASO 1 -- Limpiar la tabla de paginas (todas las entradas invalidas
     * por default; solo las regiones que mapeamos explicitamente abajo
     * quedan accesibles -- cualquier otra direccion genera fault, que
     * es exactamente el comportamiento que queremos por seguridad).
     * ----------------------------------------------------------------- */
    for (int i = 0; i < 512; i++) {
        page_table_l1[i] = 0;
    }

    /* -----------------------------------------------------------------
     * PASO 2 -- Mapear las regiones definidas en hw_config.h
     * ----------------------------------------------------------------- */
    map_region_1gb_blocks(HW_DEVICE_REGION_BASE, HW_DEVICE_REGION_SIZE,
                           PTE_DEVICE_BLOCK);
    map_region_1gb_blocks(HW_RAM_REGION_BASE, HW_RAM_REGION_SIZE,
                           PTE_RAM_BLOCK);

    uart_puts("[mmu] Tabla de paginas armada (mapeo identidad, bloques de 1GB).\n");

    /* -----------------------------------------------------------------
     * PASO 3 -- Configurar MAIR_EL1 (Memory Attribute Indirection Register)
     * -----------------------------------------------------------------
     * Define QUE significa cada AttrIndx usado en las entradas de la
     * tabla de paginas. Dos perfiles:
     *   Indice 0 (Normal) : 0xFF = Normal memory, Write-Back, cacheable,
     *                        Read/Write-Allocate en ambos niveles
     *   Indice 1 (Device) : 0x00 = Device-nGnRnE (non-Gathering,
     *                        non-Reordering, no Early write ack --
     *                        obligatorio para hardware mapeado en memoria,
     *                        cualquier optimizacion de cache aqui puede
     *                        hacer que el driver de UART/GIC falle)
     * ----------------------------------------------------------------- */
    unsigned long mair = (0xFFUL << (MAIR_IDX_NORMAL * 8)) |
                          (0x00UL << (MAIR_IDX_DEVICE * 8));
    __asm__ volatile("msr mair_el1, %0" :: "r"(mair));

    /* -----------------------------------------------------------------
     * PASO 4 -- Configurar TCR_EL1 (Translation Control Register)
     * -----------------------------------------------------------------
     * T0SZ = 25  -> espacio de direcciones virtuales de 2^(64-25) = 2^39
     *               = 512GB, usando TTBR0_EL1 (suficiente para identity
     *               map de RAM + dispositivos; se ajusta si el mapa de
     *               memoria real de VW necesita mas rango)
     * TG0  = 0   -> granulo de 4KB (valor estandar, mas compatible)
     * SH0  = 11  -> Inner Shareable para las tablas mismas
     * ORGN0/IRGN0 = 01 -> Write-Back cacheable para las tablas
     * EPD1 = 1   -> deshabilita TTBR1_EL1 (no lo usamos, solo TTBR0)
     * IPS  = 001 -> 40 bits de direccion fisica de salida (1TB, de sobra)
     * ----------------------------------------------------------------- */
    unsigned long tcr = (25UL << 0)   |  /* T0SZ */
                         (0UL  << 14) |  /* TG0 = 4KB */
                         (1UL  << 8)  |  /* IRGN0 = Write-Back */
                         (1UL  << 10) |  /* ORGN0 = Write-Back */
                         (3UL  << 12) |  /* SH0 = Inner Shareable */
                         (1UL  << 23) |  /* EPD1 = 1, TTBR1 deshabilitado */
                         (1UL  << 32);   /* IPS = 40 bits */
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr));

    /* -----------------------------------------------------------------
     * PASO 5 -- Cargar la direccion de la tabla en TTBR0_EL1
     * ----------------------------------------------------------------- */
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((unsigned long)page_table_l1));

    /* Barrera: asegurar que TODOS los registros anteriores (MAIR, TCR,
     * TTBR0) ya se escribieron antes de encender la MMU. Sin esto, hay
     * riesgo de que el procesador encienda la MMU con configuracion
     * a medio escribir -- comportamiento indefinido. */
    __asm__ volatile("isb");

    uart_puts("[mmu] MAIR_EL1 / TCR_EL1 / TTBR0_EL1 configurados.\n");

    /* -----------------------------------------------------------------
     * PASO 6 -- Encender la MMU y las caches (SCTLR_EL1)
     * -----------------------------------------------------------------
     * En boot.S dejamos estos 3 bits apagados a proposito. Este es el
     * unico lugar del kernel donde se encienden, de forma controlada,
     * DESPUES de que la tabla de paginas ya es valida.
     *   M = 1 : MMU enabled
     *   C = 1 : D-cache enabled
     *   I = 1 : I-cache enabled
     * ----------------------------------------------------------------- */
    unsigned long sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0);   /* M */
    sctlr |= (1UL << 2);   /* C */
    sctlr |= (1UL << 12);  /* I */
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile("isb");

    /* Si llegamos aqui y seguimos ejecutando normalmente, el mapeo
     * identidad funciono: la siguiente instruccion se busco en memoria
     * usando traduccion de direcciones, y como es mapeo identidad,
     * encontro exactamente el mismo codigo que antes de encender la MMU. */
    uart_puts("[mmu] MMU y caches activas. Ejecutando con traduccion de direcciones.\n");
}NNNNN