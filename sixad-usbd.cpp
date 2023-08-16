#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <syslog.h>
#include <getopt.h>
#include <pthread.h>
#include <assert.h>
#include <limits.h>

#include "shared.h"
#include "sixaxis.h"
#include "uinput.h"

#define SYSLOG_NAME "sixad-usbd"

#define OK 1
#define FAIL 0

enum {
    LOG_STDERR = 0,
    LOG_FILE,
    LOG_TOSYSLOG
} log_type = LOG_STDERR;

char *log_filename = NULL;
char *profile_name = NULL;
char *dev_name = NULL;
char *fifo_path = NULL;
char *pid_filename = NULL;
FILE *pid_file = NULL;

#define STD_FIFO_PATH "/var/run/sixad-usbd.ctl"

#define DEV_PATH            "/dev/%s"
#define SYS_HID_INFO_PATH   "/sys/class/hidraw/%s/device/uevent"
#define HID_NAME            "HID_NAME=Sony PLAYSTATION(R)3 Controller"
#define HID_UNIQ            "HID_UNIQ="

#define DEFAULT_PROFILE     "hidraw"
#define PROFILE_PATH        "/var/lib/sixad/profiles/%s"

struct controller_info {
    controller_info() {
        dev_name = hid_name = uniq_id = profile = dev_path = 0;
        next = 0;
    }
    char *dev_name;
    char *hid_name;
    char *uniq_id;
    char *profile;
    pthread_t thread_id;
    char *dev_path;
    int fd;
    struct controller_info *next;
} *controller_list = NULL;

pthread_mutex_t ctrl_list_mutex;


void sighup(int s) {

}

int daemonize() {
    pid_t pid;

    /* Fork off the parent process */
    pid = fork();
    if (pid < 0)
        exit(EXIT_FAILURE);

    /* Let the parent terminate */
    if (pid > 0)
        exit(EXIT_SUCCESS);

    /* The child process becomes session leader */
    if (setsid() < 0)
        exit(EXIT_FAILURE);

    /* Catch, ignore and handle signals */
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, sighup);

    /* Fork off for the second time*/
    pid = fork();
    if (pid < 0)
        exit(EXIT_FAILURE);

    /* Let the parent terminate */
    if (pid > 0)
        exit(EXIT_SUCCESS);

    /* Set new file permissions */
    umask(0);

    /* Change the working directory to the root directory */
    if (chdir("/")) {
    }

    /* Close all open file descriptors */
    int x;
    for (x = sysconf(_SC_OPEN_MAX); x>=0; x--)
    {
        close (x);
    }

    return OK;
}

bool write_pidfile() {
    if (!pid_filename) {
        return true;
    }

    // open the pid file
    pid_file = fopen(pid_filename, "rw");
    if (!pid_file) {
        return false;
    }

    // try to aquire write lock
    struct flock fl;
    fl.l_type = F_WRLCK;
    fl.l_start = 0;
    fl.l_len = 0;
    fl.l_pid = getpid();
    if (fcntl(fileno(pid_file), F_SETLK, &fl) < 0 ) {
        // lock failed.....
        fclose(pid_file);
        return false;
    }

    // write pid to file
    int pid = getpid();
    fprintf(pid_file, "%d\n", pid);
    return true;
}

void unlink_pidfile() {
    // if pidfile was used....
    if (pid_file) {
        // release write lock... 
        // this is actualy not eccessary, since write lock is release on close
        struct flock fl;
        fl.l_type = F_UNLCK;
        fl.l_start = 0;
        fl.l_len = 0;
        fl.l_pid = getpid();
        fcntl(fileno(pid_file), F_SETLK, &fl);

        fclose(pid_file);
        pid_file = NULL;
    }
}

static void error(const char *fmt, ...);

static int open_syslog() {
    /* Open the log file */
    openlog (SYSLOG_NAME, LOG_PID, LOG_DAEMON);
    log_type = LOG_TOSYSLOG;
    return OK;
}

