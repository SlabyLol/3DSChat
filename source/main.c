#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#define PORT        7496
#define MAX_MSGS    32
#define MSG_LEN     200
#define NAME_LEN    32
#define BACKLOG     4
#define RECV_BUF    4096

// ── Message history ──────────────────────────────────────────────────
typedef struct {
    char name[NAME_LEN];
    char text[MSG_LEN];
    char time_str[16];
} ChatMsg;

static ChatMsg  g_msgs[MAX_MSGS];
static int      g_msg_count = 0;

static void push_message(const char *name, const char *text) {
    // shift if full
    if (g_msg_count == MAX_MSGS) {
        memmove(&g_msgs[0], &g_msgs[1], sizeof(ChatMsg) * (MAX_MSGS - 1));
        g_msg_count = MAX_MSGS - 1;
    }
    ChatMsg *m = &g_msgs[g_msg_count++];
    snprintf(m->name, NAME_LEN, "%s", name);
    snprintf(m->text, MSG_LEN, "%s", text);

    // simple time from osGetTime (ms since boot → HH:MM:SS)
    u64 ms   = osGetTime();
    u32 secs = (u32)(ms / 1000);
    snprintf(m->time_str, 16, "%02u:%02u:%02u",
             (secs / 3600) % 24, (secs / 60) % 60, secs % 60);
}

// ── Simple URL-decode ─────────────────────────────────────────────────
static void url_decode(const char *src, char *dst, size_t dstlen) {
    size_t i = 0;
    while (*src && i < dstlen - 1) {
        if (*src == '+') { dst[i++] = ' '; src++; }
        else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else { dst[i++] = *src++; }
    }
    dst[i] = '\0';
}

// ── Strip dangerous HTML chars ────────────────────────────────────────
static void html_escape(const char *src, char *dst, size_t dstlen) {
    size_t i = 0;
    while (*src && i < dstlen - 5) {
        if      (*src == '<')  { memcpy(dst+i,"&lt;",  4); i+=4; }
        else if (*src == '>')  { memcpy(dst+i,"&gt;",  4); i+=4; }
        else if (*src == '&')  { memcpy(dst+i,"&amp;", 5); i+=5; }
        else if (*src == '"')  { memcpy(dst+i,"&#34;", 5); i+=5; }
        else                   { dst[i++] = *src; }
        src++;
    }
    dst[i] = '\0';
}

// ── Build the HTML page ───────────────────────────────────────────────
static const char HTML_HEAD[] =
"HTTP/1.0 200 OK\r\n"
"Content-Type: text/html; charset=utf-8\r\n"
"Connection: close\r\n\r\n"
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>3DS Chat</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:'Segoe UI',sans-serif;background:#1a1a2e;color:#eee;min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:16px}"
"h1{color:#e94560;margin-bottom:16px;font-size:1.6rem;letter-spacing:2px;text-transform:uppercase}"
"#chat{width:100%;max-width:600px;background:#16213e;border-radius:12px;padding:16px;margin-bottom:12px;height:55vh;overflow-y:auto;display:flex;flex-direction:column;gap:8px}"
".msg{background:#0f3460;border-radius:8px;padding:8px 12px}"
".msg .meta{font-size:.75rem;color:#a0a0c0;margin-bottom:2px}"
".msg .meta .name{color:#e94560;font-weight:700}"
".msg .body{font-size:.95rem;word-break:break-word}"
"form{width:100%;max-width:600px;display:flex;flex-direction:column;gap:8px}"
"input,button{border:none;border-radius:8px;padding:10px 14px;font-size:1rem}"
"input{background:#0f3460;color:#eee;flex:1}"
"input::placeholder{color:#556}"
"button{background:#e94560;color:#fff;cursor:pointer;font-weight:700;letter-spacing:1px}"
"button:hover{background:#c73652}"
"#status{font-size:.8rem;color:#667;margin-top:4px;text-align:center}"
"</style></head><body>"
"<h1>&#127918; 3DS Chat</h1>"
"<div id='chat'>";

static const char HTML_FORM[] =
"</div>"
"<form method='POST' action='/send'>"
"<input name='name' placeholder='Dein Name' maxlength='31' required>"
"<input name='msg'  placeholder='Nachricht...' maxlength='199' required>"
"<button type='submit'>Senden &#128172;</button>"
"</form>"
"<p id='status'>Verbunden mit 3DS &#10003; &nbsp;|&nbsp; Port 7496</p>"
"<script>"
"setTimeout(()=>location.reload(),5000);" // auto-refresh every 5s
"window.onload=()=>{var c=document.getElementById('chat');c.scrollTop=c.scrollHeight;};"
"</script>"
"</body></html>";

