/*
Copyright (C) 2016-2019 The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file LICENSE for details.

All modifiactions are copyrighted to XPDevs and James Turner
XPDevs and James Turner use basekernel as a base where mods are added and updated
for the NexShell kernel

The current version is 1.2.9
*/

#include "kernel/types.h"
#include "kernel/error.h"
#include "kernel/ascii.h"
#include "kshell.h"
#include "console.h"
#include "string.h"
#include "rtc.h"
#include "kmalloc.h"
#include "page.h"
#include "process.h"
#include "main.h"
#include "fs.h"
#include "syscall_handler.h"
#include "clock.h"
#include "kernelcore.h"
#include "bcache.h"
#include "printf.h"
#include "graphics.h" // Include your graphics header

// define the start screen for when the gui starts
#define COLOR_BLUE  0x0000FF      // RGB hex for blue (blue channel max)
#define COLOR_WHITE 0xFFFFFF      // White color

// DO NOT TOUCH THESE
#define PM1a_CNT_BLK 0xB004      // Example ACPI PM1a control port (adjust per your ACPI)
#define SLP_TYP1    (0x5 << 10)  // Sleep type 1 for shutdown (example value)
#define SLP_EN      (1 << 13)

static int kshell_mount( const char *devname, int unit, const char *fs_type)
{
	if(current->ktable[KNO_STDDIR]) {
		printf("root filesystem already mounted, please unmount first\n");
		return -1;
	}

	struct device *dev = device_open(devname,unit);
	if(dev) {
		struct fs *fs = fs_lookup(fs_type);
		if(fs) {
			struct fs_volume *v = fs_volume_open(fs,dev);
			if(v) {
				struct fs_dirent *d = fs_volume_root(v);
				if(d) {
					current->ktable[KNO_STDDIR] = kobject_create_dir(d);
					return 0;
				} else {
					printf("mount: couldn't find root dir on %s unit %d!\n",device_name(dev),device_unit(dev));
					return -1;
				}
				fs_volume_close(v);
			} else {
				printf("mount: couldn't mount %s on %s unit %d\n",fs_type,device_name(dev),device_unit(dev));
				return -1;
			}
		} else {
			printf("mount: invalid fs type: %s\n", fs_type);
			return -1;
		}
		device_close(dev);
	} else {
		printf("mount: couldn't open device %s unit %d\n",devname,unit);
		return -1;
	}

	return -1;
}


static int kshell_printdir(const char *d, int length)
{
	while(length > 0) {
		printf("%s\n", d);
		int len = strlen(d) + 1;
		d += len;
		length -= len;
	}
	return 0;
}

static int kshell_listdir(const char *path)
{
	struct fs_dirent *d = fs_resolve(path);
	if(d) {
		int buffer_length = 1024;
		char *buffer = kmalloc(buffer_length);
		if(buffer) {
			int length = fs_dirent_list(d, buffer, buffer_length);
			if(length>=0) {
				kshell_printdir(buffer, length);
			} else {
				printf("list: %s is not a directory\n", path);
			}
			kfree(buffer);
		}
	} else {
		printf("list: %s does not exist\n", path);
	}

	return 0;
}

int simplefs_format(struct device *dev) {
    char block[512];
    memset(block, 0, sizeof(block));

    // Superblock (sector 0)
    strcpy(block, "SIMPLEFS");       // FS signature
    *(int*)(block + 8) = 1;          // Root dir at sector 1
    if (device_write(dev, 0, block, 512) != 0) {
        printf("mkfs: failed to write superblock\n");
        return 0;
    }

    // Root directory (sector 1)
    memset(block, 0, sizeof(block));
    strcpy(block, "ROOTDIR");        // Just a dummy header
    if (device_write(dev, 1, block, 512) != 0) {
        printf("mkfs: failed to write root directory\n");
        return 0;
    }

    return 1;
}


