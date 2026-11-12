#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static pid_t child = -1;
static int terminal = -1;
static char output[262144];
static size_t used;
static char fixture[256];

typedef int (*sqlite_open_fn)(const char *, sqlite3 **);
typedef int (*sqlite_exec_fn)(sqlite3 *, const char *, int (*)(void *, int, char **, char **), void *, char **);
typedef int (*sqlite_close_fn)(sqlite3 *);

static void cleanup(void) {
    if (child > 0) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }
    if (terminal >= 0) close(terminal);
    if (!fixture[0]) return;
    char path[512];
    const char *files[] = {
        "/.claude/projects/demo/session.jsonl", "/.claude/usage-history.jsonl", "/opencode/opencode.db"
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        snprintf(path, sizeof(path), "%s%s", fixture, files[i]);
        unlink(path);
    }
    const char *dirs[] = {"/.claude/projects/demo", "/.claude/projects", "/.claude", "/.gemini", "/opencode", ""};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        snprintf(path, sizeof(path), "%s%s", fixture, dirs[i]);
        rmdir(path);
    }
}

static void require(bool ok, const char *message) {
    if (ok) return;
    fprintf(stderr, "TUI test failed: %s\nCaptured output:\n%.*s\n", message, (int)used, output);
    exit(1);
}

static void create_opencode_fixture(const char *path, time_t now) {
    void *library = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
    require(library != NULL, "load sqlite3");
    sqlite_open_fn open_db = (sqlite_open_fn)dlsym(library, "sqlite3_open");
    sqlite_exec_fn exec_sql = (sqlite_exec_fn)dlsym(library, "sqlite3_exec");
    sqlite_close_fn close_db = (sqlite_close_fn)dlsym(library, "sqlite3_close");
    require(open_db && exec_sql && close_db, "resolve sqlite3");
    sqlite3 *db = NULL;
    require(open_db(path, &db) == SQLITE_OK, "open opencode fixture");
    long long created = (long long)now * 1000 - 3600000;
    char sql[2048];
    snprintf(
        sql,
        sizeof(sql),
        "CREATE TABLE session_v2(id,title,directory,time_updated);"
        "CREATE TABLE session_message(id,session_id,type,seq,time_created,time_updated,data);"
        "INSERT INTO session_v2 VALUES('ses-tui','TUI session','/tmp/tui',%lld);"
        "INSERT INTO session_message VALUES('msg-tui','ses-tui','assistant',1,%lld,%lld,"
        "'{\"time\":{\"created\":%lld},\"model\":{\"providerID\":\"opencode-go\",\"id\":\"qwen3.8-flash\"},"
        "\"tokens\":{\"input\":10,\"output\":20,\"reasoning\":0,"
        "\"cache\":{\"read\":5,\"write\":0}}}');",
        created,
        created,
        created,
        created
    );
    require(exec_sql(db, sql, NULL, NULL, NULL) == SQLITE_OK, "write opencode fixture");
    require(close_db(db) == SQLITE_OK, "close opencode fixture");
    dlclose(library);
}

static double monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1e9;
}

static void drain(int milliseconds) {
    struct pollfd descriptor = {.fd = terminal, .events = POLLIN};
    if (poll(&descriptor, 1, milliseconds) <= 0) return;
    ssize_t count;
    while ((count = read(terminal, output + used, sizeof(output) - used - 1)) > 0) {
        used += (size_t)count;
        output[used] = '\0';
        require(used < sizeof(output) - 1, "capture buffer overflow");
    }
}

static void expect(const char *text) {
    double deadline = monotonic_seconds() + 5;
    while (!strstr(output, text) && monotonic_seconds() < deadline) drain(50);
    require(strstr(output, text) != NULL, text);
}

static void key(const char *text) {
    used = 0;
    output[0] = '\0';
    require(write(terminal, text, strlen(text)) == (ssize_t)strlen(text), "send key");
}

static void launch(const char *executable, const char *provider) {
    struct winsize size = {.ws_row = 30, .ws_col = 160};
    used = 0;
    output[0] = '\0';
    child = forkpty(&terminal, NULL, NULL, &size);
    require(child >= 0, "forkpty");
    if (child == 0) {
        setenv("HOME", fixture, 1);
        setenv("XDG_RUNTIME_DIR", fixture, 1);
        setenv("XDG_DATA_HOME", fixture, 1);
        setenv("XDG_CACHE_HOME", fixture, 1);
        setenv("TERM", "xterm-256color", 1);
        setenv("LC_ALL", "C.UTF-8", 1);
        if (provider)
            execl(executable, executable, "--provider", provider, "--refresh", "0", (char *)NULL);
        execl(executable, executable, "--refresh", "0", (char *)NULL);
        _exit(127);
    }
    require(fcntl(terminal, F_SETFL, O_NONBLOCK) == 0, "nonblocking PTY");
}

