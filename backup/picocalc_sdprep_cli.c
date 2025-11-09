#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void die(const char *msg) { perror(msg); exit(EXIT_FAILURE); }
static void xdie(const char *msg) { fprintf(stderr, "Error: %s\n", msg); exit(EXIT_FAILURE); }

static int run_cmd(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        execvp(argv[0], argv);
        perror("execvp");
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) die("waitpid");
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fprintf(stderr, "Command failed:");
        for (int i = 0; argv[i]; ++i) fprintf(stderr, " %s", argv[i]);
        fprintf(stderr, "\n");
        return -1;
    }
    return 0;
}

static int64_t get_dev_size_bytes(const char *dev) {
    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) die("open device");
    uint64_t bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &bytes) < 0) die("ioctl BLKGETSIZE64");
    close(fd);
    return (int64_t)bytes;
}

static bool is_block_device(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISBLK(st.st_mode);
}

static void require_root() {
    if (geteuid() != 0) xdie("Run as root (sudo).");
}

static bool ends_with_digit(const char *s) {
    size_t n = strlen(s);
    if (n == 0) return false;
    return (s[n-1] >= '0' && s[n-1] <= '9');
}

static void unmount_all(const char *dev) {
    // best-effort: use `lsblk -rno MOUNTPOINT`
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "lsblk -rno MOUNTPOINT %s* 2>/dev/null | sed -n '1!p' | grep -v '^$'",
             dev);
    FILE *fp = popen(cmd, "r");
    if (!fp) return;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = 0;
        if (line[0]) {
            char *argv[] = {"umount", (char*)line, NULL};
            run_cmd(argv); // ignore failure
        }
    }
    pclose(fp);
}

static void show_layout(const char *dev) {
    char *argv1[] = {"fdisk", "-l", (char*)dev, NULL};
    run_cmd(argv1); // ignore failures
    char *argv2[] = {"lsblk", "-fo", "NAME,SIZE,TYPE,FSTYPE,LABEL,MOUNTPOINT", (char*)dev, NULL};
    run_cmd(argv2);
}

int main(int argc, char **argv) {
    require_root();

    if (argc != 2) {
        fprintf(stderr, "Usage: %s /dev/sdX|/dev/mmcblk0|/dev/nvme0n1\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *DEVICE = argv[1];
    if (!is_block_device(DEVICE)) xdie("Not a block device.");

    // Basic root-device guard: ensure target is not root's parent device
    // (best-effort, user must still confirm exact path)
    {
        FILE *fp = popen("df --output=source / | tail -1", "r");
        if (fp) {
            char src[512] = {0};
            if (fgets(src, sizeof(src), fp)) {
                src[strcspn(src, "\n")] = 0;
                // Get parent disk of root
                char cmd[512];
                snprintf(cmd, sizeof(cmd), "lsblk -no PKNAME %s 2>/dev/null", src);
                FILE *fp2 = popen(cmd, "r");
                if (fp2) {
                    char pk[256] = {0};
                    if (fgets(pk, sizeof(pk), fp2)) {
                        pk[strcspn(pk, "\n")] = 0;
                        char parent[300];
                        snprintf(parent, sizeof(parent), "/dev/%s", pk);
                        if (strcmp(parent, DEVICE) == 0) {
                            pclose(fp2); pclose(fp);
                            xdie("Refusing: target appears to contain root filesystem.");
                        }
                    }
                    pclose(fp2);
                }
            }
            pclose(fp);
        }
    }

    printf("THIS WILL DESTROY ALL DATA ON %s\n\n", DEVICE);
    char *lsblk_argv[] = {"lsblk", (char*)DEVICE, NULL};
    run_cmd(lsblk_argv);

    printf("\nType the exact device path to proceed (%s): ", DEVICE);
    fflush(stdout);
    char confirm[512] = {0};
    if (!fgets(confirm, sizeof(confirm), stdin)) xdie("stdin read");
    confirm[strcspn(confirm, "\n")] = 0;
    if (strcmp(confirm, DEVICE) != 0) xdie("Confirmation mismatch. Aborting.");

    unmount_all(DEVICE);

    // wipefs -a DEVICE
    char *wipe_argv[] = {"wipefs", "-a", (char*)DEVICE, NULL};
    if (run_cmd(wipe_argv) != 0) xdie("wipefs failed");

    // parted mklabel msdos
    char *mklabel_argv[] = {"parted", "-s", (char*)DEVICE, "mklabel", "msdos", NULL};
    if (run_cmd(mklabel_argv) != 0) xdie("parted mklabel failed");

    int64_t bytes = get_dev_size_bytes(DEVICE);
    if (bytes <= 0) xdie("device size unknown");
    const int64_t MIB = 1024LL * 1024LL;
    int64_t size_mib = bytes / MIB;
    const int64_t reserved_mib = 32;
    const int64_t start_mib = 1;
    int64_t end1_mib = size_mib - reserved_mib;

    if (end1_mib <= start_mib + 8) xdie("device too small for requested layout");

    // parted mkpart p1 fat32 1MiB end1MiB
    char end1_str[64];
    snprintf(end1_str, sizeof(end1_str), "%ldMiB", (long)end1_mib);
    char *mkpart1_argv[] = {"parted", "-s", (char*)DEVICE, "mkpart", "primary", "fat32", "1MiB", end1_str, NULL};
    if (run_cmd(mkpart1_argv) != 0) xdie("parted mkpart p1 failed");

    // parted mkpart p2 end1MiB 100%
    char *mkpart2_argv[] = {"parted", "-s", (char*)DEVICE, "mkpart", "primary", (char*)end1_str, "100%", NULL};
    if (run_cmd(mkpart2_argv) != 0) xdie("parted mkpart p2 failed");

    // partprobe + udevadm settle
    char *probe_argv[] = {"partprobe", (char*)DEVICE, NULL};
    run_cmd(probe_argv); // best effort
    char *settle_argv[] = {"udevadm", "settle", NULL};
    run_cmd(settle_argv);

    // Determine partition names
    // If device name ends with a digit -> use p1/p2 suffix, else 1/2
    char P1[512], P2[512];
    if (ends_with_digit(DEVICE)) {
        snprintf(P1, sizeof(P1), "%sp1", DEVICE);
        snprintf(P2, sizeof(P2), "%sp2", DEVICE);
    } else {
        snprintf(P1, sizeof(P1), "%s1", DEVICE);
        snprintf(P2, sizeof(P2), "%s2", DEVICE);
    }

    // Wait briefly for nodes to appear
    for (int i = 0; i < 20; ++i) {
        if (is_block_device(P1) && is_block_device(P2)) break;
        usleep(250000);
    }
    if (!is_block_device(P1) || !is_block_device(P2)) xdie("partitions not detected by kernel");

    // mkfs.fat -F32 -I -n PICO_DATA P1
    char *mkfs_argv[] = {"mkfs.fat", "-F32", "-v", "-I", "-n", "PICO_DATA", P1, NULL};
    if (run_cmd(mkfs_argv) != 0) xdie("mkfs.fat failed");

    // Leave P2 unformatted intentionally (reserved)
    printf("\nFinal layout:\n");
    show_layout(DEVICE);

    puts("\nSuccess.");
    return EXIT_SUCCESS;
}

