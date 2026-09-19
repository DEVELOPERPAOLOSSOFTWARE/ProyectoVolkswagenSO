/*
 
 */

.section ".text.boot"
.global _start

/* Numero maximo de cores que soportamos reservar stack para ellos.
 * Ajustar cuando tengamos el spec real del SoC objetivo. */
.equ MAX_CORES, 4
.equ STACK_SIZE_PER_CORE, 0x4000   /* 16KB por core */

_start:
    /* =========================================================
     * PASO 1 -- Identificar el core actual (MPIDR_EL1)
     * ========================================================= */
    mrs     x19, mpidr_el1
    and     x19, x19, #0xFF         // x19 = core_id (se preserva todo el arranque)

    /* =========================================================
     * PASO 2 -- Bajar a EL1 si arrancamos en un nivel mas alto
     * ========================================================= */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #3
    b.eq    el3_to_el1
    cmp     x0, #2
    b.eq    el2_to_el1
    b       el1_entry               // Ya estamos en EL1

el3_to_el1:
    mov     x0, #0x5b1              // SCR_EL3: NS=1, RW=1, HCE=1, SMD=1
    msr     scr_el3, x0
    mov     x0, #0x3c9              // SPSR: EL1h, DAIF enmascarado
    msr     spsr_el3, x0
    adr     x0, el2_to_el1_check
    msr     elr_el3, x0
    eret

el2_to_el1_check:
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.eq    el2_to_el1
    b       el1_entry

el2_to_el1:
    mov     x0, #0x33ff             // CPTR_EL2: no trap de acceso a FP/SIMD desde EL1
    msr     cptr_el2, x0
    mov     x0, #(1 << 31)          // HCR_EL2.RW = 1 -> EL1 corre en AArch64
    msr     hcr_el2, x0
    mov     x0, #0x3c5              // SPSR: EL1h, DAIF enmascarado
    msr     spsr_el2, x0
    adr     x0, el1_entry
    msr     elr_el2, x0
    eret

el1_entry:
    /* =========================================================
     * PASO 3 -- Configurar SCTLR_EL1 explicitamente
     * ========================================================= */
    mrs     x0, sctlr_el1
    bic     x0, x0, #(1 << 0)       // M  = 0: MMU off
    bic     x0, x0, #(1 << 2)       // C  = 0: D-cache off
    bic     x0, x0, #(1 << 12)      // I  = 0: I-cache off
    bic     x0, x0, #(1 << 1)       // A  = 0: alignment check off (por ahora)
    msr     sctlr_el1, x0
    isb

    /* Permitir acceso a registros FP/SIMD desde EL1 sin trap. */
    mov     x0, #(0x3 << 20)        // CPACR_EL1.FPEN = 0b11
    msr     cpacr_el1, x0
    isb

    /* =========================================================
     * PASO 4 -- Instalar la tabla de vectores de excepcion
     * ========================================================= */
    adr     x0, exception_vector_table
    msr     vbar_el1, x0
    isb

    /* =========================================================
     * PASO 5 -- Stack propio por core
     * ========================================================= */
    ldr     x0, =__stack_base
    mov     x1, #STACK_SIZE_PER_CORE
    add     x2, x19, #1
    mul     x1, x1, x2
    add     x0, x0, x1
    mov     sp, x0

    cbnz    x19, secondary_core_park

    /* =========================================================
     * PASO 6 -- Limpiar .bss (solo core 0, una vez)
     * ========================================================= */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
    cmp     x0, x1
    b.ge    identify_cpu

clear_bss_loop:
    str     xzr, [x0], #8
    cmp     x0, x1
    b.lt    clear_bss_loop

identify_cpu:
    /* =========================================================
     * PASO 7 -- Leer MIDR_EL1 (identificacion del procesador)
     * ========================================================= */
    mrs     x20, midr_el1
    ldr     x0, =__cpu_midr_storage
    str     x20, [x0]

    /* =========================================================
     * PASO 8 -- Saltar al kernel en C
     * ========================================================= */
    bl      kernel_main

    b       panic_halt

secondary_core_park:
    wfe
    b       secondary_core_park

panic_halt:
    wfe
    b       panic_halt

/*
 * =====================================================================
 * TABLA DE VECTORES DE EXCEPCION
 * =====================================================================
 * ARM64 exige 16 entradas de 128 bytes cada una (2KB total, alineado
 * a 2KB). Cada entrada maneja una combinacion de (nivel de origen,
 * tipo de excepcion). Por ahora TODAS saltan a un handler generico
 * que imprime diagnostico y detiene el sistema -- esto se ira
 * reemplazando por handlers reales (IRQ del scheduler, page faults,
 * llamadas de sistema) conforme el kernel crezca.
 * =====================================================================
 */
.align 11                              /* 2^11 = 2048 = alineacion a 2KB */
.global exception_vector_table
exception_vector_table:

vector_entry_sync_el1_sp0:
    b       handler_generic
.align 7
vector_entry_irq_el1_sp0:
    b       handler_generic
.align 7
vector_entry_fiq_el1_sp0:
    b       handler_generic
.align 7
vector_entry_serror_el1_sp0:
    b       handler_generic
.align 7

vector_entry_sync_el1_sp1:
    b       handler_sync
.align 7
vector_entry_irq_el1_sp1:
    b       handler_irq
.align 7
vector_entry_fiq_el1_sp1:
    b       handler_generic
.align 7
vector_entry_serror_el1_sp1:
    b       handler_generic
.align 7

vector_entry_sync_el0_64:
    b       handler_generic
.align 7
vector_entry_irq_el0_64:
    b       handler_generic
.align 7
vector_entry_fiq_el0_64:
    b       handler_generic
.align 7
vector_entry_serror_el0_64:
    b       handler_generic
.align 7

vector_entry_sync_el0_32:
    b       handler_generic
.align 7
vector_entry_irq_el0_32:
    b       handler_generic
.align 7
vector_entry_fiq_el0_32:
    b       handler_generic
.align 7
vector_entry_serror_el0_32:
    b       handler_generic
.align 7

.global handler_sync
handler_sync:
    bl      exception_report_sync
    b       panic_halt

.global handler_irq
handler_irq:
    bl      exception_report_irq
    b       panic_halt

.global handler_generic
handler_generic:
    bl      exception_report_unhandled
    b       panic_halt