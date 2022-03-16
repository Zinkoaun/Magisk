#include <sys/mount.h>
#include <libgen.h>

#include <magisk.hpp>
#include <magiskpolicy.hpp>
#include <utils.hpp>
#include <stream.hpp>

#include "init.hpp"
#include "magiskrc.inc"

using namespace std;

static vector<string> rc_list;

static void patch_init_rc(const char *src, const char *dest, const char *tmp_dir) {
    FILE *rc = xfopen(dest, "we");
    if (!rc) {
        PLOGE("%s: open %s failed", __PRETTY_FUNCTION__, src);
        return;
    }
    file_readline(src, [=](string_view line) -> bool {
        // Do not start vaultkeeper
        if (str_contains(line, "start vaultkeeper")) {
            LOGD("Remove vaultkeeper\n");
            return true;
        }
        // Do not run flash_recovery
        if (str_starts(line, "service flash_recovery")) {
            LOGD("Remove flash_recovery\n");
            fprintf(rc, "service flash_recovery /system/bin/xxxxx\n");
            return true;
        }
        // Samsung's persist.sys.zygote.early will start zygotes before actual post-fs-data phase
        if (str_starts(line, "on property:persist.sys.zygote.early=")) {
            LOGD("Invalidate persist.sys.zygote.early\n");
            fprintf(rc, "on property:persist.sys.zygote.early.xxxxx=true\n");
            return true;
        }
        // Else just write the line
        fprintf(rc, "%s", line.data());
        return true;
    });

    fprintf(rc, "\n");

    // Inject custom rc scripts
    for (auto &script : rc_list) {
        // Replace template arguments of rc scripts with dynamic paths
        replace_all(script, "${MAGISKTMP}", tmp_dir);
        fprintf(rc, "\n%s\n", script.data());
    }
    rc_list.clear();

    // Inject Magisk rc scripts
    char pfd_svc[16], ls_svc[16], bc_svc[16];
    gen_rand_str(pfd_svc, sizeof(pfd_svc));
    gen_rand_str(ls_svc, sizeof(ls_svc));
    gen_rand_str(bc_svc, sizeof(bc_svc));
    LOGD("Inject magisk services: [%s] [%s] [%s]\n", pfd_svc, ls_svc, bc_svc);
    fprintf(rc, MAGISK_RC, tmp_dir, pfd_svc, ls_svc, bc_svc);

    fclose(rc);
    clone_attr(src, dest);
}

static void load_overlay_rc(const char *overlay) {
    auto dir = open_dir(overlay);
    if (!dir) return;

    int dfd = dirfd(dir.get());
    // Do not allow overwrite init.rc
    unlinkat(dfd, "init.rc", 0);
    for (dirent *entry; (entry = xreaddir(dir.get()));) {
        if (str_ends(entry->d_name, ".rc")) {
            LOGD("Found rc script [%s]\n", entry->d_name);
            int rc = xopenat(dfd, entry->d_name, O_RDONLY | O_CLOEXEC);
            rc_list.push_back(fd_full_read(rc));
            close(rc);
            unlinkat(dfd, entry->d_name, 0);
        }
    }
}

bool MagiskInit::patch_sepolicy(const char *file) {
    bool patch_init = false;
    sepolicy *sepol = nullptr;

    if (access(SPLIT_PLAT_CIL, R_OK) == 0) {
        LOGD("sepol: split policy\n");
        patch_init = true;
    } else if (access("/sepolicy", R_OK) == 0) {
        LOGD("sepol: monolithic policy\n");
        sepol = sepolicy::from_file("/sepolicy");
    } else {
        LOGD("sepol: no selinux\n");
        return false;
    }

    if (access(SELINUX_VERSION, F_OK) != 0) {
        // Mount selinuxfs to communicate with kernel
        xmount("selinuxfs", SELINUX_MNT, "selinuxfs", 0, nullptr);
        mount_list.emplace_back(SELINUX_MNT);
    }

    if (patch_init)
        sepol = sepolicy::from_split();

    if (!sepol) {
        LOGE("Cannot load split cil\n");
        return false;
    }

    sepol->magisk_rules();

    // Custom rules
    if (!custom_rules_dir.empty()) {
        if (auto dir = xopen_dir(custom_rules_dir.data())) {
            for (dirent *entry; (entry = xreaddir(dir.get()));) {
                auto rule = custom_rules_dir + "/" + entry->d_name + "/sepolicy.rule";
                if (xaccess(rule.data(), R_OK) == 0) {
                    LOGD("Loading custom sepolicy patch: [%s]\n", rule.data());
                    sepol->load_rule_file(rule.data());
                }
            }
        }
    }

    LOGD("Dumping sepolicy to: [%s]\n", file);
    sepol->to_file(file);
    delete sepol;

    // Remove OnePlus stupid debug sepolicy and use our own
    if (access("/sepolicy_debug", F_OK) == 0) {
        unlink("/sepolicy_debug");
        link("/sepolicy", "/sepolicy_debug");
    }

    return patch_init;
}