static int kshell_execute(int argc, const char **argv)
{
    if (argc < 1) {
        printf("No command provided.\n");
        return -1;
    }

    const char *cmd = argv[0];

    if (!strcmp(cmd, "start")) {
        if (argc > 1) {
            int fd = sys_open_file(KNO_STDDIR, argv[1], 0, 0);
            if (fd >= 0) {
                int pid = sys_process_run(fd, argc - 1, &argv[1]);
                if (pid > 0) {
                    printf("started process %d\n", pid);
                    process_yield();
                } else {
                    printf("couldn't start %s\n", argv[1]);
                }
                sys_object_close(fd);
            } else {
                printf("couldn't find %s\n", argv[1]);
            }
        } else {
            printf("start: requires argument.\n");
        }
    } else if (!strcmp(cmd, "run")) {
        if (argc > 1) {
            int fd = sys_open_file(KNO_STDDIR, argv[1], 0, 0);
            if (fd >= 0) {
                int pid = sys_process_run(fd, argc - 1, &argv[1]);
                if (pid > 0) {
                    printf("started process %d\n", pid);
                    process_yield();
                    struct process_info info;
                    process_wait_child(pid, &info, -1);
                    printf("process %d exited with status %d\n", info.pid, info.exitcode);
                    process_reap(info.pid);
                } else {
                    printf("couldn't start %s\n", argv[1]);
                }
                sys_object_close(fd);
            } else {
                printf("couldn't find %s\n", argv[1]);
            }
        } else {
            printf("run: requires argument\n");
        }
    } else if (!strcmp(cmd, "list")) {
        if (argc > 1) {
            printf("\nFiles of '%s'\n", argv[1]);
            kshell_listdir(argv[1]);
        } else {
            printf("\nFiles of '/'\n");
            kshell_listdir(".");
        }
    } else if (!strcmp(cmd, "mount")) {
        if (argc == 4) {
            int unit;
            if (str2int(argv[2], &unit)) {
                kshell_mount(argv[1], unit, argv[3]);
            } else {
                printf("mount: expected unit number but got %s\n", argv[2]);
            }
        } else {
            printf("mount: requires device, unit, and fs type\n");
        }
    } else if (!strcmp(cmd, "kill")) {
        if (argc > 1) {
            int pid;
            if (str2int(argv[1], &pid)) {
                process_kill(pid);
            } else {
                printf("kill: expected process id number but got %s\n", argv[1]);
            }
        } else {
            printf("kill: requires argument\n");
        }
} else if (!strcmp(cmd, "mkdir")) {
    if (argc == 3) {
        struct fs_dirent *dir = fs_resolve(argv[1]);
        if (dir) {
            struct fs_dirent *n = fs_dirent_mkdir(dir, argv[2]);
            if (!n) {
                printf("mkdir: couldn't create %s\n", argv[2]);
            } else {
                printf("mkdir: created directory %s in %s\n", argv[2], argv[1]);
                fs_dirent_close(n);
            }
            fs_dirent_close(dir);
        } else {
            printf("mkdir: couldn't open %s\n", argv[1]);
        }
    } else {
        printf("use: mkdir <parent-dir> <dirname>\n");
    }

} else if (!strcmp(cmd, "reboot")) {
        reboot_user();
   } else if (!strcmp(cmd, "shutdown")) {
    if (argc > 1 && !strcmp(argv[1], "cowsay")) {
        // User wants shutdown cowsay <message>
        if (argc > 2) {
            // Combine all args after "cowsay" into one message
            int total_len = 0;
            for (int i = 2; i < argc; i++) {
                total_len += strlen(argv[i]) + 1; // space or terminator
            }

            char *msg = kmalloc(total_len);
            if (!msg) {
                printf("shutdown cowsay: memory allocation failed.\n");
                return -1;
            }

            msg[0] = '\0';
            for (int i = 2; i < argc; i++) {
                strcat(msg, argv[i]);
                if (i < argc - 1) strcat(msg, " ");
            }

            cowsay(msg);
            kfree(msg);
        } else {
            printf("Usage: shutdown cowsay <message>\n");
            return -1;
        }
    }
    // Then proceed with shutdown normally
    shutdown_user();
} else if (!strcmp(cmd, "clear")) {
        printf("\f");
    } else if (!strcmp(cmd, "neofetch")) {
        neofetch();
    } else if (!strcmp(cmd, "startGUI")) {
        GUI();
    } else if (!strcmp(cmd, "list-drives")) {
        list_drives();
// TEST COMMAND
    } else if (!strcmp(cmd, "test_input")) {
        test_input();
// END OF TEST COMMAND
    } else if (!strcmp(cmd, "automount")) {
        automount();
} else if (!strcmp(cmd, "unmount")) {
if (current->ktable[KNO_STDDIR]) {
        printf("\nunmounting root directory\n");
        sys_object_close(KNO_STDDIR);
    } else {
        printf("\nnothing currently mounted\n");
    }
} else if (!strcmp(cmd, "mkfs")) {
    if (argc == 3) {
        int unit;
        if (str2int(argv[2], &unit)) {
            struct device *dev = device_open(argv[1], unit);
            if (!dev) {
                printf("mkfs: failed to open device\n");
                return -1;
            }

            if (!simplefs_format(dev)) {
                printf("mkfs: format failed\n");
                device_close(dev);
                return -1;
            }

            printf("mkfs: formatted %s unit %d as simplefs\n", argv[1], unit);
            device_close(dev);
        } else {
            printf("mkfs: invalid unit number\n");
        }
    } else {
        printf("Usage: mkfs <device> <unit>\n");
    }
} else if (!strcmp(cmd, "cowsay")) {
    if (argc > 1) {
        // Calculate total length for the message
        int total_len = 0;
        for (int i = 1; i < argc; i++) {
            total_len += strlen(argv[i]) + 1; // +1 for space or 0 terminator
        }

        char *msg = kmalloc(total_len);
        if (!msg) {
            printf("cowsay: memory allocation failed.\n");
            return -1;
        }

        msg[0] = '\0';
        for (int i = 1; i < argc; i++) {
            strcat(msg, argv[i]);
            if (i < argc - 1) strcat(msg, " ");
        }

        cowsay(msg);
        kfree(msg);
    } else {
        printf("Usage: cowsay <message>\n");
    }
} else if (!strcmp(cmd, "contents")) {
    if (argc > 1) {
        const char *filepath = argv[1];
        printf("Reading file: %s\n", filepath);

        int fd = sys_open_file(KNO_STDDIR, filepath, 0, 0);
        if (fd < 0) {
            printf("Failed to open file: %s\n", filepath);
            return 0;
        }

        char *buffer = kmalloc(4096);
        if (!buffer) {
            printf("Memory allocation failed\n");
            sys_object_close(fd);
            return 0;
        }

        int bytes_read = sys_object_read(fd, buffer, 4096);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            printf("\f%s\n", buffer);  // Clear screen before showing contents
        } else {
            printf("File read failed or is empty\n");
        }

        kfree(buffer);
        sys_object_close(fd);

        __asm__ __volatile__ (
            "mov $100000000, %%ecx\n"
            "1:\n"
            "dec %%ecx\n"
            "jnz 1b\n"
            :
            :
            : "ecx"
        );
    } else {
        printf("Usage: contents <filepath>\n");
    } 
} else if (!strcmp(cmd, "echo")) {
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            printf("%s", argv[i]);
            if (i < argc - 1) printf(" ");
        }
        printf("\n");
    } else {
        printf("\n"); // If just "echo" is typed, print a blank line
    }
} else if (!strcmp(cmd, "help")) {
           if (argc == 1) {
        printf("\nCommands:\n");
        printf("start <path> <args>\n");
        printf("run <path> <args>\n");
        printf("list <directory>\n");
        printf("mount <device> <unit> <fstype>\n");
        printf("kill <pid>\n");
        printf("reboot\n");
        printf("shutdown\n");
        printf("clear\n");
        printf("neofetch\n");
        printf("startGUI\n");
        printf("automount\n");
        printf("unmount\n");
        printf("help <command>\n");
        printf("contents <file>\n");
        printf("list-drives");
        printf("cowsay\n\n");
        printf("Keybourd combinations:\n");
        printf("control (ctrl) + e This will exit a program\n");
        printf("control (ctrl) + w This will show the last command the user used\n\n");
        } else {
            printf("\n");
        }
    } else {
        printf("%s: command not found :(\n", argv[0]);
    }

    return 0;
}

