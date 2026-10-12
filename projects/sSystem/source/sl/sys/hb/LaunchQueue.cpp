#include <sl/sys/hb/LaunchQueue.hpp>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <cctype>

// main.cpp: daemon.log, with a raw-fs fallback when stdio writes fail.
void DaemonLog(const char *fmt, ...);

namespace sl::sys::hb {

    namespace {

        bool EndsWithNro(const char *p) {
            const size_t n = strlen(p);
            if (n < 4) return false;
            return strcasecmp(p + n - 4, ".nro") == 0;
        }

        // What the daemon is willing to be told to run. This is an
        // unauthenticated drop box on a card anyone can edit, and acting on it
        // means starting code, so the path has to be a real .nro on the SD card
        // and nothing else. ".." is refused outright rather than resolved: the
        // sdmc: prefix is the whole of the restriction, and a path that climbs
        // out of it is not something to reason about, it is something to drop.
        bool PathLooksSane(const char *p) {
            if (!p[0] || strlen(p) >= MaxNroPath)      return false;
            if (strncmp(p, "sdmc:/", 6) != 0)          return false;
            if (strstr(p, ".."))                       return false;
            if (!EndsWithNro(p))                       return false;
            struct stat st;
            if (stat(p, &st) != 0)                     return false;
            return S_ISREG(st.st_mode);
        }

        // Every game's id is 0x0100... to 0x01FF...; anything else is not one,
        // and a value that is not all hex digits is refused rather than read
        // up to the first one that is not.
        bool ParseAppId(const char *val, u64 &out) {
            char *end = nullptr;
            const unsigned long long id = strtoull(val, &end, 16);
            if (end == val || *end != '\0') return false;
            if ((id >> 56) != 0x01) return false;
            out = id;
            return true;
        }

        // 32 hex digits: uid[0] then uid[1], as users.txt lists them.
        bool ParseUser(const char *val, AccountUid &out) {
            if (strlen(val) != 32) return false;
            for (int i = 0; i < 32; i++) if (!isxdigit((unsigned char)val[i])) return false;
            char half[17] = {};
            memcpy(half, val, 16);      out.uid[0] = strtoull(half, nullptr, 16);
            memcpy(half, val + 16, 16); out.uid[1] = strtoull(half, nullptr, 16);
            return accountUidIsValid(&out);
        }

        // Read one request file into `out`. The file is the caller's to delete.
        bool Parse(const char *path, Request &out) {
            FILE *fp = fopen(path, "r");
            if (!fp) return false;

            char line[MaxNroPath + 64];
            bool have_nro = false, have_app = false, have_list = false;
            while (fgets(line, sizeof(line), fp)) {
                line[strcspn(line, "\r\n")] = '\0';
                char *eq = strchr(line, '=');
                if (!eq) continue;
                *eq = '\0';
                const char *key = line;
                const char *val = eq + 1;

                if (strcmp(key, "nro") == 0) {
                    if (strlen(val) >= MaxNroPath) { fclose(fp); return false; }
                    strncpy(out.nro, val, sizeof(out.nro) - 1);
                    have_nro = true;
                } else if (strcmp(key, "argv") == 0) {
                    if (strlen(val) >= MaxArgv) { fclose(fp); return false; }
                    strncpy(out.argv, val, sizeof(out.argv) - 1);
                } else if (strcmp(key, "mode") == 0) {
                    out.mode = (strcmp(val, "app") == 0) ? LaunchMode::App
                                                         : LaunchMode::Applet;
                } else if (strcmp(key, "donor") == 0) {
                    out.donor = strtoull(val, nullptr, 16);
                } else if (strcmp(key, "app") == 0) {
                    if (!ParseAppId(val, out.app_id)) { fclose(fp); return false; }
                    have_app = true;
                } else if (strcmp(key, "user") == 0) {
                    if (!ParseUser(val, out.user)) { fclose(fp); return false; }
                } else if (strcmp(key, "list") == 0) {
                    have_list = strcmp(val, "1") == 0;
                }
            }
            fclose(fp);

            // Exactly one thing to do: a file that asks for two is not one the
            // daemon can guess the meaning of.
            if ((int)have_nro + (int)have_app + (int)have_list != 1) return false;
            out.kind = have_app  ? Kind::Game
                     : have_list ? Kind::List
                     :             Kind::Homebrew;
            return true;
        }