#define MOCK_LOAD      SELINUXMOCK "/load"
#define MOCK_ENFORCE   SELINUXMOCK "/enforce"
#define REAL_SELINUXFS SELINUXMOCK "/fs"

void MagiskInit::hijack_sepolicy() {
    // Read all custom rules into memory
    string rules;
    if (!custom_rules_dir.empty()) {
        if (auto dir = xopen_dir(custom_rules_dir.data())) {
            for (dirent *entry; (entry = xreaddir(dir.get()));) {
                auto rule_file = custom_rules_dir + "/" + entry->d_name + "/sepolicy.rule";
                if (xaccess(rule_file.data(), R_OK) == 0) {
                    LOGD("Load custom sepolicy patch: [%s]\n", rule_file.data());
                    full_read(rule_file.data(), rules);
                    rules += '\n';
                }
            }
        }
    }

    // Hijack the "load" and "enforce" node in selinuxfs to manipulate
    // the actual sepolicy being loaded into the kernel

    // We need to preserve sysfs and selinuxfs after re-exec
    mount_list.erase(std::remove_if(
            mount_list.begin(), mount_list.end(),
            [](const string &s) { return s == "/sys"; }), mount_list.end());

    if (access(SELINUX_ENFORCE, F_OK) != 0) {
        // selinuxfs needs to be mounted
        xmount("selinuxfs", SELINUX_MNT, "selinuxfs", 0, nullptr);
    }

    LOGD("Hijack [" SELINUX_LOAD "] and [" SELINUX_ENFORCE "]\n");

    xmkdir(SELINUXMOCK, 0);
    mkfifo(MOCK_LOAD, 0600);
    mkfifo(MOCK_ENFORCE, 0644);
    xmount(MOCK_LOAD, SELINUX_LOAD, nullptr, MS_BIND, nullptr);
    xmount(MOCK_ENFORCE, SELINUX_ENFORCE, nullptr, MS_BIND, nullptr);

    // Create a new process waiting for original init to load sepolicy into our fifo
    if (xfork()) {
        // In parent, return and continue boot process
        return;
    }

    // Read full sepolicy
    int fd = xopen(MOCK_LOAD, O_RDONLY);
    string policy = fd_full_read(fd);
    close(fd);
    auto sepol = unique_ptr<sepolicy>(sepolicy::from_data(policy.data(), policy.length()));

    sepol->magisk_rules();
    sepol->load_rules(rules);

    // Mount selinuxfs to another path
    xmkdir(REAL_SELINUXFS, 0755);
    xmount("selinuxfs", REAL_SELINUXFS, "selinuxfs", 0, nullptr);

    // This open will block until the actual init calls security_getenforce
    fd = xopen(MOCK_ENFORCE, O_WRONLY);

    // Cleanup the hijacks
    umount2("/init", MNT_DETACH);
    xumount2(SELINUX_LOAD, MNT_DETACH);
    xumount2(SELINUX_ENFORCE, MNT_DETACH);

    // Load patched policy
    sepol->to_file(REAL_SELINUXFS "/load");

    // Write to mock "enforce" ONLY after sepolicy is loaded. We need to make sure
    // the actual init process is blocked until sepolicy is loaded, or else
    // restorecon will fail and re-exec won't change context, causing boot failure.
    // We (ab)use the fact that security_getenforce reads the "enforce" file, and
    // because it has been replaced with our FIFO file, init will block until we
    // write something into the pipe, effectively hijacking its control flow.

    xwrite(fd, "0", 1);
    close(fd);

    // At this point, the actual init process will be unblocked
    // and continue on with restorecon + re-exec.

    // Terminate process
    exit(0);
}