int ctrl_e(char c) {
    if ((unsigned char)c == 0x05) {  // Ctrl+E = ASCII 0x05
        return 0;
    }
    return 1; // continue as normal
}

int kshell_readline(char *line, int length)
{
    static char last_command[1024] = {0};
    int i = 0;

    while (i < (length - 1)) {
        int c = console_getchar(&console_root);

        if ((unsigned char)c == 0x05) {  // Ctrl+E
            shutdown_user();
            return 0;
        } else if ((unsigned char)c == 0x0D) {  // Ctrl+M or Enter
            // Intercept Ctrl+M specifically to run FORCE_MENU
            if (i == 0) { // only if nothing typed
                FORCE_MENU(); // ← your function here
                continue;     // don't process as command
            }

            // Normal Enter behaviour
            line[i] = '\0';
            printf("\n");

            if (i > 0) {
                strncpy(last_command, line, sizeof(last_command) - 1);
                last_command[sizeof(last_command) - 1] = '\0';
            }

            return 1;
        } else if ((unsigned char)c == 0x08 || c == ASCII_BS) { // Backspace
            if (i > 0) {
                i--;
                printf("\b \b");
            }
        } else if ((unsigned char)c == 0x17) { // Ctrl+W
            printf("\nLast command: %s\n", last_command);
            printf("\nroot@Doors: /core/NexShell# ");
            i = 0;
            line[0] = '\0';
        } else if (c >= 0x20 && c <= 0x7E) { // Printable characters
            putchar(c);
            line[i++] = c;
        }
    }

    line[i] = '\0';
    return 0;
}