int open_logfile() {
    if (log_filename) {
        FILE *fp;
        fp = fopen(log_filename, "a");
        if (!fp) {
            error("cannot open logfile: %s", strerror(errno));

        } else {
            fclose(fp);
        }
    }
    return OK;
}

static void vlog(int priority, const char *fmt, va_list va) {

    FILE *fp;

    switch(log_type) {
        case LOG_STDERR:
            vfprintf(stdout, fmt, va);
            fprintf(stdout, "\n");
            break;
        case LOG_FILE:
            fp = fopen(log_filename, "a");
            if (fp) { 
                vfprintf(fp, fmt, va);
                fprintf(fp, "\n");
                fclose(fp);
            }
            break;
        case LOG_TOSYSLOG:
            vsyslog(priority, fmt, va);
            break;
    }
}

static void debug(const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    vlog(LOG_DEBUG, fmt, va);
}

static void info(const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    vlog(LOG_INFO, fmt, va);
}

static void fatal(const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    vlog(LOG_CRIT, fmt, va);
}

static void error(const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    vlog(LOG_ERR, fmt, va);
}

static void warning(const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    vlog(LOG_WARNING, fmt, va);
}

int make_fifo() {
    int ret;
    if (!fifo_path) {
        fifo_path = strdup(STD_FIFO_PATH);
    }
    unlink(fifo_path);
    ret = mkfifo(fifo_path, O_RDWR);
    if (ret) {
        fatal("failed to create control file: %s", strerror(errno));
        return FAIL;
    }
    chmod(fifo_path, 0666);
    return OK;
}

void unlink_fifo() {
    if (fifo_path) {
        unlink(fifo_path);
    }
}

int send_fifo(const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);

    if (!fifo_path) {
        fifo_path = strdup(STD_FIFO_PATH);
    }

    FILE *fp = fopen(fifo_path, "w");
    if (!fp) {
        //return FAIL;
    }
    vfprintf(fp, fmt, va);
    fprintf(fp,"\n");
    fclose(fp);

    return OK;
}

int read_fifo(char *buffer, size_t max_length) {
    char *p = buffer;
    size_t n = 0;
    FILE *fp = fopen(fifo_path, "r");
    if (!fp) {
        return 0;
    }
    while(n<max_length) {
        char ch;
        int ret = fread(&ch, 1, 1, fp);
        if (ret<=0) {
            fclose(fp);
            return 0;
        }
        if (ch=='\n') {
            *p = 0;
            return n;
        } else {
            *p++ = ch;
            ++n;
        }
    }
    p[-1] = 0;
    debug("read %d characters from fifo: %s", n, buffer);
    return n-1;
}

int check_devname(const char *devname) {
    char path[PATH_MAX];
    struct stat stat_buf;
return OK;
    snprintf(path, PATH_MAX, DEV_PATH, devname);
    if (lstat(path, &stat_buf))
        return FAIL;
    if (!S_ISCHR(stat_buf.st_mode))
        return FAIL;
    return OK;
}

int check_profile(const char *profile) {
    char path[PATH_MAX];
    struct stat stat_buf;

    snprintf(path, PATH_MAX, PROFILE_PATH, profile);
    if (stat(path, &stat_buf))
        return FAIL;
    if (!S_ISREG(stat_buf.st_mode))
        return FAIL;
        
    return OK;    
}

void init_threading() {
    pthread_mutex_init(&ctrl_list_mutex, NULL);
}

void finit_threading() {
    pthread_mutex_destroy(&ctrl_list_mutex);
}
 
void link_controller(struct controller_info * ctrl) {
    pthread_mutex_lock(&ctrl_list_mutex);

    ctrl->next = controller_list;
    controller_list = ctrl;

    pthread_mutex_unlock(&ctrl_list_mutex);
}

void unlink_controller(struct controller_info * ctrl) {
    pthread_mutex_lock(&ctrl_list_mutex);
    struct controller_info *parent = NULL;

    struct controller_info *p;
    for (p=controller_list; p; p=p->next) {
        if (p==ctrl) {
            if (parent) {
                parent->next = p->next;
            } else {
                controller_list = p->next;
            }
            p->next = NULL;
            break;
        }
        parent = p;
    }
    
    pthread_mutex_unlock(&ctrl_list_mutex);
}