static void finish(int signal_number) {
    if (signal_number) require(kill(child, signal_number) == 0, "send signal");
    else key("q");
    double deadline = monotonic_seconds() + 5;
    int status = 0;
    pid_t ended = 0;
    while (monotonic_seconds() < deadline) {
        drain(30);
        ended = waitpid(child, &status, WNOHANG);
        if (ended == child) break;
    }
    require(ended == child, "exit timeout");
    child = -1;
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "clean exit");
    struct termios settings;
    require(tcgetattr(terminal, &settings) == 0, "read restored terminal settings");
    require((settings.c_lflag & (ICANON | ECHO)) == (ICANON | ECHO), "restore canonical input and echo");
    close(terminal);
    terminal = -1;
}

int main(int argc, char **argv) {
    require(argc == 2, "expected TUI executable path");
    atexit(cleanup);
    snprintf(fixture, sizeof(fixture), "/tmp/ai-usage-tui-test-XXXXXX");
    require(mkdtemp(fixture) != NULL, "create fixture");
    char path[512];
    const char *dirs[] = {"/.claude", "/.claude/projects", "/.claude/projects/demo", "/.gemini", "/opencode"};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        snprintf(path, sizeof(path), "%s%s", fixture, dirs[i]);
        require(mkdir(path, 0700) == 0, "create fixture directory");
    }
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    snprintf(path, sizeof(path), "%s/.claude/projects/demo/session.jsonl", fixture);
    FILE *file = fopen(path, "w");
    require(file != NULL, "open session fixture");
    fprintf(file, "{\"type\":\"assistant\",\"timestamp\":\"%s\",\"sessionId\":\"tui-session\",\"cwd\":\"tui-test-project/café\",\"message\":{\"id\":\"tui-message\",\"model\":\"tui-test-model\",\"usage\":{\"input_tokens\":100,\"output_tokens\":50}}}\n", timestamp);
    require(fclose(file) == 0, "write session fixture");
    snprintf(path, sizeof(path), "%s/.claude/usage-history.jsonl", fixture);
    file = fopen(path, "w");
    require(file != NULL, "open quota fixture");
    fprintf(file, "{\"t\":%lld,\"pct\":25,\"reset\":%lld,\"pct7\":40,\"reset7\":%lld}\n", (long long)now, (long long)now + 9000, (long long)now + 300000);
    require(fclose(file) == 0, "write quota fixture");

    launch(argv[1], NULL);
    expect("claude");
    expect("25.0%");
    expect("Local rolling usage");
    key("2");
    expect("tui-test-model");
    key("3");
    char day[11];
    snprintf(day, sizeof(day), "%.10s", timestamp);
    expect(day);
    key("4");
    expect("tui-test-project");
    expect("café");
    key("1");
    expect("25.0%");
    key("p");
    expect("claude");
    key("d");
    expect("30");
    key("r");
    expect("Refreshing...");
    key("2");
    expect("tui-test-model");
    key("1");
    expect("25.0%");
    struct winsize narrow = {.ws_row = 8, .ws_col = 24};
    require(ioctl(terminal, TIOCSWINSZ, &narrow) == 0, "resize small");
    drain(100);
    struct winsize normal = {.ws_row = 30, .ws_col = 80};
    used = 0;
    output[0] = '\0';
    require(ioctl(terminal, TIOCSWINSZ, &normal) == 0, "resize normal");
    expect("25.0%");
    key("p");
    expect("codex");
    key("2");
    expect("No usage");
    key("ppp");
    expect("tui-test-model");
    expect("150");
    finish(0);

    launch(argv[1], "codex");
    expect("codex");
    key("2");
    expect("No ");
    finish(SIGTERM);

    snprintf(path, sizeof(path), "%s/opencode/opencode.db", fixture);
    create_opencode_fixture(path, now);
    launch(argv[1], "opencode");
    expect("opencode");
    expect("Quota / rate limits");
    expect("5h");
    expect("est. from");
    key("2");
    expect("opencode-go/qwen3.8-flash");
    finish(SIGTERM);

    puts("TUI pseudo-terminal tests passed.");
    return 0;
}
