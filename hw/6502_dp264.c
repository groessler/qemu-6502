/*
 * QEMU MOS 6502 hardware system emulator.
 *
 */

#include "hw.h"
#include "boards.h"
#include "loader.h"
#include "sysemu.h"
#include "exec-memory.h"

#include "console.h"
#include "qemu-option.h"
#include "qemu-char.h"
#include "6502_keyboard.h"
#include "6502_timer.h"

#define BIOS_FILENAME      "6502_bios.rom"

// NOTE: I/O addresses are relative to start of I/O region
#define KEYB_READ_ADDR      0x00
#define SCREEN_WRITE_ADDR   0x00
#define CON_ECHO_WRITE_ADDR 0x01
#define TIMER_READ_ADDR     0x02
#define TIMER_WRITE_ADDR    0x02


static CPUState *cpu;
static CharDriverState *console;

static int can_read_handler(void *opaque)
{
    return 1;
}

// This is called when a key pressed by the user is delivered to our console
static void read_handler(void *opaque, const uint8_t* data, int datalen)
{
    int i;
    for(i = 0; i < datalen; i++) {
        if(data[i] == '/') {    // IRQ
            cpu_interrupt(cpu, CPU_INTERRUPT_IRQ);
        } else if(data[i] == '*') { // NMI
            cpu_interrupt(cpu, CPU_INTERRUPT_NMI);
        } else if(data[i] == '-') { // RST
            cpu_interrupt(cpu, CPU_INTERRUPT_RESET);
        } else {
            write_char(data[i]);
        }
    }
}


static void timer_callback(void)
{
    cpu_interrupt(cpu, CPU_INTERRUPT_IRQ);
}




static uint64_t io_read(void *opaque, target_phys_addr_t addr, unsigned size)
{
    switch(addr) {
        case KEYB_READ_ADDR:
            return read_char();
            break;

        case TIMER_READ_ADDR:
            return get_timer_value();
            break;

        default:
            fprintf(stderr, "Reading IO address %llu.\n", (unsigned long long)addr);
            break;
    }

    return 0;
}


static void io_write(void *opaque, target_phys_addr_t addr, uint64_t value, unsigned size)
{
    uint8_t c;
    switch(addr) {
        case SCREEN_WRITE_ADDR:
            c = (uint8_t)value;
            qemu_chr_fe_write(console, (uint8_t*)&c, 1);
            if(c == '\n') {
                c = '\r';
                qemu_chr_fe_write(console, (uint8_t*)&c, 1);
            }
            break;

        case CON_ECHO_WRITE_ADDR:   // enable or disable console echo
            qemu_chr_fe_set_echo(console, (value != 0));
            break;

        case TIMER_WRITE_ADDR:
            set_timer_value(value);
            break;

        default:
            fprintf(stderr, "Writting %llu in IO address %llu.\n", (unsigned long long)value, (unsigned long long)addr);
            break;
    }
}




static const MemoryRegionOps io_ops = {
    .read = io_read,
    .write = io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = 1,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = 1,
    },
};



static void mos6502_init(ram_addr_t ram_size,
                         const char *boot_device,
                         const char *kernel_filename,
                         const char *kernel_cmdline,
                         const char *initrd_filename,
                         const char *cpu_model)
{
    cpu = cpu_init(NULL);
    cpu->pc = 0x1000;   // Address where to start execution

    MemoryRegion *address_space = get_system_memory();

    /*
     * Address Range  |   Function    |       Size
     * ---------------+---------------+----------------------
     * $0000 - $0FFF  |     RAM       |    4096 bytes
     * $1000 - $1FFF  |     ROM       |    4096 bytes
     * $2000 - $FDFF  |     RAM       |   56832 bytes
     * $FE00 - $FEFF  |     I/O       |     256 bytes
     * $FF00 - $FFFF  |     RAM       |     256 bytes
     */

    // RAM
    MemoryRegion *ram1 = g_new(MemoryRegion, 1);
    memory_region_init_ram(ram1, "6502.ram1", 0x0FFF - 0x0000 + 1);
    vmstate_register_ram_global(ram1);
    memory_region_add_subregion(address_space, 0x0000, ram1);

    // ROM
    MemoryRegion *rom = g_new(MemoryRegion, 1);
    memory_region_init_ram(rom, "6502.rom", 0x1FFF - 0x1000 + 1);
    memory_region_set_readonly(rom, true);
    vmstate_register_ram_global(rom);
    memory_region_add_subregion(address_space, 0x1000, rom);

    // More RAM
    MemoryRegion *ram2 = g_new(MemoryRegion, 1);
    memory_region_init_ram(ram2, "6502.ram2", 0xFFEF - 0x2000 + 1);
    vmstate_register_ram_global(ram2);
    memory_region_add_subregion(address_space, 0x2000, ram2);

    // I/O
    MemoryRegion *io = g_new(MemoryRegion, 1);
    memory_region_init_io(io, &io_ops, NULL, "6502.io", 0xFEFF - 0xFE00 + 1);
    memory_region_add_subregion(address_space, 0xFE00, io);

    // Even more RAM
    MemoryRegion *ram3 = g_new(MemoryRegion, 1);
    memory_region_init_ram(ram3, "6502.ram3", 0xFFFF - 0xFF00 + 1);
    vmstate_register_ram_global(ram3);
    memory_region_add_subregion(address_space, 0xFF00, ram3);


    // Load ROM
    if(bios_name == NULL) {
        bios_name = BIOS_FILENAME;
    }
    // 4 KB of BIOS starting at 0x1000
    if(load_image_targphys(bios_name, 0x1000, 0x1FFF - 0x1000 + 1) < 0) {
        fprintf(stderr, "Error loading bios file: %s\n", bios_name);
        exit(-1);
    }

    // Create console
    static QemuOptsList opts_list = {
        .name = "6502.console",
        .head = QTAILQ_HEAD_INITIALIZER(opts_list.head),
        .desc = {
            {
                .name = "cols",
                .type = QEMU_OPT_NUMBER,
            },{
                .name = "rows",
                .type = QEMU_OPT_NUMBER,
            },
            { /* end of list */ }
        },
    };

    QemuOpts *console_options = qemu_opts_create(&opts_list, NULL, 0);
    qemu_opt_set(console_options, "cols", "80");
    qemu_opt_set(console_options, "rows", "25");
    console = text_console_init(console_options);

    DisplayState *ds = get_displaystate();
    text_consoles_set_display(ds);

    console_select(3);

    qemu_chr_add_handlers(console, can_read_handler, read_handler, NULL, NULL);

    init_keyboard();
    init_timer(&timer_callback);

}



static QEMUMachine mos6502_machine = {
    .name = "mos6502_dummy",
    .desc = "MOS 6502 CPU",
    .init = mos6502_init,
    .max_cpus = 1,
    .is_default = 1,
};

static void mos6502_machine_init(void)
{
    qemu_register_machine(&mos6502_machine);
}

machine_init(mos6502_machine_init);