int test_input() {
    while (1) {
        char c = console_getchar(&console_root);

        if (ctrl_e(c) == 0) {
            return 0;
        }

        printf("char: 0x%02X (%c)\n", (unsigned char)c, c);
    }
}


void cowsay(const char *message) {
    int len = strlen(message);
    int i;

    // Top of bubble
    printf(" ");
    for (i = 0; i < len + 2; i++) printf("_");
    printf("\n");

    // Bubble with message
    printf("< %s >\n", message);

    // Bottom of bubble
    printf(" ");
    for (i = 0; i < len + 2; i++) printf("-");
    printf("\n");

    // Cow ASCII art
    printf("        \\   ^__^\n");
    printf("         \\  (oo)\\_______\n");
    printf("            (__)\\       )\\/\\\n");
    printf("                ||----w |\n");
    printf("                ||     ||\n");
}


////////////////////////////////////////////////////////////
// everything past this point is for system interactions //
//////////////////////////////////////////////////////////


// this function is for a menu that shows options for forcing things if the operating system or application isnt responding
void FORCE_MENU() {
    printf("\f"); // Clear the screen first
    printf("=== ⚠ FORCE MENU ⚠ ===\n");
    printf("1. Force Reboot (unsafe)\n");
    printf("2. Force Shutdown (unsafe)\n");
    printf("3. Cancel\n");
    printf("\nSelect an option (1-3): ");

    char choice = console_getchar(&console_root);
    printf("%c\n", choice); // echo the choice

    switch (choice) {
        case '1':
            printf("Rebooting...\n");
            reboot(); // Your existing function
            break;
        case '2':
            printf("Powering off...\n");
// run the ACPI imdediatlly no unmounting disk
    // acpi command to poweroff the cpu
    __asm__ __volatile__ (
        "mov $0x604, %%dx\n\t"
        "mov $0x2000, %%ax\n\t"
        "out %%ax, %%dx\n\t"
        :
        :
        : "ax", "dx"
    );

// make the user force poweroff the system if it can be turned off by the acpi
// display the warning message and then halt the cpu
printf("\f");
    printf("System halted.\n");
    printf("The system could not be shut down via ACPI.\n");
    halt();
    break;
        case '3':
        default:
            printf("Cancelled.\n");
            printf("root@Doors: /core/NexShell# ");
            return 0;
    }
}


void list_drives() {
    const char *devices[] = {"atapi", "ata"};
    const char *fstypes[] = {"cdromfs", "simplefs"};
    int found = 0;

    for (int d = 0; d < 2; d++) { // atapi and ata
        const char *devname = devices[d];
        const char *fstype = fstypes[d];

        for (int unit = 0; unit < 4; unit++) {
            struct device *dev = device_open(devname, unit);
            if (!dev) continue;

            struct fs *fs = fs_lookup(fstype);
            if (!fs) {
                device_close(dev);
                continue;
            }

            struct fs_volume *vol = fs_volume_open(fs, dev);
            if (!vol) {
                device_close(dev);
                continue;
            }

            struct fs_dirent *root = fs_volume_root(vol);
            if (root) {
                printf("Detected Drive:\n");
                printf("  Device: %s\n", devname);
                printf("  Unit: %d\n", unit);
                printf("  Filesystem: %s\n", fstype);
                printf("\n");

                fs_dirent_close(root);
                found = 1;
            }

            fs_volume_close(vol);
            device_close(dev);
        }
    }

    if (!found) {
        printf("No valid drives found.\n");
    }
}

int automount()
{
	int i;

	for(i=0;i<4;i++) {
		printf("automount: trying atapi unit %d.\n",i);
		if(kshell_mount("atapi",i,"cdromfs")==0) return 0;
	}

	for(i=0;i<4;i++) {
		printf("automount: trying ata unit %d.\n",i);
		if(kshell_mount("ata",i,"simplefs")==0) return 0;
	}

	printf("automount: no bootable devices available.\n");
	return -1;
}