struct controller_info *find_controller(const char *dev) {
    pthread_mutex_lock(&ctrl_list_mutex);

    struct controller_info *p;
    for (p=controller_list; p; p=p->next) {
        if (strcmp(dev, p->dev_name)==0)
            break;
    }

    pthread_mutex_unlock(&ctrl_list_mutex);
    return p;
}

void release_controller(struct controller_info * ctrl) {
    if (ctrl->dev_name) free(ctrl->dev_name);
    if (ctrl->dev_path) free(ctrl->dev_path);
    if (ctrl->uniq_id) free(ctrl->uniq_id);
    if (ctrl->profile) free(ctrl->profile);
    delete ctrl;
}

struct controller_info *get_controller(char *dev, char *profile) {
    char path[PATH_MAX];
    struct controller_info *ctrl;

    ctrl = new controller_info;
    assert(ctrl != NULL);

    ctrl->dev_name = strdup(dev);

    snprintf(path, PATH_MAX, DEV_PATH, dev);
    ctrl->dev_path = strdup(path);
    if (access(path, F_OK)!=0) {
        release_controller(ctrl);
        return NULL;
    }

    snprintf(path, PATH_MAX, SYS_HID_INFO_PATH, dev);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        release_controller(ctrl);
        return NULL;
    }

    int name_ok = 0;
    while(!feof(fp)) {
        char line[100];
        if (!fgets(line, 100, fp)) {
            break;
        }
        if (strncmp(line, HID_NAME, strlen(HID_NAME)) == 0) 
            name_ok = 1;
        if (strncmp(line, HID_UNIQ, strlen(HID_UNIQ)) == 0) {
            int len = strlen(line) - strlen(HID_UNIQ); 
            if (line[len-1] == '\n') {
                len --;
            }
            char *p = (char *) malloc(len+1);
            assert( p!=0 );
            strncpy(p, line+strlen(HID_UNIQ), len-1);
            ctrl->uniq_id = p;
        }
    }
    fclose(fp);
    if (!name_ok) {
        release_controller(ctrl);
        return NULL;
    }

    const char *p;
    if (!profile) {
        p = ctrl->uniq_id;
    } else {
        p = profile;
    }

    char symlink[1024];
    struct stat stat_buf;

    snprintf(path, PATH_MAX, PROFILE_PATH, p);
    if (lstat(path, &stat_buf)) {
        p = DEFAULT_PROFILE;
    } else {
        if (S_ISLNK(stat_buf.st_mode)) {
            if (readlink(path, symlink, 1024)<0) {
                p = DEFAULT_PROFILE;
            } else {
                p = symlink;
            }
        }
    }

    if (check_profile(p)) {
        ctrl->profile = strdup(p);
    } else {
        ctrl->profile = strdup(DEFAULT_PROFILE);
    }

    return ctrl;

}

void controller_loop(struct controller_info * ctrl) {

    int fd = 0;
    int first_packet = 1;
    struct uinput_fd *ufd = NULL;

    struct device_settings settings = init_values(ctrl->profile);

    fd = open(ctrl->dev_path, O_RDONLY);
    if (!fd) {
        error("cannot open %s for reading: %s", strerror(fd));
        goto done;
    }


    ufd = uinput_open(DEV_TYPE_SIXAXIS, "hidraw", settings);
    if (ufd->js < 0 || ufd->mk < 0) {
        goto done;
    }

info("starting controller-loop for [%s] using profile '%s'", ctrl->uniq_id, ctrl->profile);

    for (;;) {
        unsigned char buffer[128];
        int sz = read(fd, buffer, sizeof buffer);
//info("read %d bytes from device", sz);
        if (sz==0) {
            break;
        }
        if (sz < 49 || sz > 50) {
            error("protocol error on %s, detaching device", ctrl->dev_name);
            break;
        }
        if (sz == 49) {
            for (int i=50; i>0; i--) {
                buffer[i] = buffer[i-1];
            }
        }
        if (first_packet) {
            first_packet = 0;
        }

        if (settings.joystick.enabled) 
            do_joystick(ufd->js, buffer, settings.joystick);
        if (settings.input.enabled) 
            do_input(ufd->mk, buffer, settings.input);
    }

info("stopping controller-loop for %s", ctrl->dev_name);

done:
    if (fd>0) close(fd);

    if (ufd) {
        if (ufd->js>0) close(ufd->js);
        if (ufd->js>0) close(ufd->js);
        free(ufd);
    }

    unlink_controller(ctrl);
    release_controller(ctrl);
}

