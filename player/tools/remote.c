/* pidvd-remote — a pocket web remote for the picker UI.
 *
 * The player normalizes every input device to a word it reads from the
 * /tmp/pidvd-ctl FIFO (see platform/linux/input_evdev.c). This is a tiny
 * dependency-free HTTP server that serves one self-contained remote-control
 * page and, on each button tap, writes the matching word to that FIFO — so a
 * phone or laptop on the LAN becomes the remote when no keyboard/IR is
 * plugged in. It is "just another thing that writes the FIFO", exactly like
 * the `echo up > /tmp/pidvd-ctl` over ssh that docs/UI.md describes, so it
 * needs no cooperation from the player and can come and go independently.
 *
 *   pidvd-remote [port] [fifo] [stat]   default: 8080 /tmp/pidvd-ctl
 *                                       /tmp/pidvd-stat
 *
 * It also serves GET /stat: the live presentation rate and 1% low that the
 * player publishes to that stats file once a second, which the page polls.
 *
 * Pure POSIX sockets: builds and runs on the Pi and on the macOS host (point
 * it at a FIFO you `cat` to watch the words for UI work without a board).
 */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_PORT 8080
#define DEFAULT_FIFO "/tmp/pidvd-ctl"
#define DEFAULT_STAT "/tmp/pidvd-stat"

/* The control words the player understands (input_evdev.c map_word). The
 * remote can write nothing else — this list is the whole attack surface. */
static const char *const KEYS[] = {
    "up",   "down", "left",  "right", "enter", "menu",  "title",
    "pause", "stop", "next", "prev",  "audio", "sub",
    "volup", "voldown", NULL,
};

static const char *fifo_path = DEFAULT_FIFO;
static const char *stat_path = DEFAULT_STAT;

static int valid_key(const char *w)
{
    for (int i = 0; KEYS[i]; i++)
        if (!strcmp(w, KEYS[i]))
            return 1;
    return 0;
}

/* Open the FIFO non-blocking and write "<word>\n". The player holds the read
 * end open (O_RDWR), so a write succeeds whenever it is running; if it is not
 * (ENXIO: no reader, or the FIFO is missing), report failure rather than
 * block or die. The word is tiny, so the write is atomic. */
static int send_key(const char *word)
{
    int fd = open(fifo_path, O_WRONLY | O_NONBLOCK);
    if (fd < 0)
        return -1;
    char line[16];
    int n = snprintf(line, sizeof(line), "%s\n", word);
    ssize_t w = write(fd, line, (size_t)n);
    close(fd);
    return w == n ? 0 : -1;
}

/* The remote page. Self-contained (no external CSS/JS/fonts — the appliance
 * has no guaranteed internet) and themed to match the on-screen amber picker
 * (docs/UI.md §2). Buttons fire on pointerdown for a hardware-remote feel. */
static const char PAGE[] =
"<!doctype html><html lang=en><head><meta charset=utf-8>"
"<meta name=viewport content=\"width=device-width,initial-scale=1,"
"maximum-scale=1,user-scalable=no\">"
"<title>PiDVD Remote</title><style>"
":root{--bg:#0d0a06;--panel:#161310;--dim:#4e6a86;--text:#d98e00;"
"--bright:#f4efe2;--bar:#ffa000}"
"*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}"
"html,body{margin:0;height:100%;background:var(--bg);color:var(--text);"
"font-family:ui-monospace,Menlo,Consolas,monospace;"
"user-select:none;-webkit-user-select:none}"
"body{display:flex;flex-direction:column;align-items:center;"
"justify-content:center;gap:18px;padding:18px}"
".hd{letter-spacing:.35em;color:var(--bright);font-size:15px}"
".hd b{color:var(--bar)}"
".pad{display:grid;grid-template-columns:repeat(3,72px);"
"grid-template-rows:repeat(3,72px);gap:10px}"
".row{display:flex;gap:10px;flex-wrap:wrap;justify-content:center;"
"max-width:264px}"
"button{font:inherit;font-size:15px;color:var(--text);background:var(--panel);"
"border:2px solid var(--dim);border-radius:12px;padding:0;height:72px;"
"min-width:72px;flex:1;cursor:pointer;transition:none}"
"button.wide{flex:1 1 78px}"
"button:active,button.hit{background:var(--bar);color:var(--bg);"
"border-color:var(--bar)}"
".pad .up{grid-area:1/2}.pad .left{grid-area:2/1}.pad .ok{grid-area:2/2;"
"font-size:18px;color:var(--bright)}.pad .right{grid-area:2/3}"
".pad .down{grid-area:3/2}"
".st{height:16px;font-size:12px;letter-spacing:.2em;color:var(--dim)}"
".stats{display:flex;gap:22px;font-size:12px;letter-spacing:.18em;"
"color:var(--dim)}.stats b{color:var(--bright);font-size:15px;"
"letter-spacing:.05em}.stats .lo b{color:var(--bar)}"
"</style></head><body>"
"<div class=hd>\xe2\x97\x89 <b>PiDVD</b> REMOTE</div>"
"<div class=stats>"
"<span>FPS <b id=fps>\xe2\x80\x94</b></span>"
"<span class=lo>1% LOW <b id=low>\xe2\x80\x94</b></span>"
"</div>"
"<div class=pad>"
"<button class=up data-k=up>\xe2\x96\xb2</button>"
"<button class=left data-k=left>\xe2\x97\x82</button>"
"<button class=ok data-k=enter>OK</button>"
"<button class=right data-k=right>\xe2\x96\xb8</button>"
"<button class=down data-k=down>\xe2\x96\xbc</button>"
"</div>"
"<div class=row>"
"<button class=wide data-k=menu>MENU</button>"
"<button class=wide data-k=title>TITLE</button>"
"</div>"
"<div class=row>"
"<button class=wide data-k=prev>\xe2\x8f\xae</button>"
"<button class=wide data-k=pause>\xe2\x96\xb6\xe2\x9d\x9a</button>"
"<button class=wide data-k=stop>\xe2\x96\xa0</button>"
"<button class=wide data-k=next>\xe2\x8f\xad</button>"
"</div>"
"<div class=row>"
"<button class=wide data-k=audio>AUDIO</button>"
"<button class=wide data-k=sub>SUB</button>"
"</div>"
"<div class=row>"
"<button class=wide data-k=voldown>VOL \xe2\x88\x92</button>"
"<button class=wide data-k=volup>VOL +</button>"
"</div>"
"<div class=st id=st>&nbsp;</div>"
"<script>"
"var st=document.getElementById('st');"
"function tap(b){var k=b.dataset.k;"
"b.classList.add('hit');setTimeout(function(){b.classList.remove('hit')},120);"
"fetch('/k/'+k,{method:'POST'}).then(function(r){"
"st.textContent=r.ok?k.toUpperCase():'\xe2\x9c\x95 '+k.toUpperCase()+' (player?)'"
"}).catch(function(){st.textContent='\xe2\x9c\x95 OFFLINE'});}"
"document.querySelectorAll('button').forEach(function(b){"
"b.addEventListener('pointerdown',function(e){e.preventDefault();tap(b)});});"
"var fps=document.getElementById('fps'),low=document.getElementById('low');"
"function fmt(v){v=parseFloat(v);return(v>0&&isFinite(v))?v.toFixed(1):'\xe2\x80\x94'}"
"function poll(){fetch('/stat').then(function(r){return r.text()}).then("
"function(t){var a=t.trim().split(/\\s+/);fps.textContent=fmt(a[0]);"
"low.textContent=fmt(a[1])}).catch(function(){fps.textContent='\xe2\x80\x94';"
"low.textContent='\xe2\x80\x94'});}"
"setInterval(poll,1000);poll();"
"</script></body></html>";