void shutdown_user() {
    printf("\f");
    // main message for shutdown
    printf("Powering off... ");
    // inside this will be containing all of the background processes of shutting down
    // that will consist of stopping all the background processes, unmounting disk and rootfs
    // as well as then running the acpi command to poweroff the cpu
    // if the computer cant be shutdown using the acpi it warns the user and halts the system for it to force powered off

   // first process is to kill them all as well as the children
    for (int pid = 2; pid <= 100; pid++) {
        process_kill(pid);
        if ((pid - 1) % 10 == 0) {
        }
    }

   // unmount the disk and root file system
if (current->ktable[KNO_STDDIR]) {
        sys_object_close(KNO_STDDIR);
    } else {
    }


    // wait to show the user it had been proceesing stuff
        __asm__ __volatile__ (
        "mov $400000000, %%ecx\n"
        "1:\n"
        "dec %%ecx\n"
        "jnz 1b\n"
        :
        :
        : "ecx"
    );

// print that it was successful of shutting it down just before the acpi
printf("Done\n");

    // wait to make it smooth instead of the rush
        __asm__ __volatile__ (
        "mov $400000000, %%ecx\n"
        "1:\n"
        "dec %%ecx\n"
        "jnz 1b\n"
        :
        :
        : "ecx"
    );


    // acpi command to poweroff the cpu
    __asm__ __volatile__ (
        "mov $0x604, %%dx\n\t"
        "mov $0x2000, %%ax\n\t"
        "out %%ax, %%dx\n\t"
        :
        :
        : "ax", "dx"
    );

// make the user force poweroff the system if it can be turned off by the acpi
// display the warning message and then halt the cpu

    printf("\f");
    printf("System halted.\n");
    printf("The system could not be shut down via ACPI.\n");

    halt();
}

void reboot_user() {
    printf("\f");
    // main message for reboot
    printf("Rebooting... ");
   // first process is to kill them all as well as the children
    for (int pid = 2; pid <= 100; pid++) {
        process_kill(pid);
        if ((pid - 1) % 10 == 0) {
        }
    }

   // unmount the disk and root file system
if (current->ktable[KNO_STDDIR]) {
        sys_object_close(KNO_STDDIR);
    } else {
    }


    // wait to show the user it had been proceesing stuff
        __asm__ __volatile__ (
        "mov $400000000, %%ecx\n"
        "1:\n"
        "dec %%ecx\n"
        "jnz 1b\n"
        :
        :
        : "ecx"
    );

// print that it was successful of shutting it down just before the acpi
printf("Done\n");

    // wait to make it smooth instead of the rush
        __asm__ __volatile__ (
        "mov $400000000, %%ecx\n"
        "1:\n"
        "dec %%ecx\n"
        "jnz 1b\n"
        :
        :
        : "ecx"
    );


    // reboot command
reboot();

}