void *controller_thread(void *arg) {
    struct controller_info * ctrl;
    ctrl = (struct controller_info *) arg;
    controller_loop(ctrl);
    return NULL;
}

const char *short_options = "fba:d:qht:p:lP:L:";
struct option long_options[] = {
    { "foreground", no_argument, 0, 'f' },
    { "start",     no_argument, 0, 'b' },

    { "attach",     required_argument, 0, 'a' },
    { "detach",     required_argument, 0, 'r' },
    { "stop",       no_argument, 0, 'q' },

    { "help",       no_argument, 0, 'h' },
    { "test",       required_argument, 0, 't'},
    { "profile",    required_argument, 0, 'p' },

    { "list",       required_argument, 0, 'l' },

    { "pidfile",    required_argument, 0, 'P' },
    { "logfile",    required_argument, 0, 'L' },


    { 0, 0, 0, 0}
};


void help(char * const argv[]) {
    printf(
        "usage: %s <options\n"
        "\n"
        "options can be:\n"
        "\t-h, --help              Show this help\n"
        "\t-b, --start             Start daemon\n"
        "\t-a, --attach <name>     Attach controller /dev/<name>\n"
        "\t-d, --detach <name>     Detach previously attached controller <name>\n"
        "\t-q, --stop              Stop daemon\n"
        "\t-f, --foreground        Start daemon in foreground, mainly used for debugging\n"

    , argv[0]);

}