static void respond(int fd, const char *status, const char *ctype,
                    const char *body, size_t len)
{
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "Cache-Control: no-store\r\n\r\n",
                     status, ctype, len);
    if (write(fd, hdr, (size_t)n) < 0)
        return;
    if (len)
        (void)!write(fd, body, len);
}

static void handle(int fd)
{
    char buf[1024];
    ssize_t r = recv(fd, buf, sizeof(buf) - 1, 0);
    if (r <= 0)
        return;
    buf[r] = 0;

    char method[8], path[256];
    if (sscanf(buf, "%7s %255s", method, path) != 2) {
        respond(fd, "400 Bad Request", "text/plain", "bad\n", 4);
        return;
    }

    if (!strncmp(path, "/k/", 3)) {
        char word[16];
        snprintf(word, sizeof(word), "%s", path + 3); /* truncates safely */
        for (char *p = word; *p; p++)              /* trim ?query / #frag */
            if (*p == '?' || *p == '#') { *p = 0; break; }
        if (!valid_key(word))
            respond(fd, "400 Bad Request", "text/plain", "bad key\n", 8);
        else if (send_key(word) == 0)
            respond(fd, "204 No Content", "text/plain", "", 0);
        else
            respond(fd, "503 Service Unavailable", "text/plain",
                    "no player\n", 10);
        return;
    }

    /* Live stats the player publishes once a second: "<fps> <1%low>". Served
     * raw for the page to poll; absent/empty (no playback) reads as idle. */
    if (!strcmp(path, "/stat")) {
        char body[64] = "0 0\n";
        size_t len = 4;
        int sfd = open(stat_path, O_RDONLY);
        if (sfd >= 0) {
            ssize_t k = read(sfd, body, sizeof(body) - 1);
            close(sfd);
            if (k > 0)
                len = (size_t)k;
        }
        respond(fd, "200 OK", "text/plain", body, len);
        return;
    }

    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        respond(fd, "200 OK", "text/html; charset=utf-8", PAGE,
                sizeof(PAGE) - 1);
        return;
    }

    respond(fd, "404 Not Found", "text/plain", "not found\n", 10);
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : DEFAULT_PORT;
    if (argc > 2)
        fifo_path = argv[2];
    if (argc > 3)
        stat_path = argv[3];
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "usage: %s [port] [fifo] [stat]\n", argv[0]);
        return 2;
    }

    signal(SIGPIPE, SIG_IGN); /* a client vanishing mid-write must not kill us */

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));

    /* Bind INADDR_ANY: works before DHCP gives eth0 an address, so boot
     * order vs. the network init script does not matter — it serves the
     * moment the lease arrives. */
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(s, 16) < 0) {
        perror("listen");
        return 1;
    }
    fprintf(stderr, "pidvd-remote: http://0.0.0.0:%d/  ->  %s\n", port,
            fifo_path);

    /* One user, a handful of taps a second: a sequential accept loop is
     * plenty. A per-client recv timeout keeps a stalled connection from
     * wedging the whole remote. */
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0)
            continue;
        struct timeval tv = { .tv_sec = 5 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        handle(c);
        close(c);
    }
}
