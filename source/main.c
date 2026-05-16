#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define PORT        7496
#define MAX_MSGS    32
#define MSG_LEN     200
#define NAME_LEN    32
#define BACKLOG     4
#define RECV_BUF    4096
#define RESP_BUF    32768

typedef struct {
    char name[NAME_LEN];
    char text[MSG_LEN];
    char time_str[16];
} ChatMsg;

static ChatMsg g_msgs[MAX_MSGS];
static int     g_msg_count = 0;

static void push_message(const char *name, const char *text)
{
    if (g_msg_count == MAX_MSGS) {
        memmove(&g_msgs[0], &g_msgs[1], sizeof(ChatMsg) * (MAX_MSGS - 1));
        g_msg_count = MAX_MSGS - 1;
    }
    ChatMsg *m = &g_msgs[g_msg_count++];
    snprintf(m->name, NAME_LEN, "%s", name);
    snprintf(m->text, MSG_LEN,  "%s", text);

    u64 ms  = osGetTime();
    u32 sec = (u32)(ms / 1000ULL);
    unsigned int hh = (unsigned int)((sec / 3600U) % 24U);
    unsigned int mm = (unsigned int)((sec /   60U) % 60U);
    unsigned int ss = (unsigned int)( sec           % 60U);
    snprintf(m->time_str, sizeof(m->time_str), "%02u:%02u:%02u", hh, mm, ss);
}