int main(int argc, char * const argv[]) {

    enum {
        RUN_NONE,
        RUN_BACKGROUND, RUN_FOREGROUND,
        RUN_ATTACH, RUN_DETACH, RUN_TERMINATE,
        RUN_TEST,
        RUN_LIST
    } run_mode = RUN_NONE;
    int ret = EXIT_SUCCESS;

    for (;;) {
        int option = getopt_long(argc, argv, short_options, long_options, 0);
        if (option<0) break;
        switch(option) {
            case 'h': // --help
                help(argv);
                return EXIT_SUCCESS;

            case 'b': // --daemon
                run_mode = RUN_BACKGROUND;
                break;

            case 'f': // --foreground
                run_mode = RUN_FOREGROUND;
                break;

            case 't': // --test
                run_mode = RUN_TEST;
                dev_name = strdup(optarg);
                break;

            case 'a': // --attach <hidraw>
                run_mode = RUN_ATTACH;
                dev_name = strdup(optarg);
                break;

            case 'd': // --detach <hidraw>
                run_mode = RUN_DETACH;
                dev_name = strdup(optarg);
                break;

            case 'q': // --detach <hidraw>
                run_mode = RUN_TERMINATE;
                break;

            case 'l': // --detach <hidraw>
                run_mode = RUN_LIST;
                break;

            case 'p': // --profile <name>
                profile_name = strdup(optarg);
                break;

            case 'P': // --pidfile <path>
                if (pid_filename)
                    free(pid_filename);
                pid_filename = (char *) malloc(PATH_MAX);
                assert(pid_filename!=NULL);
                if (!realpath(optarg, pid_filename)) {
                    error("cannot resolve pid file path: %s", strerror(errno));
                }
                break;            

            case 'L': // --logfile <path>
                if (log_filename)
                    free(log_filename);
                log_filename = (char *) malloc(PATH_MAX);
                assert(log_filename!=NULL);
                if (!realpath(optarg, log_filename)) {
                    error("cannot resolve log file path: %s", strerror(errno));
                }
                break;            

            case ':':
            case '?':
                help(argv);
                goto exit_failure;
        }
    }


    if (run_mode==RUN_FOREGROUND || run_mode==RUN_BACKGROUND) {

        if (!open_logfile())
            goto exit_failure;

        if (run_mode==RUN_BACKGROUND) {
            if (!daemonize())
                goto exit_failure;
            if (log_type==LOG_STDERR) {
                open_syslog();
            }            
        }
        if (!make_fifo()) {
            goto exit_failure;
        }

        init_threading();
        
        info("sixad-usbd listening on %s", fifo_path);
        for(int quit=0;!quit;) {
            char line[80];
            char *cmd=0, *dev=0, *profile=0;
            pthread_t thread_id;
            int err;

            if (read_fifo(line, 80)) {
                sscanf(line, "%ms %ms %ms", &cmd, &dev, &profile);

                if (strcmp(cmd, "attach")==0) {
                    if (!dev) {
                        error("protocol error: missing dev name on attach");
                        goto loop;
                    }
                    if (!check_devname(dev)) {
                        error("device %s not found on attach", dev);
                        goto loop;
                    }
                    if (profile) {
                        if (!check_profile(profile)) {
                            error("profile %s not found on attach", profile);
                            goto loop;
                        }
                    }

                    info("attached controller at %s using profile %s", dev, profile);

                    struct controller_info *ctrl = get_controller(dev, profile);
                    if (ctrl) {
                        ctrl->thread_id = thread_id;
                        err = pthread_create(&thread_id, NULL, controller_thread, ctrl);
                        if (err) {
                            error("internal error: cannot create controller thread: %s", strerror(err));
                        }
                    }
                } else if (strcmp(cmd, "detach")==0) {

                } else if (strcmp(cmd, "quit")==0) {
                    quit=1;
                }
            loop:
                if (cmd) free(cmd);
                if (dev) free(dev);
                if (profile) free(profile);
            }
        }

        finit_threading();
        unlink_fifo();
        unlink_pidfile();

    } else if (run_mode==RUN_ATTACH) {
        if (!check_devname(dev_name))
            goto exit_failure;
        if (profile_name) {
            if (!send_fifo("attach %s %s", dev_name, profile_name))
                goto exit_failure;
        } else
            if (!send_fifo("attach %s", dev_name))
                goto exit_failure;

    } else if (run_mode==RUN_DETACH) {
        if (!check_devname(dev_name))
            goto exit_failure;
        if (!send_fifo("detach %s", dev_name))
            goto exit_failure;

    } else if (run_mode==RUN_TERMINATE) {
        if (!send_fifo("quit", dev_name))
            goto exit_failure;

    } else if (run_mode==RUN_LIST) {
        DIR *dir;
        struct dirent *entry;
        
        dir = opendir("/dev");
        if (dir) {
            for (;;) {
                entry = readdir(dir);
                if (!entry)  break;

                if (strncmp("hidraw", entry->d_name, 6)==0) {
                    struct controller_info *ctrl;
                    ctrl = get_controller(entry->d_name, NULL);
                    if (ctrl) {
                        printf("%s: Sony controller %s, profile:%s\n",
                            entry->d_name, ctrl->uniq_id, ctrl->profile);
                        release_controller(ctrl);
                    } else {
                        printf("%s: not a Sony controller\n", entry->d_name);
                    }
                }
            }
            closedir(dir);
        }

    } else if (run_mode==RUN_TEST) {
        struct controller_info *ctrl;
        ctrl = get_controller(dev_name, profile_name);
        if (!ctrl) {
            error("cannot find controller");
            goto exit_failure;
        }
        printf("handling controller %s with profile %s", ctrl->dev_name, ctrl->profile);
        controller_loop(ctrl);

    } else {
        help(argv);
        goto exit_failure;
    }

exit_success:
    return ret;

exit_failure:
    ret = EXIT_FAILURE;
    goto exit_success;
}