        bool HasQueueSuffix(const char *name) {
            const size_t n = strlen(name);
            const size_t sfx = strlen(QueueSuffix);
            if (name[0] == '.' || n <= sfx) return false;
            return strcasecmp(name + n - sfx, QueueSuffix) == 0;
        }

    } // namespace

    void LaunchQueue::Reset() {
        mkdir("sdmc:/slaunch", 0777);
        mkdir(QueueDir, 0777);

        DIR *d = opendir(QueueDir);
        if (!d) return;
        int dropped = 0;
        char path[FS_MAX_PATH];
        while (dirent *e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            snprintf(path, sizeof(path), "%s/%s", QueueDir, e->d_name);
            if (remove(path) == 0) dropped++;
        }
        closedir(d);
        if (dropped) DaemonLog("hb_queue: dropped %d stale request(s) from a previous boot", dropped);
    }

    bool LaunchQueue::Take(Request &out, bool games_only) {
        // Lowest name wins, so two requests are taken in a defined order rather
        // than in whatever order the FAT driver happens to hand them back. With
        // `games_only`, a .nro request is passed over for the next name up.
        char pick[FS_MAX_PATH] = {};
        char after[FS_MAX_PATH] = {};
        char path[FS_MAX_PATH];
        Request req;
        bool parsed = false;
        while (true) {
            DIR *d = opendir(QueueDir);
            if (!d) return false;
            pick[0] = '\0';
            while (dirent *e = readdir(d)) {
                if (!HasQueueSuffix(e->d_name)) continue;
                if (after[0] && strcmp(e->d_name, after) <= 0) continue;
                if (!pick[0] || strcmp(e->d_name, pick) < 0) {
                    strncpy(pick, e->d_name, sizeof(pick) - 1);
                    pick[sizeof(pick) - 1] = '\0';
                }
            }
            closedir(d);
            if (!pick[0]) return false;

            snprintf(path, sizeof(path), "%s/%s", QueueDir, pick);
            req = Request{};
            parsed = Parse(path, req);
            if (!games_only || (parsed && req.kind != Kind::Homebrew)) break;
            strncpy(after, pick, sizeof(after) - 1);   // not ours to take now
        }

        // One-shot, whatever happened: taken means gone. A request that is kept
        // because it failed is a request that fails again on the next pass, and
        // the next, with the console launching nothing in between.
        remove(path);

        if (!parsed) {
            DaemonLog("hb_queue: %s is not a request the daemon understands - dropped", pick);
            return false;
        }
        if (req.kind == Kind::Game) {
            // Whether it is installed is for the launch to find out: it fails
            // cleanly and the log says so.
            out = req;
            DaemonLog("hb_queue: took %s app=0x%016llx", pick, (unsigned long long)out.app_id);
            return true;
        }
        if (req.kind == Kind::List) {
            out = req;
            DaemonLog("hb_queue: took %s list", pick);
            return true;
        }
        if (!PathLooksSane(req.nro)) {
            DaemonLog("hb_queue: refused nro=%s (must be an existing sdmc:/...nro) - dropped",
                req.nro);
            return false;
        }

        out = req;
        DaemonLog("hb_queue: took %s mode=%s donor=0x%016llx nro=%s", pick,
            out.mode == LaunchMode::App ? "app" : "applet",
            (unsigned long long)out.donor, out.nro);
        return true;
    }

} // namespace sl::sys::hb