int GUI() {
    printf("\nThe GUI is being loaded, please wait...\n");

    __asm__ __volatile__ (
        "mov $400000000, %%ecx\n"
        "1:\n"
        "dec %%ecx\n"
        "jnz 1b\n"
        :
        :
        : "ecx"
    );

    int fd = sys_open_file(KNO_STDDIR, "/core/gui/render/boot.nex", 0, 0);
    if (fd < 0) {
        printf("Failed to open boot.nex\n");
        return -1;
    }

    int pid = sys_process_run(fd, 0, 0);
    if (pid <= 0) {
        printf("Failed to start GUI process\n");
        sys_object_close(fd);
        return -1;
    } else {
        printf("GUI process started with PID %d\n", pid);
        sys_object_close(fd);
    }

    // Draw pixel function using video_buffer instead of video_framebuffer
    void graphics_draw_pixel(int x, int y, uint32_t color) {
        if (x < 0 || y < 0 || x >= video_xres || y >= video_yres) return;
        uint32_t *fb = (uint32_t *)video_buffer;
        fb[y * video_xres + x] = color;
    }

    // Draw cursor function
    void graphics_draw_cursor(int x, int y, const uint32_t *pixels, int width, int height) {
        if (!pixels || width <= 0 || height <= 0) return;

        for (int py = 0; py < height; py++) {
            for (int px = 0; px < width; px++) {
                uint32_t color = pixels[py * width + px];
                if ((color >> 24) == 0) // skip transparent pixels
                    continue;
                graphics_draw_pixel(x + px, y + py, color);
            }
        }
    }

    // Cursor structure
    typedef struct {
        int width;
        int height;
        int hotspot_x;
        int hotspot_y;
        uint32_t *pixels;
    } cursor_t;

    // Load cursor from file function
    int load_cursor_from_file(const char *path, cursor_t *cursor) {
        int fd = sys_open_file(KNO_STDDIR, path, 0, 0);
        if (fd < 0) {
            printf("Failed to open cursor file: %s\n", path);
            return -1;
        }

        char *buffer = kmalloc(16384);
        if (!buffer) {
            printf("Failed to allocate memory for cursor file\n");
            sys_object_close(fd);
            return -1;
        }

        int bytes_read = sys_object_read(fd, buffer, 16384);
        if (bytes_read <= 0) {
            printf("Failed to read cursor file\n");
            kfree(buffer);
            sys_object_close(fd);
            return -1;
        }

        sys_object_close(fd);

        cursor->width = 32;
        cursor->height = 32;
        cursor->hotspot_x = 0;
        cursor->hotspot_y = 0;

        cursor->pixels = kmalloc(cursor->width * cursor->height * sizeof(uint32_t));
        if (!cursor->pixels) {
            printf("Failed to allocate cursor pixels\n");
            kfree(buffer);
            return -1;
        }

        for (int i = 0; i < cursor->width * cursor->height; i++) {
            cursor->pixels[i] = 0xFFFF0000;
        }

        kfree(buffer);
        return 0;
    }

    // Draw cursor now
    cursor_t cursor;
    if (load_cursor_from_file("/core/gui/cursor/main.cur", &cursor) == 0) {
        int cursor_x = video_xres / 2;
        int cursor_y = video_yres / 2;
        graphics_draw_cursor(cursor_x - cursor.hotspot_x, cursor_y - cursor.hotspot_y, cursor.pixels, cursor.width, cursor.height);
    } else {
        printf("Failed to load cursor\n");
    }

    return 0;
}



void neofetch() {
    const char *architecture = "x86";
    const char *shell = "Kshell";

    printf("\n");
    printf("|----------------------------------------------------------|\n");
    printf("|                     NexShell v3.6.9-DEV                  |\n");
    printf("|                  (C)Copyright 2025 XPDevs                |\n");
    printf("|                  Build id: NS127-0425-S1                 |\n");
    printf("|----------------------------------------------------------|\n");
    printf("| Architecture: %s\n", architecture);
    printf("| Shell: %s\n", shell);
    printf("| Video: %d x %d\n", video_xres, video_yres);
    printf("| Kernel size: %d bytes\n", kernel_size);
    printf("|----------------------------------------------------------|\n\n");
}


int kshell_launch()
{
	char line[1024];
	const char *argv[100];
	int argc;
// everything past here for control
printf("ACPI: initialized");

    // Try to mount the root filesystem
    printf("\nMounting root filesystem\n");

automount();

// after automounting the disk clear the screen for more space for displaying content after that wait to ensure nothing feels to rushed
// clear
printf("\f");
    __asm__ __volatile__ (
        "mov $400000000, %%ecx\n"
        "1:\n"
        "dec %%ecx\n"
        "jnz 1b\n"
        :
        :
        : "ecx"
    );

// then go straight into the GUI but command line if it has any errors
GUI();

	while(1) {
               printf("\n");
		printf("root@Doors: /core/NexShell# ");
		kshell_readline(line, sizeof(line));

		argc = 0;
		argv[argc] = strtok(line, " ");
		while(argv[argc]) {
			argc++;
			argv[argc] = strtok(0, " ");
		}

		        if (argc > 0) {
            // Check for 'then' keyword
            int then_index = -1;
            for (int i = 0; i < argc; i++) {
                if (!strcmp(argv[i], "then")) {
                    then_index = i;
                    break;
                }
            }

            if (then_index != -1) {
                // Split into two commands
                int result;

                // First command before 'then'
                argv[then_index] = 0;
                result = kshell_execute(then_index, argv);

                // If successful, run second command
                if (result == 0) {
                    const char *argv2[100];
                    int argc2 = 0;

                    for (int i = then_index + 1; i < argc && argv[i]; i++) {
                        argv2[argc2++] = argv[i];
                    }

                    if (argc2 > 0) {
                        kshell_execute(argc2, argv2);
                    }
                }
            } else {
                // Single command
                kshell_execute(argc, argv);
            }
        }

	}
	return 0;
}