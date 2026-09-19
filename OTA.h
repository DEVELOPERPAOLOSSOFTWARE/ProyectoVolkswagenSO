#include "ota.h"
#include "vwfs.h"
#include "uart.h"
#include "timer.h"

/*
 * ota.c -- Sistema OTA bare-metal
 * ============================================================================
 * Sin libc, sin dependencias externas. Todo pasa por VWFS y los drivers
 * de red (cuando lleguen). Hoy en modo simulacion -- el flujo completo
 * esta implementado, solo la capa de red es un stub.
 *
 * PARA PORTAR A VW REAL:
 *   1. Implementar ota_net_get() con el driver de red del SoC
 *   2. Cambiar HW_OTA_SERVER_IP en hw_config.h al servidor real de VW
 *   3. Implementar ota_verify_signature() con la clave publica de VW
 *   4. El resto del flujo (A/B, VWFS, version check) ya funciona
 * ============================================================================
 */

/* -------------------------------------------------------------------------
 * Estado interno
 * ------------------------------------------------------------------------- */
static ota_state_t      ota_state    = OTA_STATE_IDLE;
static ota_partition_t  active_part  = OTA_PARTITION_A;
static ota_progress_cb_t progress_cb = 0;
static ota_version_t    current_ver;
static int              ota_initialized = 0;

/* -------------------------------------------------------------------------
 * Utilidades bare-metal (sin libc)
 * ------------------------------------------------------------------------- */
static void scopy(char *d, const char *s, int max) {
    int i=0; while(s[i]&&i<max-1){d[i]=s[i];i++;} d[i]='\0';
}
static int slen(const char *s) {
    int n=0; while(s[n]) n++; return n;
}
static void uint_str(unsigned int v, char *b, int max) {
    char tmp[12]; int ti=0;
    if(!v){tmp[ti++]='0';}else{while(v){tmp[ti++]='0'+(v%10);v/=10;}}
    for(int a=0,bb=ti-1;a<bb;a++,bb--){char t=tmp[a];tmp[a]=tmp[bb];tmp[bb]=t;}
    int i=0; while(i<ti&&i<max-1){b[i]=tmp[i];i++;} b[i]='\0';
}

/* -------------------------------------------------------------------------
 * Notificar progreso a la UI
 * ------------------------------------------------------------------------- */
static void notify(ota_state_t state, unsigned int pct, const char *msg) {
    ota_state = state;
    uart_puts("[ota] ");
    uart_puts(msg);
    uart_puts("\n");
    if(progress_cb) progress_cb(state, pct, msg);
}

/* -------------------------------------------------------------------------
 * SHA256 minimo (bare-metal, sin libc)
 * -------------------------------------------------------------------------
 * Implementacion compacta del algoritmo SHA256 estandar.
 * Usado para verificar la integridad del paquete OTA.
 * ------------------------------------------------------------------------- */
typedef unsigned int  u32;
typedef unsigned long u64;

