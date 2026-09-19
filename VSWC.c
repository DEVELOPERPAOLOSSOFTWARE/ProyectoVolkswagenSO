#include "vwfs.h"
#include "uart.h"
#include "timer.h"

/*
 * vwfs.c -- VW File System, implementacion completa
 * ============================================================================
 * Sin malloc, sin libc, sin Linux. Todo estatico y bare-metal.
 * La direccion base del storage viene de hw_config.h (via vwfs.h).
 * ============================================================================
 */

/* -------------------------------------------------------------------------
 * Acceso al storage
 * -------------------------------------------------------------------------
 * Toda lectura/escritura pasa por estas dos funciones.
 * En modo RAMDISK: acceso directo a RAM (HW_STORAGE_BASE + offset).
 * En modo eMMC/UFS: aqui se llama al driver del controlador de storage.
 * SOLO este bloque cambia cuando migramos de RAMDISK a hardware real.
 * ------------------------------------------------------------------------- */

static inline unsigned char *storage_ptr(unsigned int offset)
{
#if HW_STORAGE_TYPE == HW_STORAGE_TYPE_RAMDISK
    return (unsigned char *)((unsigned long)HW_STORAGE_BASE + (unsigned long)offset);
#else
    (void)offset;
    return (unsigned char *)0;
#endif
}

static void storage_read(unsigned int offset, void *buf, unsigned int len)
{
    unsigned char *src = storage_ptr(offset);
    unsigned char *dst = (unsigned char *)buf;
    for (unsigned int i = 0; i < len; i++) dst[i] = src[i];
}

static void storage_write(unsigned int offset, const void *buf, unsigned int len)
{
    unsigned char *dst = storage_ptr(offset);
    const unsigned char *src = (const unsigned char *)buf;
    for (unsigned int i = 0; i < len; i++) dst[i] = src[i];
}

static void storage_zero(unsigned int offset, unsigned int len)
{
    unsigned char *dst = storage_ptr(offset);
    for (unsigned int i = 0; i < len; i++) dst[i] = 0;
}

/* -------------------------------------------------------------------------
 * CRC32 -- checksum para detectar corrupcion
 * -------------------------------------------------------------------------
 * Implementacion propia, sin libc. Polinomio estandar 0xEDB88320.
 * ------------------------------------------------------------------------- */
static unsigned int crc32(const void *data, unsigned int len)
{
    const unsigned char *buf = (const unsigned char *)data;
    unsigned int crc = 0xFFFFFFFF;
    for (unsigned int i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320 : 0);
        }
    }
    return ~crc;
}

/* -------------------------------------------------------------------------
 * Estado en RAM del FS montado
 * ------------------------------------------------------------------------- */
static vwfs_superblock_t sb;           /* superbloque en RAM               */
static int               fs_mounted = 0;

/* Bitmap de bloques e inodos libres (en RAM, sincronizado al storage) */
#define MAX_BLOCKS   (HW_STORAGE_SIZE / HW_STORAGE_BLOCK_SIZE)
static unsigned char block_bitmap[MAX_BLOCKS / 8 + 1];
static unsigned char inode_bitmap[VWFS_MAX_INODES / 8 + 1];

/* -------------------------------------------------------------------------
 * Utilidades de bitmap
 * ------------------------------------------------------------------------- */