static void recreate_sbin(const char *mirror, bool use_bind_mount) {
    auto dp = xopen_dir(mirror);
    int src = dirfd(dp.get());
    char buf[4096];
    for (dirent *entry; (entry = xreaddir(dp.get()));) {
        string sbin_path = "/sbin/"s + entry->d_name;
        struct stat st;
        fstatat(src, entry->d_name, &st, AT_SYMLINK_NOFOLLOW);
        if (S_ISLNK(st.st_mode)) {
            xreadlinkat(src, entry->d_name, buf, sizeof(buf));
            xsymlink(buf, sbin_path.data());
        } else {
            sprintf(buf, "%s/%s", mirror, entry->d_name);
            if (use_bind_mount) {
                auto mode = st.st_mode & 0777;
                // Create dummy
                if (S_ISDIR(st.st_mode))
                    xmkdir(sbin_path.data(), mode);
                else
                    close(xopen(sbin_path.data(), O_CREAT | O_WRONLY | O_CLOEXEC, mode));

                xmount(buf, sbin_path.data(), nullptr, MS_BIND, nullptr);
            } else {
                xsymlink(buf, sbin_path.data());
            }
        }
    }
}

static string magic_mount_list;

static void magic_mount(const string &sdir, const string &ddir = "") {
    auto dir = xopen_dir(sdir.data());
    if (!dir) return;
    for (dirent *entry; (entry = xreaddir(dir.get()));) {
        string src = sdir + "/" + entry->d_name;
        string dest = ddir + "/" + entry->d_name;
        if (access(dest.data(), F_OK) == 0) {
            if (entry->d_type == DT_DIR) {
                // Recursive
                magic_mount(src, dest);
            } else {
                LOGD("Mount [%s] -> [%s]\n", src.data(), dest.data());
                xmount(src.data(), dest.data(), nullptr, MS_BIND, nullptr);
                magic_mount_list += dest;
                magic_mount_list += '\n';
            }
        }
    }
}

static void patch_socket_name(const char *path) {
    static char rstr[16] = { 0 };
    if (rstr[0] == '\0')
        gen_rand_str(rstr, sizeof(rstr));
    auto bin = mmap_data(path, true);
    bin.patch({ make_pair(MAIN_SOCKET, rstr) });
}

#define ROOTMIR     MIRRDIR "/system_root"
#define NEW_INITRC  "/system/etc/init/hw/init.rc"

void SARBase::patch_ro_root() {
    string tmp_dir;

    if (access("/sbin", F_OK) == 0) {
        tmp_dir = "/sbin";
    } else {
        char buf[8];
        gen_rand_str(buf, sizeof(buf));
        tmp_dir = "/dev/"s + buf;
        xmkdir(tmp_dir.data(), 0);
    }

    setup_tmp(tmp_dir.data());
    chdir(tmp_dir.data());

    mount_rules_dir(BLOCKDIR, MIRRDIR);

    // Mount system_root mirror
    xmkdir(ROOTMIR, 0755);
    xmount("/", ROOTMIR, nullptr, MS_BIND, nullptr);
    mount_list.emplace_back(tmp_dir + "/" ROOTMIR);

    // Recreate original sbin structure if necessary
    if (tmp_dir == "/sbin")
        recreate_sbin(ROOTMIR "/sbin", true);

    xmkdir(ROOTOVL, 0);

    // Handle avd hack
    if (avd_hack) {
        int src = xopen("/init", O_RDONLY | O_CLOEXEC);
        auto init = mmap_data("/init");
        // Force disable early mount on original init
        init.patch({ make_pair("android,fstab", "xxx") });
        int dest = xopen(ROOTOVL "/init", O_CREAT | O_WRONLY | O_CLOEXEC, 0);
        xwrite(dest, init.buf, init.sz);
        fclone_attr(src, dest);
        close(src);
        close(dest);
    }

    // Handle overlay.d
    restore_folder(ROOTOVL, overlays);
    overlays.clear();
    load_overlay_rc(ROOTOVL);
    if (access(ROOTOVL "/sbin", F_OK) == 0) {
        // Move files in overlay.d/sbin into tmp_dir
        mv_path(ROOTOVL "/sbin", ".");
    }

    // Patch init.rc
    if (access(NEW_INITRC, F_OK) == 0) {
        // Android 11's new init.rc
        xmkdirs(dirname(ROOTOVL NEW_INITRC), 0755);
        patch_init_rc(NEW_INITRC, ROOTOVL NEW_INITRC, tmp_dir.data());
    } else {
        patch_init_rc("/init.rc", ROOTOVL "/init.rc", tmp_dir.data());
    }

    // Extract magisk
    {
        auto magisk = mmap_data("magisk32.xz");
        unlink("magisk32.xz");
        int fd = xopen("magisk32", O_WRONLY | O_CREAT, 0755);
        unxz(fd, magisk.buf, magisk.sz);
        close(fd);
        patch_socket_name("magisk32");
        if (access("magisk64.xz", F_OK) == 0) {
            magisk = mmap_data("magisk64.xz");
            unlink("magisk64.xz");
            fd = xopen("magisk64", O_WRONLY | O_CREAT, 0755);
            unxz(fd, magisk.buf, magisk.sz);
            close(fd);
            patch_socket_name("magisk64");
            xsymlink("./magisk64", "magisk");
        } else {
            xsymlink("./magisk32", "magisk");
        }
    }

    // Mount rootdir
    magic_mount(ROOTOVL);
    int dest = xopen(ROOTMNT, O_WRONLY | O_CREAT, 0);
    write(dest, magic_mount_list.data(), magic_mount_list.length());
    close(dest);

    hijack_sepolicy();

    chdir("/");
}