static void url_decode(const char *src, char *dst, size_t dstlen)
{
    size_t i = 0;
    while (*src && i < dstlen - 1) {
        if (*src == '+') {
            dst[i++] = ' '; src++;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], '\0' };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static void html_escape(const char *src, char *dst, size_t dstlen)
{
    size_t i = 0;
    while (*src && i < dstlen - 6) {
        switch (*src) {
            case '<': memcpy(dst+i, "&lt;",  4); i += 4; break;
            case '>': memcpy(dst+i, "&gt;",  4); i += 4; break;
            case '&': memcpy(dst+i, "&amp;", 5); i += 5; break;
            case '"': memcpy(dst+i, "&#34;", 5); i += 5; break;
            default:  dst[i++] = *src; break;
        }
        src++;
    }
    dst[i] = '\0';
}

/* Build entire HTML body into buf, return length */
static int build_html(char *buf, int bufsz)
{
    int n = 0;

    n += snprintf(buf+n, bufsz-n,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>3DS Chat</title>"
        "<style>"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{font-family:'Segoe UI',sans-serif;background:#1a1a2e;color:#eee;"
             "min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:16px}"
        "h1{color:#e94560;margin-bottom:16px;font-size:1.6rem;letter-spacing:2px;text-transform:uppercase}"
        "#chat{width:100%%;max-width:600px;background:#16213e;border-radius:12px;padding:16px;"
              "margin-bottom:12px;height:55vh;overflow-y:auto;display:flex;flex-direction:column;gap:8px}"
        ".msg{background:#0f3460;border-radius:8px;padding:8px 12px}"
        ".msg .meta{font-size:.75rem;color:#a0a0c0;margin-bottom:2px}"
        ".msg .meta .name{color:#e94560;font-weight:700}"
        ".msg .body{font-size:.95rem;word-break:break-word}"
        "form{width:100%%;max-width:600px;display:flex;flex-direction:column;gap:8px}"
        "input,button{border:none;border-radius:8px;padding:10px 14px;font-size:1rem}"
        "input{background:#0f3460;color:#eee;flex:1}"
        "input::placeholder{color:#556}"
        "button{background:#e94560;color:#fff;cursor:pointer;font-weight:700;letter-spacing:1px}"
        "#status{font-size:.8rem;color:#667;margin-top:4px;text-align:center}"
        "</style></head><body>"
        "<h1>&#127918; 3DS Chat</h1>"
        "<div id='chat'>");

    if (g_msg_count == 0) {
        n += snprintf(buf+n, bufsz-n,
            "<p style='color:#556;margin:auto'>Noch keine Nachrichten. Sag Hallo! &#128075;</p>");
    }

    for (int i = 0; i < g_msg_count; i++) {
        char sname[NAME_LEN*6], stext[MSG_LEN*6];
        html_escape(g_msgs[i].name, sname, sizeof(sname));
        html_escape(g_msgs[i].text, stext, sizeof(stext));
        n += snprintf(buf+n, bufsz-n,
            "<div class='msg'><div class='meta'>"
            "<span class='name'>%s</span> &nbsp;%s</div>"
            "<div class='body'>%s</div></div>",
            sname, g_msgs[i].time_str, stext);
    }

    n += snprintf(buf+n, bufsz-n,
        "</div>"
        "<form method='POST' action='/'>"
        "<input name='name' placeholder='Dein Name' maxlength='31' required>"
        "<input name='msg'  placeholder='Nachricht...' maxlength='199' required>"
        "<button type='submit'>Senden &#128172;</button>"
        "</form>"
        "<p id='status'>3DS Chat &nbsp;&#10003;&nbsp; Port 7496</p>"
        "<script>"
        "setTimeout(function(){location.reload();},5000);"
        "window.onload=function(){var c=document.getElementById('chat');c.scrollTop=c.scrollHeight;};"
        "</script>"
        "</body></html>");

    return n;
}

static void send_page(int fd)
{
    char *body = (char*)malloc(RESP_BUF);
    if (!body) return;

    int bodylen = build_html(body, RESP_BUF);

    /* Send headers with correct Content-Length so Safari doesn't hang */
    char headers[256];
    int hlen = snprintf(headers, sizeof(headers),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        bodylen);

    send(fd, headers, hlen,    0);
    send(fd, body,    bodylen, 0);
    free(body);
    /* wait for client to read before closing */
    shutdown(fd, SHUT_WR);
    svcSleepThread(100000000LL); /* 100ms */
}

static void send_redirect(int fd)
{
    const char *r =
        "HTTP/1.1 302 Found\r\n"
        "Location: /\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";
    send(fd, r, strlen(r), 0);
    shutdown(fd, SHUT_WR);
    svcSleepThread(50000000LL); /* 50ms */
}

static void handle_post(int fd, const char *body)
{
    char raw_name[NAME_LEN*3]; raw_name[0] = '\0';
    char raw_msg [MSG_LEN*3];  raw_msg[0]  = '\0';
    char dec_name[NAME_LEN];   dec_name[0] = '\0';
    char dec_msg [MSG_LEN];    dec_msg[0]  = '\0';

    const char *p;
    if ((p = strstr(body, "name=")) != NULL) {
        p += 5;
        const char *end = strchr(p, '&');
        size_t len = end ? (size_t)(end-p) : strlen(p);
        if (len >= sizeof(raw_name)) len = sizeof(raw_name)-1;
        memcpy(raw_name, p, len); raw_name[len] = '\0';
    }
    if ((p = strstr(body, "msg=")) != NULL) {
        p += 4;
        const char *end = strchr(p, '&');
        size_t len = end ? (size_t)(end-p) : strlen(p);
        if (len >= sizeof(raw_msg)) len = sizeof(raw_msg)-1;
        memcpy(raw_msg, p, len); raw_msg[len] = '\0';
    }

    url_decode(raw_name, dec_name, sizeof(dec_name));
    url_decode(raw_msg,  dec_msg,  sizeof(dec_msg));

    if (dec_name[0] && dec_msg[0])
        push_message(dec_name, dec_msg);

    send_redirect(fd);
}

static void handle_client(int fd)
{
    char buf[RECV_BUF];
    int  total = 0, n;

    while (total < RECV_BUF - 1) {
        n = (int)recv(fd, buf+total, (size_t)(RECV_BUF-1-total), 0);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    if (total == 0) return;
    buf[total] = '\0';

    int is_post = (strncmp(buf, "POST", 4) == 0);

    if (!is_post) {
        send_page(fd);
        return;
    }

    char *body = strstr(buf, "\r\n\r\n");
    if (body) body += 4; else body = "";

    /* read more if body not yet arrived */
    if (strlen(body) < 4) {
        n = (int)recv(fd, buf+total, (size_t)(RECV_BUF-1-total), 0);
        if (n > 0) {
            total += n; buf[total] = '\0';
            body = strstr(buf, "\r\n\r\n");
            if (body) body += 4; else body = "";
        }
    }
    handle_post(fd, body);
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    void *sock_heap = memalign(0x1000, 0x100000);
    socInit((u32*)sock_heap, 0x100000);

    printf("\033[1;32m3DS Chat Server\033[0m\n");
    printf("Port: \033[1;33m%d\033[0m\n\n", PORT);

    struct in_addr myip;
    myip.s_addr = gethostid();
    printf("IP:  \033[1;36m%s\033[0m\n",   inet_ntoa(myip));
    printf("URL: http://%s:%d\n\n",         inet_ntoa(myip), PORT);
    printf("START = Beenden\n");
    printf("----------------------------\n");

    push_message("3DS", "Server gestartet! Schreib etwas ^^");

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { printf("socket() Fehler\n"); goto cleanup; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("bind() Fehler: %d\n", errno);
        close(srv); goto cleanup;
    }
    listen(srv, BACKLOG);
    fcntl(srv, F_SETFL, O_NONBLOCK);

    printf("Warte auf Verbindungen...\n");

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;

        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int cfd = accept(srv, (struct sockaddr*)&cli, &cli_len);
        if (cfd >= 0) {
            printf("Verbindung: %s\n", inet_ntoa(cli.sin_addr));
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
    free(sock_heap);
    gfxExit();
    return 0;
}