static void bitmap_set(unsigned char *bm, unsigned int idx)
{
    bm[idx / 8] |= (1 << (idx % 8));
}
static void bitmap_clear(unsigned char *bm, unsigned int idx)
{
    bm[idx / 8] &= ~(1 << (idx % 8));
}
static int bitmap_test(unsigned char *bm, unsigned int idx)
{
    return (bm[idx / 8] >> (idx % 8)) & 1;
}
static int bitmap_alloc(unsigned char *bm, unsigned int max)
{
    for (unsigned int i = 0; i < max; i++) {
        if (!bitmap_test(bm, i)) { bitmap_set(bm, i); return (int)i; }
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Journaling
 * -------------------------------------------------------------------------
 * journal_begin  : registrar una operacion como PENDING antes de hacerla
 * journal_commit : marcarla COMMITTED cuando termina exitosamente
 * journal_recover: al montar, completar o revertir operaciones PENDING
 * ------------------------------------------------------------------------- */
static unsigned short journal_seq = 0;

static void journal_begin(unsigned char op, unsigned int target_id,
                           unsigned int old_val, unsigned int new_val)
{
    vwfs_journal_entry_t entry;
    entry.state     = VWFS_JRNL_STATE_PENDING;
    entry.op        = op;
    entry.seq       = journal_seq++;
    entry.target_id = target_id;
    entry.old_value = old_val;
    entry.new_value = new_val;
    entry.checksum  = crc32(&entry, sizeof(entry) - 4);

    /* Escribir al journal en storage (antes de hacer la operacion real) */
    unsigned int slot = entry.seq % VWFS_JOURNAL_ENTRIES;
    unsigned int off  = VWFS_JOURNAL_OFFSET +
                        slot * sizeof(vwfs_journal_entry_t);
    storage_write(off, &entry, sizeof(entry));
}

static void journal_commit(unsigned short seq)
{
    unsigned int slot = seq % VWFS_JOURNAL_ENTRIES;
    unsigned int off  = VWFS_JOURNAL_OFFSET +
                        slot * sizeof(vwfs_journal_entry_t);
    vwfs_journal_entry_t entry;
    storage_read(off, &entry, sizeof(entry));
    entry.state    = VWFS_JRNL_STATE_COMMITTED;
    entry.checksum = crc32(&entry, sizeof(entry) - 4);
    storage_write(off, &entry, sizeof(entry));
}

static void journal_recover(void)
{
    uart_puts("[vwfs] Revisando journal para recovery...\n");
    int recovered = 0;

    for (unsigned int i = 0; i < VWFS_JOURNAL_ENTRIES; i++) {
        unsigned int off = VWFS_JOURNAL_OFFSET + i * sizeof(vwfs_journal_entry_t);
        vwfs_journal_entry_t entry;
        storage_read(off, &entry, sizeof(entry));

        /* Validar checksum */
        unsigned int crc = crc32(&entry, sizeof(entry) - 4);
        if (crc != entry.checksum) continue;

        if (entry.state == VWFS_JRNL_STATE_PENDING) {
            /* Operacion incompleta -- revertir al valor anterior */
            if (entry.op == VWFS_JRNL_OP_ALLOC_INODE) {
                bitmap_clear(inode_bitmap, entry.target_id);
                sb.free_inodes++;
            } else if (entry.op == VWFS_JRNL_OP_ALLOC_BLOCK) {
                bitmap_clear(block_bitmap, entry.target_id);
                sb.free_blocks++;
            }
            /* Marcar como revertida */
            entry.state    = VWFS_JRNL_STATE_FREE;
            entry.checksum = crc32(&entry, sizeof(entry) - 4);
            storage_write(off, &entry, sizeof(entry));
            recovered++;
        }
    }

    if (recovered > 0) {
        uart_puts("[vwfs] Journal: operaciones revertidas por crash.\n");
    } else {
        uart_puts("[vwfs] Journal: limpio, sin operaciones pendientes.\n");
    }
}

/* -------------------------------------------------------------------------
 * Inodos: leer y escribir del storage
 * ------------------------------------------------------------------------- */
static unsigned int inode_offset(unsigned int id)
{
    return VWFS_INODE_TABLE_OFFSET + id * sizeof(vwfs_inode_t);
}

static void inode_read(unsigned int id, vwfs_inode_t *out)
{
    storage_read(inode_offset(id), out, sizeof(vwfs_inode_t));
}

static void inode_write(unsigned int id, const vwfs_inode_t *inode)
{
    journal_begin(VWFS_JRNL_OP_WRITE_INODE, id, 0, 0);
    storage_write(inode_offset(id), inode, sizeof(vwfs_inode_t));
    journal_commit((unsigned short)(journal_seq - 1));
}

static int inode_alloc(void)
{
    int id = bitmap_alloc(inode_bitmap, VWFS_MAX_INODES);
    if (id < 0) return VWFS_ERR_FULL;
    journal_begin(VWFS_JRNL_OP_ALLOC_INODE, (unsigned int)id, 0, 1);
    sb.free_inodes--;
    journal_commit((unsigned short)(journal_seq - 1));
    return id;
}

static void inode_free(unsigned int id)
{
    journal_begin(VWFS_JRNL_OP_FREE_INODE, id, 1, 0);
    bitmap_clear(inode_bitmap, id);
    sb.free_inodes++;
    vwfs_inode_t empty;
    storage_zero(inode_offset(id), sizeof(vwfs_inode_t));
    (void)empty;
    journal_commit((unsigned short)(journal_seq - 1));
}

/* -------------------------------------------------------------------------
 * Bloques de datos: asignar y liberar
 * ------------------------------------------------------------------------- */
static unsigned int block_offset(unsigned int block_id)
{
    return VWFS_DATA_OFFSET + block_id * HW_STORAGE_BLOCK_SIZE;
}

static int block_alloc(void)
{
    unsigned int max_blocks = (HW_STORAGE_SIZE - VWFS_DATA_OFFSET)
                               / HW_STORAGE_BLOCK_SIZE;
    int id = bitmap_alloc(block_bitmap, max_blocks);
    if (id < 0) return VWFS_ERR_NOSPACE;
    journal_begin(VWFS_JRNL_OP_ALLOC_BLOCK, (unsigned int)id, 0, 1);
    storage_zero(block_offset((unsigned int)id), HW_STORAGE_BLOCK_SIZE);
    sb.free_blocks--;
    journal_commit((unsigned short)(journal_seq - 1));
    return id;
}

static void block_free(unsigned int id)
{
    journal_begin(VWFS_JRNL_OP_FREE_BLOCK, id, 1, 0);
    bitmap_clear(block_bitmap, id);
    sb.free_blocks++;
    journal_commit((unsigned short)(journal_seq - 1));
}

/* -------------------------------------------------------------------------
 * Utilidades de path
 * ------------------------------------------------------------------------- */
static int str_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int str_len(const char *s)
{
    int n = 0; while (s[n]) n++; return n;
}

/* Separar el path en directorio padre y nombre del archivo.
 * "/config/wifi.cfg" -> parent="/config", name="wifi.cfg" */
static void path_split(const char *path, char *parent, char *name)
{
    int len = str_len(path);
    int last_slash = 0;
    for (int i = 0; i < len; i++) if (path[i] == '/') last_slash = i;

    /* Copiar parte del padre */
    if (last_slash == 0) {
        parent[0] = '/'; parent[1] = '\0';
    } else {
        for (int i = 0; i < last_slash; i++) parent[i] = path[i];
        parent[last_slash] = '\0';
    }
    /* Copiar nombre */
    int ni = 0;
    for (int i = last_slash + 1; i < len; i++) name[ni++] = path[i];
    name[ni] = '\0';
}

/* Buscar un inodo por path absoluto. Retorna el ID o error. */
static int path_resolve(const char *path)
{
    if (path[0] != '/') return VWFS_ERR_NOENT;
    if (path[1] == '\0') return 0;  /* root */

    int current = 0;  /* empezar desde root */
    char segment[VWFS_MAX_NAME_LEN + 1];
    int  si = 0;
    int  pi = 1;  /* saltar el '/' inicial */

    while (1) {
        char c = path[pi];
        if (c == '/' || c == '\0') {
            if (si == 0) { if (c == '\0') break; pi++; continue; }
            segment[si] = '\0';
            si = 0;

            /* Buscar 'segment' en los inodos hijos de 'current' */
            int found = -1;
            for (unsigned int id = 0; id < VWFS_MAX_INODES; id++) {
                if (!bitmap_test(inode_bitmap, id)) continue;
                vwfs_inode_t inode;
                inode_read(id, &inode);
                if (inode.parent_inode == (unsigned int)current
                    && str_eq(inode.name, segment)) {
                    found = (int)id; break;
                }
            }
            if (found < 0) return VWFS_ERR_NOENT;
            current = found;
            if (c == '\0') break;
        } else {
            if (si < VWFS_MAX_NAME_LEN) segment[si++] = c;
        }
        pi++;
    }
    return current;
}

/* -------------------------------------------------------------------------
 * vwfs_format -- formatear el storage como VWFS
 * ------------------------------------------------------------------------- */
int vwfs_format(void)
{
    uart_puts("[vwfs] Formateando storage como VWFS...\n");

    unsigned int total_blocks = (HW_STORAGE_SIZE - VWFS_DATA_OFFSET)
                                 / HW_STORAGE_BLOCK_SIZE;

    /* Limpiar zona del superbloque */
    storage_zero(VWFS_SUPERBLOCK_OFFSET, sizeof(vwfs_superblock_t));

    vwfs_superblock_t new_sb;
    volatile unsigned char *p = (volatile unsigned char *)&new_sb;
    for (unsigned int i = 0; i < sizeof(vwfs_superblock_t); i++) p[i] = 0;

    new_sb.magic               = VWFS_MAGIC;
    new_sb.version             = 1;
    new_sb.block_size          = HW_STORAGE_BLOCK_SIZE;
    new_sb.total_blocks        = total_blocks;
    new_sb.free_blocks         = total_blocks;
    new_sb.total_inodes        = VWFS_MAX_INODES;
    new_sb.free_inodes         = VWFS_MAX_INODES - 1;  /* root usa 1 */
    new_sb.journal_offset      = VWFS_JOURNAL_OFFSET;
    new_sb.journal_size        = 64 * 1024;
    new_sb.inode_table_offset  = VWFS_INODE_TABLE_OFFSET;
    new_sb.data_offset         = VWFS_DATA_OFFSET;
    new_sb.root_inode          = 0;
    new_sb.mount_time          = timer_get_ticks();
    new_sb.last_write_time     = new_sb.mount_time;
    new_sb.mount_count         = 0;
    new_sb.state               = 0;  /* limpio */
    new_sb.checksum            = crc32(&new_sb, sizeof(new_sb) - sizeof(unsigned int) - sizeof(new_sb.reserved));

    storage_write(VWFS_SUPERBLOCK_OFFSET, &new_sb, sizeof(new_sb));

    /* Limpiar journal e inodos */
    storage_zero(VWFS_JOURNAL_OFFSET, 64 * 1024);
    storage_zero(VWFS_INODE_TABLE_OFFSET,
                 VWFS_MAX_INODES * sizeof(vwfs_inode_t));

    /* Crear inodo raiz (ID 0, directorio "/") */
    vwfs_inode_t root;
    volatile unsigned char *rp = (volatile unsigned char *)&root;
    for (unsigned int i = 0; i < sizeof(vwfs_inode_t); i++) rp[i] = 0;

    root.id           = 0;
    root.type         = VWFS_INODE_DIR;
    root.permissions  = VWFS_PERM_READ | VWFS_PERM_WRITE | VWFS_PERM_EXEC;
    root.size         = 0;
    root.block_count  = 0;
    root.create_time  = timer_get_ticks();
    root.modify_time  = root.create_time;
    root.access_time  = root.create_time;
    root.parent_inode = 0;  /* root es su propio padre */
    root.name[0]      = '/'; root.name[1] = '\0';
    for (int i = 0; i < VWFS_DIRECT_BLOCKS; i++) root.direct[i] = 0xFFFFFFFF;
    root.indirect        = 0xFFFFFFFF;
    root.double_indirect = 0xFFFFFFFF;
    root.checksum = crc32(&root, sizeof(root) - sizeof(unsigned int));

    storage_write(VWFS_INODE_TABLE_OFFSET, &root, sizeof(root));

    uart_puts("[vwfs] Formato completado. Root inode creado.\n");
    return VWFS_OK;
}

/* -------------------------------------------------------------------------
 * vwfs_mount -- montar el FS (con recovery del journal)
 * ------------------------------------------------------------------------- */
int vwfs_mount(void)
{
    uart_puts("[vwfs] Montando VWFS...\n");

    /* Leer y validar el superbloque */
    storage_read(VWFS_SUPERBLOCK_OFFSET, &sb, sizeof(sb));

    if (sb.magic != VWFS_MAGIC) {
        uart_puts("[vwfs] ERROR: magic invalido -- storage no es VWFS.\n");
        return VWFS_ERR_MAGIC;
    }
    uart_puts("[vwfs] Superbloque valido (magic OK).\n");

    /* Inicializar bitmaps vacios */
    for (unsigned int i = 0; i < sizeof(block_bitmap); i++) block_bitmap[i] = 0;
    for (unsigned int i = 0; i < sizeof(inode_bitmap); i++) inode_bitmap[i] = 0;

    /* Reconstruir bitmaps escaneando la tabla de inodos */
    bitmap_set(inode_bitmap, 0);  /* root siempre ocupado */
    for (unsigned int id = 1; id < VWFS_MAX_INODES; id++) {
        vwfs_inode_t inode;
        inode_read(id, &inode);
        if (inode.type != VWFS_INODE_FREE && inode.id == id) {
            bitmap_set(inode_bitmap, id);
            for (int b = 0; b < VWFS_DIRECT_BLOCKS; b++) {
                if (inode.direct[b] != 0xFFFFFFFF) {
                    bitmap_set(block_bitmap, inode.direct[b]);
                }
            }
        }
    }

    /* Recovery del journal */
    journal_recover();

    /* Actualizar estado del superbloque */
    sb.state = 1;  /* montado */
    sb.mount_count++;
    sb.mount_time = timer_get_ticks();
    storage_write(VWFS_SUPERBLOCK_OFFSET, &sb, sizeof(sb));

    fs_mounted = 1;
    uart_puts("[vwfs] VWFS montado correctamente.\n");
    return VWFS_OK;
}

/* -------------------------------------------------------------------------
 * vwfs_unmount -- desmontar limpiamente
 * ------------------------------------------------------------------------- */
int vwfs_unmount(void)
{
    if (!fs_mounted) return VWFS_OK;
    sb.state           = 0;  /* limpio */
    sb.last_write_time = timer_get_ticks();
    sb.checksum        = crc32(&sb, sizeof(sb) - sizeof(unsigned int) - sizeof(sb.reserved));
    storage_write(VWFS_SUPERBLOCK_OFFSET, &sb, sizeof(sb));
    fs_mounted = 0;
    uart_puts("[vwfs] VWFS desmontado limpiamente.\n");
    return VWFS_OK;
}

/* -------------------------------------------------------------------------
 * vwfs_mkdir -- crear un directorio
 * ------------------------------------------------------------------------- */
int vwfs_mkdir(const char *path)
{
    if (!fs_mounted) return VWFS_ERR_MAGIC;

    /* Verificar que no exista ya */
    if (path_resolve(path) >= 0) return VWFS_ERR_EXIST;

    /* Separar parent y nombre */
    char parent_path[256], name[VWFS_MAX_NAME_LEN + 1];
    path_split(path, parent_path, name);

    int parent_id = path_resolve(parent_path);
    if (parent_id < 0) return VWFS_ERR_NOENT;

    /* Crear inodo para el nuevo directorio */
    int id = inode_alloc();
    if (id < 0) return VWFS_ERR_FULL;

    vwfs_inode_t inode;
    inode.id          = (unsigned int)id;
    inode.type        = VWFS_INODE_DIR;
    inode.permissions = VWFS_PERM_READ | VWFS_PERM_WRITE | VWFS_PERM_EXEC;
    inode.size        = 0;
    inode.block_count = 0;
    inode.create_time = timer_get_ticks();
    inode.modify_time = inode.create_time;
    inode.access_time = inode.create_time;
    inode.parent_inode = (unsigned int)parent_id;
    for (int b = 0; b < VWFS_DIRECT_BLOCKS; b++) inode.direct[b] = 0xFFFFFFFF;
    inode.indirect = 0xFFFFFFFF;
    inode.double_indirect = 0xFFFFFFFF;

    /* Copiar nombre */
    int ni = 0;
    while (name[ni] && ni < VWFS_MAX_NAME_LEN) {
        inode.name[ni] = name[ni]; ni++;
    }
    inode.name[ni] = '\0';
    inode.checksum = crc32(&inode, sizeof(inode) - sizeof(unsigned int));

    inode_write((unsigned int)id, &inode);
    return VWFS_OK;
}

/* -------------------------------------------------------------------------
 * vwfs_open -- abrir un archivo (crear si no existe y write=1)
 * ------------------------------------------------------------------------- */
int vwfs_open(const char *path, vwfs_file_t *file, int write)
{
    if (!fs_mounted) return VWFS_ERR_MAGIC;

    int id = path_resolve(path);

    if (id < 0 && write) {
        /* Crear el archivo */
        char parent_path[256], name[VWFS_MAX_NAME_LEN + 1];
        path_split(path, parent_path, name);
        int parent_id = path_resolve(parent_path);
        if (parent_id < 0) return VWFS_ERR_NOENT;

        id = inode_alloc();
        if (id < 0) return VWFS_ERR_FULL;

        vwfs_inode_t inode;
        inode.id           = (unsigned int)id;
        inode.type         = VWFS_INODE_FILE;
        inode.permissions  = VWFS_PERM_READ | VWFS_PERM_WRITE;
        inode.size         = 0;
        inode.block_count  = 0;
        inode.create_time  = timer_get_ticks();
        inode.modify_time  = inode.create_time;
        inode.access_time  = inode.create_time;
        inode.parent_inode = (unsigned int)parent_id;
        for (int b = 0; b < VWFS_DIRECT_BLOCKS; b++) inode.direct[b] = 0xFFFFFFFF;
        inode.indirect = 0xFFFFFFFF;
        inode.double_indirect = 0xFFFFFFFF;

        int ni = 0;
        while (name[ni] && ni < VWFS_MAX_NAME_LEN) {
            inode.name[ni] = name[ni]; ni++;
        }
        inode.name[ni] = '\0';
        inode.checksum = crc32(&inode, sizeof(inode) - sizeof(unsigned int));
        inode_write((unsigned int)id, &inode);

    } else if (id < 0) {
        return VWFS_ERR_NOENT;
    }

    file->inode_id = (unsigned int)id;
    file->position = 0;
    file->writable = (unsigned char)write;
    file->valid    = 1;
    return VWFS_OK;
}

/* -------------------------------------------------------------------------
 * vwfs_close -- cerrar un archivo
 * ------------------------------------------------------------------------- */
int vwfs_close(vwfs_file_t *file)
{
    if (!file || !file->valid) return VWFS_ERR_NOENT;
    file->valid = 0;
    return VWFS_OK;
}

/* -------------------------------------------------------------------------
 * vwfs_write -- escribir datos en un archivo
 * ------------------------------------------------------------------------- */
int vwfs_write(vwfs_file_t *file, const void *buf, unsigned int len)
{
    if (!file || !file->valid || !file->writable) return VWFS_ERR_NOENT;

    vwfs_inode_t inode;
    inode_read(file->inode_id, &inode);

    unsigned int written = 0;
    const unsigned char *src = (const unsigned char *)buf;

    while (written < len) {
        unsigned int block_idx = file->position / HW_STORAGE_BLOCK_SIZE;
        unsigned int block_off = file->position % HW_STORAGE_BLOCK_SIZE;

        if (block_idx >= VWFS_DIRECT_BLOCKS) break;  /* sin indirecto aun */

        /* Asignar bloque si no existe */
        if (inode.direct[block_idx] == 0xFFFFFFFF) {
            int new_block = block_alloc();
            if (new_block < 0) break;
            inode.direct[block_idx] = (unsigned int)new_block;
            inode.block_count++;
        }

        unsigned int space = HW_STORAGE_BLOCK_SIZE - block_off;
        unsigned int to_write = len - written;
        if (to_write > space) to_write = space;

        unsigned int dst_off = block_offset(inode.direct[block_idx]) + block_off;
        storage_write(dst_off, src + written, to_write);

        written        += to_write;
        file->position += to_write;
        if (file->position > inode.size) inode.size = file->position;
    }

    inode.modify_time = timer_get_ticks();
    inode.checksum    = crc32(&inode, sizeof(inode) - sizeof(unsigned int));
    inode_write(file->inode_id, &inode);
    sb.last_write_time = timer_get_ticks();

    return (int)written;
}

/* -------------------------------------------------------------------------
 * vwfs_read -- leer datos de un archivo
 * ------------------------------------------------------------------------- */
int vwfs_read(vwfs_file_t *file, void *buf, unsigned int len)
{
    if (!file || !file->valid) return VWFS_ERR_NOENT;

    vwfs_inode_t inode;
    inode_read(file->inode_id, &inode);

    if (file->position >= inode.size) return 0;

    unsigned int available = inode.size - file->position;
    if (len > available) len = available;

    unsigned int red = 0;
    unsigned char *dst = (unsigned char *)buf;

    while (red < len) {
        unsigned int block_idx = file->position / HW_STORAGE_BLOCK_SIZE;
        unsigned int block_off = file->position % HW_STORAGE_BLOCK_SIZE;

        if (block_idx >= VWFS_DIRECT_BLOCKS) break;
        if (inode.direct[block_idx] == 0xFFFFFFFF) break;

        unsigned int space    = HW_STORAGE_BLOCK_SIZE - block_off;
        unsigned int to_read  = len - red;
        if (to_read > space) to_read = space;

        unsigned int src_off = block_offset(inode.direct[block_idx]) + block_off;
        storage_read(src_off, dst + red, to_read);

        red            += to_read;
        file->position += to_read;
    }

    inode.access_time = timer_get_ticks();
    inode_write(file->inode_id, &inode);
    return (int)red;
}

/* -------------------------------------------------------------------------
 * vwfs_seek -- mover el cursor de lectura/escritura
 * ------------------------------------------------------------------------- */
int vwfs_seek(vwfs_file_t *file, unsigned int pos)
{
    if (!file || !file->valid) return VWFS_ERR_NOENT;
    file->position = pos;
    return VWFS_OK;
}

/* -------------------------------------------------------------------------
 * vwfs_delete -- eliminar un archivo
 * ------------------------------------------------------------------------- */
int vwfs_delete(const char *path)
{
    int id = path_resolve(path);
    if (id < 0) return VWFS_ERR_NOENT;

    vwfs_inode_t inode;
    inode_read((unsigned int)id, &inode);

    /* Liberar bloques de datos */
    for (int b = 0; b < VWFS_DIRECT_BLOCKS; b++) {
        if (inode.direct[b] != 0xFFFFFFFF) {
            block_free(inode.direct[b]);
        }
    }

    inode_free((unsigned int)id);
    return VWFS_OK;
}

/* -------------------------------------------------------------------------
 * vwfs_listdir -- listar contenido de un directorio
 * ------------------------------------------------------------------------- */
int vwfs_listdir(const char *path, vwfs_dir_entry_t *entries, int max)
{
    int dir_id = path_resolve(path);
    if (dir_id < 0) return VWFS_ERR_NOENT;

    int count = 0;
    for (unsigned int id = 0; id < VWFS_MAX_INODES && count < max; id++) {
        if (!bitmap_test(inode_bitmap, id)) continue;
        vwfs_inode_t inode;
        inode_read(id, &inode);
        if (inode.parent_inode == (unsigned int)dir_id && id != (unsigned int)dir_id) {
            entries[count].inode_id = id;
            entries[count].type     = inode.type;
            int ni = 0;
            while (inode.name[ni] && ni < VWFS_MAX_NAME_LEN) {
                entries[count].name[ni] = inode.name[ni]; ni++;
            }
            entries[count].name[ni] = '\0';
            entries[count].name_len = (unsigned char)ni;
            count++;
        }
    }
    return count;
}

/* -------------------------------------------------------------------------
 * vwfs_stat -- obtener info de un archivo o directorio
 * ------------------------------------------------------------------------- */
int vwfs_stat(const char *path, vwfs_inode_t *out_inode)
{
    int id = path_resolve(path);
    if (id < 0) return VWFS_ERR_NOENT;
    inode_read((unsigned int)id, out_inode);
    return VWFS_OK;
}

int vwfs_free_blocks(void)  { return (int)sb.free_blocks;  }
int vwfs_free_inodes(void)  { return (int)sb.free_inodes;  }