#define TMP_MNTDIR "/dev/mnt"
#define TMP_RULESDIR "/.backup/.sepolicy.rules"

void MagiskInit::patch_rw_root() {
    // Create hardlink mirror of /sbin to /root
    mkdir("/root", 0777);
    clone_attr("/sbin", "/root");
    link_path("/sbin", "/root");

    // Handle custom sepolicy rules
    xmkdir(TMP_MNTDIR, 0755);
    xmkdir("/dev/block", 0755);
    mount_rules_dir("/dev/block", TMP_MNTDIR);
    // Preserve custom rule path
    if (!custom_rules_dir.empty()) {
        string rules_dir = "./" + custom_rules_dir.substr(sizeof(TMP_MNTDIR));
        xsymlink(rules_dir.data(), TMP_RULESDIR);
    }

    if (patch_sepolicy("/sepolicy")) {
        if (access("/system/bin/init", F_OK) == 0) {
            auto init = mmap_data("/system/bin/init");
            init.patch({ make_pair(SPLIT_PLAT_CIL, "xxx") });
            int dest = xopen("/init", O_TRUNC | O_WRONLY | O_CLOEXEC, 0);
            xwrite(dest, init.buf, init.sz);
            close(dest);
        } else {
            auto init = mmap_data("/init", true);
            init.patch({ make_pair(SPLIT_PLAT_CIL, "xxx") });
        }
    }

    // Handle overlays
    if (access("/overlay.d", F_OK) == 0) {
        LOGD("Merge overlay.d\n");
        load_overlay_rc("/overlay.d");
        mv_path("/overlay.d", "/");
    }

    patch_init_rc("/init.rc", "/init.p.rc", "/sbin");
    rename("/init.p.rc", "/init.rc");

    // Dump magiskinit as magisk
    int fd = xopen("/sbin/magisk", O_WRONLY | O_CREAT, 0755);
    write(fd, self.buf, self.sz);
    close(fd);
}

void MagiskProxy::start() {
    // Mount rootfs as rw to do post-init rootfs patches
    xmount(nullptr, "/", nullptr, MS_REMOUNT, nullptr);

    // Backup stuffs before removing them
    self = mmap_data("/sbin/magisk");
    magisk_cfg = mmap_data("/.backup/.magisk");
    auto magisk = mmap_data("/sbin/magisk32.xz");
    auto magisk64 = mmap_data("/sbin/magisk64.xz");
    char custom_rules_dir[64];
    custom_rules_dir[0] = '\0';
    xreadlink(TMP_RULESDIR, custom_rules_dir, sizeof(custom_rules_dir));

    unlink("/sbin/magisk");
    unlink("/sbin/magisk32.xz");
    unlink("/sbin/magisk64.xz");
    rm_rf("/.backup");

    setup_tmp("/sbin");

    // Extract magisk
    int fd = xopen("/sbin/magisk32", O_WRONLY | O_CREAT, 0755);
    unxz(fd, magisk.buf, magisk.sz);
    close(fd);
    patch_socket_name("/sbin/magisk32");
    if (magisk64.sz) {
        fd = xopen("/sbin/magisk64", O_WRONLY | O_CREAT, 0755);
        unxz(fd, magisk64.buf, magisk64.sz);
        close(fd);
        patch_socket_name("/sbin/magisk64");
        xsymlink("./magisk64", "/sbin/magisk");
    } else {
        xsymlink("./magisk32", "/sbin/magisk");
    }

    // Create symlinks pointing back to /root
    recreate_sbin("/root", false);

    if (custom_rules_dir[0])
        xsymlink(custom_rules_dir, "/sbin/" RULESDIR);

    // Tell magiskd to remount rootfs
    setenv("REMOUNT_ROOT", "1", 1);
    execv("/sbin/magisk", argv);
}