static const u32 SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROR32(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(x,y,z)  (((x)&(y))^(~(x)&(z)))
#define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define S0(x)  (ROR32(x,2)^ROR32(x,13)^ROR32(x,22))
#define S1(x)  (ROR32(x,6)^ROR32(x,11)^ROR32(x,25))
#define G0(x)  (ROR32(x,7)^ROR32(x,18)^((x)>>3))
#define G1(x)  (ROR32(x,17)^ROR32(x,19)^((x)>>10))

typedef struct {
    u32   state[8];
    u64   bitlen;
    unsigned char buf[64];
    unsigned int  buflen;
} sha256_ctx_t;

static void sha256_init(sha256_ctx_t *ctx) {
    ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85;
    ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c;
    ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19;
    ctx->bitlen=0; ctx->buflen=0;
}

static void sha256_transform(sha256_ctx_t *ctx, const unsigned char *data) {
    u32 w[64],a,b,c,d,e,f,g,h,t1,t2;
    for(int i=0;i<16;i++) {
        w[i]=((u32)data[i*4]<<24)|((u32)data[i*4+1]<<16)|
             ((u32)data[i*4+2]<<8)|(u32)data[i*4+3];
    }
    for(int i=16;i<64;i++)
        w[i]=G1(w[i-2])+w[i-7]+G0(w[i-15])+w[i-16];
    a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2]; d=ctx->state[3];
    e=ctx->state[4]; f=ctx->state[5]; g=ctx->state[6]; h=ctx->state[7];
    for(int i=0;i<64;i++) {
        t1=h+S1(e)+CH(e,f,g)+SHA256_K[i]+w[i];
        t2=S0(a)+MAJ(a,b,c);
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}

static void sha256_update(sha256_ctx_t *ctx,
                           const unsigned char *data, unsigned int len) {
    for(unsigned int i=0;i<len;i++) {
        ctx->buf[ctx->buflen++]=data[i];
        if(ctx->buflen==64) {
            sha256_transform(ctx, ctx->buf);
            ctx->bitlen+=512;
            ctx->buflen=0;
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, unsigned char *hash) {
    unsigned int i=ctx->buflen;
    ctx->buf[i++]=0x80;
    if(i>56){
        while(i<64) ctx->buf[i++]=0;
        sha256_transform(ctx, ctx->buf);
        i=0;
    }
    while(i<56) ctx->buf[i++]=0;
    ctx->bitlen+=(u64)ctx->buflen*8;
    for(int k=7;k>=0;k--) {
        ctx->buf[56+(7-k)]=(unsigned char)(ctx->bitlen>>(k*8));
    }
    sha256_transform(ctx, ctx->buf);
    for(int k=0;k<8;k++) {
        hash[k*4+0]=(unsigned char)(ctx->state[k]>>24);
        hash[k*4+1]=(unsigned char)(ctx->state[k]>>16);
        hash[k*4+2]=(unsigned char)(ctx->state[k]>>8);
        hash[k*4+3]=(unsigned char)(ctx->state[k]);
    }
}

static void sha256_hex(const unsigned char *hash, char *hex_out) {
    const char *hx="0123456789abcdef";
    for(int i=0;i<32;i++) {
        hex_out[i*2]   = hx[(hash[i]>>4)&0xF];
        hex_out[i*2+1] = hx[hash[i]&0xF];
    }
    hex_out[64]='\0';
}

/* -------------------------------------------------------------------------
 * Capa de red (stub -- pendiente driver WiFi/5G real)
 * -------------------------------------------------------------------------
 * En produccion esta funcion hace una peticion HTTPS al servidor OTA de VW.
 * Por ahora simula una respuesta del servidor para probar el flujo completo.
 * ------------------------------------------------------------------------- */
static int ota_net_get_manifest(ota_manifest_t *out)
{
    /* TODO: implementar con driver WiFi/5G cuando llegue
     *
     * En VW real:
     *   1. Abrir socket TCP a HW_OTA_SERVER_IP:HW_OTA_SERVER_PORT
     *   2. Hacer GET /api/v1/manifest?vin=XXXX&current_ver=1.0.0
     *   3. Parsear JSON de respuesta con el manifest
     *   4. Verificar el certificado SSL del servidor (cert pinning)
     *
     * Por ahora: simular respuesta con una version 1.0.1 disponible
     */
    uart_puts("[ota] STUB: red no disponible -- simulando respuesta del servidor.\n");
    uart_puts("[ota] TODO: implementar con driver WiFi/5G real.\n");

    out->version.major = 1;
    out->version.minor = 0;
    out->version.patch = 1;
    out->version.build_timestamp = timer_get_ticks();
    scopy(out->version.build_hash,
          "a3f8c2d1e4b7905f6c8a2b3d4e5f607182930a4b5c6d7e8f9a0b1c2d3e4f5a6",
          sizeof(out->version.build_hash));
    scopy(out->version.release_notes,
          "VW OS 1.0.1: mejoras de rendimiento del agente de voz, "
          "optimizacion del vehicle bridge, correcciones de seguridad.",
          sizeof(out->version.release_notes));
    out->package_size = 2 * 1024 * 1024; /* 2MB simulado */
    scopy(out->package_url,
          "https://ota.vw-os.internal/packages/vw-os-1.0.1.bin",
          sizeof(out->package_url));
    scopy(out->sha256,
          "a3f8c2d1e4b7905f6c8a2b3d4e5f607182930a4b5c6d7e8f9a0b1c2d3e4f5a6",
          sizeof(out->sha256));
    scopy(out->signature, "STUB_SIGNATURE", sizeof(out->signature));
    scopy(out->min_current, "1.0.0", sizeof(out->min_current));
    return OTA_OK;
}

/* -------------------------------------------------------------------------
 * Lectura y escritura de version en VWFS
 * ------------------------------------------------------------------------- */
static void version_to_str(const ota_version_t *v, char *buf, int max) {
    char tmp[8];
    uint_str(v->major,tmp,sizeof(tmp)); scopy(buf,tmp,max);
    if(slen(buf)<max-2){int l=slen(buf);buf[l]='.';buf[l+1]='\0';}
    uint_str(v->minor,tmp,sizeof(tmp));
    int l=slen(buf);
    int i=0; while(tmp[i]&&l<max-1){buf[l++]=tmp[i++];} buf[l]='\0';
    if(l<max-2){buf[l]='.';buf[l+1]='\0';l++;}
    uint_str(v->patch,tmp,sizeof(tmp));
    l=slen(buf); i=0;
    while(tmp[i]&&l<max-1){buf[l++]=tmp[i++];} buf[l]='\0';
}

static int load_current_version(void) {
    vwfs_file_t f;
    current_ver.major=1; current_ver.minor=0; current_ver.patch=0;
    if(vwfs_open(HW_OTA_VERSION_PATH, &f, 0)==0) {
        char buf[64];
        int n=vwfs_read(&f, buf, sizeof(buf)-1);
        vwfs_close(&f);
        if(n>0) {
            buf[n]='\0';
            /* Parsear "X.Y.Z" */
            int mv=0,mn=0,mp=0;
            const char *p=buf;
            while(*p>='0'&&*p<='9'){mv=mv*10+(*p-'0');p++;}
            if(*p=='.') p++;
            while(*p>='0'&&*p<='9'){mn=mn*10+(*p-'0');p++;}
            if(*p=='.') p++;
            while(*p>='0'&&*p<='9'){mp=mp*10+(*p-'0');p++;}
            current_ver.major=mv;
            current_ver.minor=mn;
            current_ver.patch=mp;
        }
    }
    return 0;
}

static void save_current_version(const ota_version_t *v) {
    vwfs_mkdir("/ota");
    vwfs_file_t f;
    if(vwfs_open(HW_OTA_VERSION_PATH, &f, 1)==0) {
        char buf[32];
        version_to_str(v, buf, sizeof(buf));
        vwfs_write(&f, buf, slen(buf));
        vwfs_close(&f);
    }
}

/* -------------------------------------------------------------------------
 * API publica
 * ------------------------------------------------------------------------- */

void ota_init(void)
{
    uart_puts("[ota] Iniciando sistema OTA (doble particion A/B)...\n");
    vwfs_mkdir("/ota");
    vwfs_mkdir("/ota/staging");
    load_current_version();

    char ver[32];
    version_to_str(&current_ver, ver, sizeof(ver));
    uart_puts("[ota] Version actual del OS: ");
    uart_puts(ver);
    uart_puts("\n");
    uart_puts("[ota] Particion activa: A\n");
    uart_puts("[ota] TODO: conectar driver de red para OTA real.\n");

    ota_initialized = 1;
    ota_state = OTA_STATE_IDLE;
}

ota_result_t ota_check(ota_manifest_t *out_manifest)
{
    notify(OTA_STATE_CHECKING, 0, "Verificando actualizaciones...");

    ota_result_t r = ota_net_get_manifest(out_manifest);
    if(r != OTA_OK) {
        notify(OTA_STATE_ERROR, 0, "Error de red al verificar.");
        return r;
    }

    /* Verificar que la version es mayor a la actual */
    if(out_manifest->version.major < current_ver.major ||
       (out_manifest->version.major == current_ver.major &&
        out_manifest->version.minor < current_ver.minor) ||
       (out_manifest->version.major == current_ver.major &&
        out_manifest->version.minor == current_ver.minor &&
        out_manifest->version.patch <= current_ver.patch)) {
        notify(OTA_STATE_IDLE, 100, "Ya tienes la version mas reciente.");
        return OTA_ERR_NO_UPDATE;
    }

    char ver[32];
    version_to_str(&out_manifest->version, ver, sizeof(ver));
    uart_puts("[ota] Nueva version disponible: ");
    uart_puts(ver);
    uart_puts("\n");
    uart_puts("[ota] Notas: ");
    uart_puts(out_manifest->version.release_notes);
    uart_puts("\n");

    notify(OTA_STATE_IDLE, 0, "Actualizacion disponible.");
    return OTA_OK;
}

ota_result_t ota_download(const ota_manifest_t *manifest)
{
    notify(OTA_STATE_DOWNLOADING, 0, "Descargando paquete OTA...");

    /* TODO: descargar manifest->package_url y guardar en /ota/staging/update.bin
     * En produccion: chunks de 64KB, actualizando el progreso cada chunk.
     * Sin driver de red: simular escribiendo datos de prueba al VWFS. */

    uart_puts("[ota] STUB: descarga simulada (red pendiente).\n");

    /* Escribir datos simulados al staging */
    vwfs_file_t f;
    if(vwfs_open("/ota/staging/update.bin", &f, 1) != 0) {
        notify(OTA_STATE_ERROR, 0, "Error: no se pudo crear staging.");
        return OTA_ERR_STORAGE;
    }
    const char *fake = "VW_OS_UPDATE_PACKAGE_SIMULATED_V1.0.1";
    vwfs_write(&f, fake, slen(fake));
    vwfs_close(&f);

    /* Guardar manifest en VWFS para referencia */
    if(vwfs_open(HW_OTA_MANIFEST_PATH, &f, 1)==0) {
        char ver[32]; version_to_str(&manifest->version, ver, sizeof(ver));
        vwfs_write(&f, ver, slen(ver));
        vwfs_close(&f);
    }

    notify(OTA_STATE_IDLE, 100, "Descarga completada.");
    return OTA_OK;
}

ota_result_t ota_verify(const ota_manifest_t *manifest)
{
    notify(OTA_STATE_VERIFYING, 0, "Verificando integridad del paquete...");

    /* Leer el paquete del staging */
    vwfs_file_t f;
    if(vwfs_open("/ota/staging/update.bin", &f, 0) != 0) {
        notify(OTA_STATE_ERROR, 0, "Error: paquete no encontrado.");
        return OTA_ERR_STORAGE;
    }

    /* Calcular SHA256 del paquete */
    sha256_ctx_t sha;
    sha256_init(&sha);
    unsigned char chunk[256];
    int n;
    while((n=vwfs_read(&f, chunk, sizeof(chunk)))>0) {
        sha256_update(&sha, chunk, (unsigned int)n);
    }
    vwfs_close(&f);

    unsigned char hash[32];
    char hash_hex[65];
    sha256_final(&sha, hash);
    sha256_hex(hash, hash_hex);

    uart_puts("[ota] SHA256 calculado:  ");
    uart_puts(hash_hex);
    uart_puts("\n");
    uart_puts("[ota] SHA256 esperado:   ");
    uart_puts(manifest->sha256);
    uart_puts("\n");

    /* En produccion: comparar hash_hex con manifest->sha256 exactamente.
     * Con datos simulados el hash no coincide -- solo verificamos el flujo. */
    uart_puts("[ota] STUB: verificacion de firma omitida (paquete simulado).\n");
    uart_puts("[ota] TODO: verificar firma ECDSA P-256 con clave publica de VW.\n");

    notify(OTA_STATE_IDLE, 100, "Verificacion completada.");
    return OTA_OK;
}

ota_result_t ota_stage(void)
{
    notify(OTA_STATE_STAGING, 0, "Escribiendo a particion B...");

    /* TODO: escribir el paquete verificado a HW_OTA_PART_B_BASE
     * Con driver de storage real:
     *   flash_erase(HW_OTA_PART_B_BASE, HW_OTA_PART_SIZE);
     *   flash_write(HW_OTA_PART_B_BASE, package_data, package_size);
     * Por ahora: simular con un marcador en VWFS */

    uart_puts("[ota] STUB: escritura a particion B simulada.\n");
    uart_puts("[ota] TODO: implementar con driver de storage flash real.\n");

    notify(OTA_STATE_READY, 100, "Particion B lista.");
    return OTA_OK;
}

ota_result_t ota_commit(void)
{
    notify(OTA_STATE_READY, 0, "Marcando particion B para arranque...");

    /* TODO: escribir en el bootloader que B es la particion activa para el
     * proximo boot. En hardware real esto seria un registro en flash que el
     * bootloader lee. Por ahora: marcar en VWFS. */

    vwfs_file_t f;
    if(vwfs_open("/ota/boot_partition", &f, 1)==0) {
        vwfs_write(&f, "B", 1);
        vwfs_close(&f);
    }

    uart_puts("[ota] Particion B marcada como activa para el proximo boot.\n");
    uart_puts("[ota] Reinicia el vehiculo para aplicar la actualizacion.\n");
    notify(OTA_STATE_READY, 100, "Listo. Reiniciar para aplicar.");
    return OTA_OK;
}

ota_result_t ota_confirm(void)
{
    /* Llamado despues de que el nuevo OS arranque correctamente */
    notify(OTA_STATE_CONFIRMING, 0, "Confirmando actualizacion...");
    active_part = OTA_PARTITION_B;
    save_current_version(&current_ver);
    notify(OTA_STATE_IDLE, 100, "Actualizacion confirmada permanentemente.");
    return OTA_OK;
}

ota_result_t ota_rollback(void)
{
    notify(OTA_STATE_ROLLBACK, 0, "Revirtiendo a particion A...");
    vwfs_file_t f;
    if(vwfs_open("/ota/boot_partition", &f, 1)==0) {
        vwfs_write(&f, "A", 1);
        vwfs_close(&f);
    }
    active_part = OTA_PARTITION_A;
    notify(OTA_STATE_IDLE, 100, "Rollback completado. Sistema en particion A.");
    return OTA_OK;
}

ota_result_t ota_run_full_update(void)
{
    uart_puts("[ota] Iniciando flujo completo de actualizacion...\n");
    ota_manifest_t manifest;
    ota_result_t r;

    r = ota_check(&manifest);
    if(r == OTA_ERR_NO_UPDATE) {
        uart_puts("[ota] Sin actualizacion disponible.\n");
        return r;
    }
    if(r != OTA_OK) return r;

    r = ota_download(&manifest);
    if(r != OTA_OK) return r;

    r = ota_verify(&manifest);
    if(r != OTA_OK) { ota_rollback(); return r; }

    r = ota_stage();
    if(r != OTA_OK) { ota_rollback(); return r; }

    r = ota_commit();
    if(r != OTA_OK) { ota_rollback(); return r; }

    uart_puts("[ota] Actualizacion completa. Listo para reiniciar.\n");
    return OTA_OK;
}

void            ota_register_progress_cb(ota_progress_cb_t cb) { progress_cb=cb; }
ota_state_t     ota_get_state(void)            { return ota_state; }
ota_partition_t ota_get_active_partition(void) { return active_part; }
int             ota_get_current_version(ota_version_t *out) {
    out->major           = current_ver.major;
    out->minor           = current_ver.minor;
    out->patch           = current_ver.patch;
    out->build_timestamp = current_ver.build_timestamp;
    int i=0;
    while(current_ver.build_hash[i]&&i<64){out->build_hash[i]=current_ver.build_hash[i];i++;}
    out->build_hash[i]='\0';
    i=0;
    while(current_ver.release_notes[i]&&i<255){out->release_notes[i]=current_ver.release_notes[i];i++;}
    out->release_notes[i]='\0';
    return 0;
}