static void send_page(int fd) {
    char buf[256];
    write(fd, HTML_HEAD, sizeof(HTML_HEAD)-1);

    if (g_msg_count == 0) {
        const char *empty = "<p style='color:#556;margin:auto'>Noch keine Nachrichten. Sag Hallo! &#128075;</p>";
        write(fd, empty, strlen(empty));
    }

    for (int i = 0; i < g_msg_count; i++) {
        char safe_name[NAME_LEN*4], safe_text[MSG_LEN*4];
        html_escape(g_msgs[i].name, safe_name, sizeof(safe_name));
        html_escape(g_msgs[i].text, safe_text, sizeof(safe_text));

        int len = snprintf(buf, sizeof(buf),
            "<div class='msg'><div class='meta'>"
            "<span class='name'>%s</span> &nbsp;%s</div>"
            "<div class='body'>%s</div></div>",
            safe_name, g_msgs[i].time_str, safe_text);
        write(fd, buf, len);
    }
    write(fd, HTML_FORM, sizeof(HTML_FORM)-1);
}

static void send_redirect(int fd) {
    const char *r =
        "HTTP/1.0 302 Found\r\n"
        "Location: /\r\n"
        "Connection: close\r\n\r\n";
    write(fd, r, strlen(r));
}

// ── Parse POST body: name=...&msg=... ─────────────────────────────────
static void handle_post(int fd, const char *body) {
    char raw_name[NAME_LEN*3] = {0};
    char raw_msg [MSG_LEN*3]  = {0};
    char dec_name[NAME_LEN]   = {0};
    char dec_msg [MSG_LEN]    = {0};

    // extract name=
    const char *p = strstr(body, "name=");
    if (p) {
        p += 5;
        const char *end = strchr(p, '&');
        size_t len = end ? (size_t)(end-p) : strlen(p);
        if (len >= sizeof(raw_name)) len = sizeof(raw_name)-1;
        memcpy(raw_name, p, len);
    }
    // extract msg=
    p = strstr(body, "msg=");
    if (p) {
        p += 4;
        const char *end = strchr(p, '&');
        size_t len = end ? (size_t)(end-p) : strlen(p);
        if (len >= sizeof(raw_msg)) len = sizeof(raw_msg)-1;
        memcpy(raw_msg, p, len);
    }

    url_decode(raw_name, dec_name, sizeof(dec_name));
    url_decode(raw_msg,  dec_msg,  sizeof(dec_msg));

    if (dec_name[0] && dec_msg[0])
        push_message(dec_name, dec_msg);

    send_redirect(fd);
}

// ── Handle one client connection ──────────────────────────────────────
static void handle_client(int fd) {
    char buf[RECV_BUF];
    int  total = 0, n;

    // read until end of headers (or body arrives)
    while (total < RECV_BUF - 1) {
        n = recv(fd, buf + total, RECV_BUF - 1 - total, 0);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    if (total == 0) return;
    buf[total] = '\0';

    // determine method + path
    int is_post = (strncmp(buf, "POST", 4) == 0);
    int is_get  = (strncmp(buf, "GET",  3) == 0);

    if (is_get) {
        send_page(fd);
    } else if (is_post) {
        // POST body follows after \r\n\r\n
        char *body = strstr(buf, "\r\n\r\n");
        if (body) body += 4;
        else      body  = "";

        // if body is short, try reading more
        if (strlen(body) < 4) {
            int extra = recv(fd, buf + total, RECV_BUF - 1 - total, 0);
            if (extra > 0) {
                total += extra;
                buf[total] = '\0';
                body = strstr(buf, "\r\n\r\n");
                if (body) body += 4; else body = "";
            }
        }
        handle_post(fd, body);
    }
}

// ── Main ──────────────────────────────────────────────────────────────
int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    socInit((u32*)memalign(0x1000, 0x100000), 0x100000);

    printf("\033[1;32m3DS Chat Server\033[0m\n");
    printf("Port: \033[1;33m%d\033[0m\n\n", PORT);

    // get our IP
    struct in_addr myip;
    myip.s_addr = gethostid();
    printf("IP: \033[1;36m%s\033[0m\n", inet_ntoa(myip));
    printf("Browser: http://%s:%d\n\n", inet_ntoa(myip), PORT);
    printf("START startet Home-Button\n");
    printf("--------------------------------\n");

    // pre-load a welcome message
    push_message("3DS", "Server gestartet! Schreib etwas ^^");

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { printf("socket() failed\n"); goto cleanup; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("bind() failed: %d\n", errno);
        close(srv); goto cleanup;
    }
    listen(srv, BACKLOG);

    // non-blocking so we can poll HOME button
    fcntl(srv, F_SETFL, O_NONBLOCK);

    printf("Server läuft! Warte auf Verbindungen...\n");

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;

        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int cfd = accept(srv, (struct sockaddr*)&cli, &cli_len);
        if (cfd >= 0) {
            printf("Verbindung von %s\n", inet_ntoa(cli.sin_addr));
            handle_client(cfd);
            close(cfd);
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    close(srv);
cleanup:
    socExit();
    gfxExit();
    return 0;
}
