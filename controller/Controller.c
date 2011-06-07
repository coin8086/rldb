/******************************************************************************
* Copyright (C) 2011 Robert Ray<louirobert@gmail.com>.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <assert.h>
#include <string.h>
#include "Socket.h"
#include "SocketBuf.h"
#include "Dump.h"

typedef enum
{
    CMD_INVALID = -1,
    CMD_STEP = 0,
    CMD_OVER,
    CMD_RUN,
    CMD_LISTL,
    CMD_LISTU,
    CMD_LISTG,
    CMD_PRINTSTACK,
    CMD_WATCH,
    CMD_EXEC,
    CMD_SETB,
    CMD_DELB,
    CMD_LISTB,
    CMD_MEMORY,
    CMD_HELP
} CmdType;

/*
** THIS ARRAY MUST CORRESPOND EXACTLY TO THE ABOVE ENUM TYPE!!!
*/
const char * g_cmds[] =
{
    "s",
    "o",
    "r",
    "ll",
    "lu",
    "lg",
    "ps",
    "w",
    "e",
    "sb",
    "db",
    "lb",
    "m",
    "h",
    0
};

static void mainloop(SOCKET s);
static int extractArgs(char * buf, char * argv[]);
static CmdType validateArgs(char * argv[], int argc);
static int sendCmd(SOCKET s, CmdType t, char * argv[], int argc);
static int waitForBreakOrQuit(SocketBuf * sb, const char ** file, const char ** lineno);
static int waitForResponseFirstLine(SocketBuf * sb);
static int showError(SocketBuf * sb);
static int listL(SocketBuf * sb);
static int printStack(SocketBuf * sb);
static int watch(SocketBuf * sb);
static int listB(SocketBuf * sb);
static int watchM(SocketBuf * sb, char * argv[], int argc);
static void showHelp();

#define CMD_LINE 1024
#define MAX_ARGS 8

#define SHOW_USAGE_AND_RETURN(s) \
    do {\
        printf("Usage:\n%s [-aXXX.XXX.XXX.XXX] [-pXXXX]\n", (s));\
        return -1;\
    } while (0);

#ifdef OS_WIN
static int initSocket()
{
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData) ? -1 : 0;
}

void uninitSocket()
{
    WSACleanup();
}

#else
#define initSocket() 0
#define uninitSocket()
#endif

int main(int argc, char * argv[])
{
    SOCKET s;
    SOCKET a;
    struct sockaddr_in addr;
    char addrStr[64] = {0};
    unsigned short port = 0;

    if (argc > 1) {
        int i = 1;
        for (; i < argc; i++) {
            if (argv[i][0] == '-') {
                if (argv[i][1] == 'a') {
                    strncpy(addrStr, argv[i] + 2, 63);
                    addrStr[63] = 0;
                }
                else if (argv[i][1] == 'p') {
                    port = (unsigned short)atoi(argv[i] + 2);
                }
                else {
                    SHOW_USAGE_AND_RETURN(argv[0]);
                }
            }
            else {
                SHOW_USAGE_AND_RETURN(argv[0]);
            }
        }
    }
    if (addrStr[0] == 0)
        strcpy(addrStr, "127.0.0.1");
    if (port == 0)
        port = 2679;

    if (initSocket()) {
        printf("initSocket failed!\n");
        return -1;
    }

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        printf("Socket error!\n");
        uninitSocket();
        return -1;
    }

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(addrStr);
    addr.sin_port = htons(port);

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR
        || listen(s, 1) == SOCKET_ERROR) {
        printf("Socket error!\nIP %s Port %d\n", addrStr, (int)port);
        closesocket(s);
        uninitSocket();
        return -1;
    }

    printf("RLdb 2.0.0 Copyright (C) 2011 Robert Ray<louirobert@gmail.com>\n");
    printf("Waiting at %s:%d for remote debuggee...\n", addrStr, (int)port);
    do {
      a = accept(s, NULL, NULL);
    } while (a == SOCKET_ERROR);

    printf("Connected!\n");
    closesocket(s);
    mainloop(a);
    closesocket(a);
    uninitSocket();
    return 0;
}

void mainloop(SOCKET s)
{
    SocketBuf sb;
    SB_Init(&sb, s);

    while (1) {
        int rc;
        const char * file;
        const char * lineno;

        //Wait for a BREAK or QUIT message...
        rc = waitForBreakOrQuit(&sb, &file, &lineno);
        if (rc < 0) {
            printf("Socket or protocol error!\n");
            break;
        }
        if (!rc) {
            printf("Remote script is over!\n");
            break;
        }
        printf("Break At \"%s:%s\"\n", file, lineno);

        while (1) {
            char buf[CMD_LINE];
            char * argv[MAX_ARGS];
            int argc;
            CmdType t;

            //Prompt user...
            printf("?>");
            fgets(buf, CMD_LINE, stdin);
            if ((argc = extractArgs(buf, argv)) > 0)
                t = validateArgs(argv, argc);
            if (argc < 1 || t == CMD_INVALID) {
                printf("Invalid command! Type 'h' for help.\n");
                continue;
            }

            if (t == CMD_HELP) {
                showHelp();
                continue;
            }

            //Send command...
            if (sendCmd(s, t, argv, argc) < 0) {
                printf("Socket error!\n");
                return;
            }

            if (t == CMD_STEP || t == CMD_OVER || t == CMD_RUN)
                break;

            //Wait for result message...
            rc = waitForResponseFirstLine(&sb);
            if (rc < 0) {
                printf("Socket or protocol error!\n");
                return;
            }

            //Show result...
            if (rc == 0) {
                if (showError(&sb) < 0) {
                    printf("Socket or protocol error!\n");
                    return;
                }
                continue;
            }

            switch (t) {
                case CMD_LISTL:
                case CMD_LISTU:
                case CMD_LISTG:
                {
                    rc = listL(&sb);
                    break;
                }

                case CMD_PRINTSTACK: {
                    rc = printStack(&sb);
                    break;
                }

                case CMD_WATCH: {
                    rc = watch(&sb);
                    break;
                }
//
//                case CMD_EXEC: {
//                    rc = exec(&sb);
//                    break;
//                }
//
                case CMD_SETB:
                case CMD_DELB:
                {
                    //No content in this case, so read out the rest and drop it.
                    rc = SB_Read(&sb, SB_R_LEFT);
                    assert(sb.end);
                    break;
                }

                case CMD_LISTB: {
                    rc = listB(&sb);
                    break;
                }

                case CMD_MEMORY: {
                    rc = watchM(&sb, argv, argc);
                    break;
                }

                default: {
                    assert(0 && "Impossibility!");
                }
            }

            if (rc < 0) {
                printf("Socket or protocol error!\n");
                return;
            }
        }
    }
}

int extractArgs(char * buf, char * argv[])
{
    char * p = buf;
    int argc = 0;

    while (*p && argc < MAX_ARGS) {
        while (isspace(*p))
            ++p;
        if (!*p)
            break;

        argv[argc++] = p;
        if (*p != '"') {
            while (!isspace(*p) && *p)
                ++p;
            if (isspace(*p))
                *p++ = 0;
        }
        else {
            while (*++p != '"' && *p);
            if (!*p)
                return -1;  //End '"' not found!
            *++p = 0;
            ++p;
        }
    }
    return argc;
}

static int allDigits(char * str)
{
    while (isdigit(*str))
        ++str;
    return *str ? 0 : 1;
}

CmdType validateArgs(char * argv[], int argc)
{
    CmdType t = CMD_INVALID;

    if (argc > 0) {
        char * p = argv[0];

        if (!strcmp(p, "s")) {
            if (argc == 1)
                t = CMD_STEP;
        }
        else if (!strcmp(p, "o")) {
            if (argc == 1)
                t = CMD_OVER;
        }
        else if (!strcmp(p, "r")) {
            if (argc == 1)
                t = CMD_RUN;
        }
        else if (!strcmp(p, "ll")) {
            if (argc == 2 && allDigits(argv[1]))
                t = CMD_LISTL;
        }
        else if (!strcmp(p, "lu")) {
            if (argc == 2 && allDigits(argv[1]))
                t = CMD_LISTU;
        }
        else if (!strcmp(p, "lg")) {
            if (argc == 2 && allDigits(argv[1]))
                t = CMD_LISTG;
        }
        else if (!strcmp(p, "w")) {
            if (argc > 1) {
                if (allDigits(argv[1]) && argc > 3 && argv[2][1] == 0
                    && (argv[2][0] == 'l' || argv[2][0] == 'u' || argv[2][0] == 'g')) {
                    if (argc == 5) {
                        if (argv[4][0] == 'r' && argv[4][1] == 0)
                            t = CMD_WATCH;
                    }
                    else if (argc == 4)
                        t = CMD_WATCH;
                }
                else if (argv[1][0] == '|') {
                    if (argc == 3) {
                        if (argv[2][0] == 'r' && argv[2][1] == 0)
                            t = CMD_WATCH;
                    }
                    else if (argc == 2)
                        t = CMD_WATCH;
                }
            }
        }
        else if (!strcmp(p, "ps")) {
            if (argc == 1)
                t = CMD_PRINTSTACK;
        }
        else if (!strcmp(p, "sb")) {
            if (argc == 3 && allDigits(argv[2]))
                t = CMD_SETB;
        }
        else if (!strcmp(p, "db")) {
            if (argc == 3 && allDigits(argv[2]))
                t = CMD_DELB;
        }
        else if (!strcmp(p, "lb")) {
            if (argc == 1)
                t = CMD_LISTB;
        }
        else if (!strcmp(p, "m")) {
            if (argc == 3) {
                char * end;
                char * end2;
                strtoul(argv[1], &end, 0);
                strtoul(argv[2], &end2, 0);
                if (!*end && !*end2)
                    t = CMD_MEMORY;
            }
        }
        else if (!strcmp(p, "h")) {
            t = CMD_HELP;
        }
    }
    return t;
}

static int SendData(SOCKET s, const char * buf, int len)
{
    while (len > 0) {
        int sent = send(s, buf, len, 0);
        if (sent == SOCKET_ERROR)
            return -1;
        len -= sent;
        buf += sent;
    }
    return 0;
}

int sendCmd(SOCKET s, CmdType t, char * argv[], int argc)
{
    //The buffer size is exactly the same with the one used by fgets in main().
    //So it's convenient to use strcat without worrying about buffer overflow!
    char cmdline[CMD_LINE];
    const char * cmd = g_cmds[t];
    int i;

    strcpy(cmdline, cmd);
    for (i = 1; i < argc; i++) {
        strcat(cmdline, " ");
        strcat(cmdline, argv[i]);
    }

    return SendData(s, cmdline, strlen(cmdline) + 1);
}

int waitForBreakOrQuit(SocketBuf * sb, const char ** file, const char ** lineno)
{
    int rc;
    char * p = sb->lbuf;
    rc = SB_Read(sb, SB_R_LEFT);
    if (rc < 0 || !sb->end)
        return -1;

    if (!strncmp(p, "BR\n", 3)) {
        p += 3;
        *file = p;
        p = strchr(p, '\n');
        if (!p)
            return -1;
        *p++ = 0;
        *lineno = p;
        p = strchr(p, '\n');
        if (!p)
            return -1;
        *p = 0;
        return 1;
    }
    else if (!strcmp(p, "QT\n\n")) {
        return 0;
    }
    return -1;
}

int waitForResponseFirstLine(SocketBuf * sb)
{
    char * p = sb->lbuf;
    if (SB_Read(sb, 3) < 0)
        return -1;
    if (!strncmp(p, "OK\n", 3)) {
        return 1;
    }
    else if (!strncmp(p, "ER\n", 3)) {
        return 0;
    }
    return -1;
}

int showError(SocketBuf * sb)
{
    char * p = sb->lbuf;
    if (SB_Read(sb, SB_R_LEFT) < 0 || !sb->end)
        return -1;

    fprintf(stdout, "%s", p);
    return 0;
}

typedef enum
{
    LV_NAME = 1,
    LV_VALUE
} State_lv;

static int lv(State_lv * st, const char * str, int length);

int listL(SocketBuf * sb)
{
    State_lv st = LV_NAME;
    return SB_ReadAndParse(sb, "\n", (UserParser)lv, &st);
}

static const char * typestr(char t)
{
    const char * tstr;
    switch (t) {
        case 's':
            tstr = "STR";
            break;
        case 'n':
            tstr = "NUM";
            break;
        case 't':
            tstr = "TAB";
            break;
        case 'f':
            tstr = "FNC";
            break;
        case 'u':
            tstr = "URD";
            break;
        case 'U':
            tstr = "LUD";
            break;
        case 'b':
            tstr = "BLN";
            break;
        case 'l':
            tstr = "NIL";
            break;
        case 'd':
            tstr = "THD";
            break;
        default:
            tstr = "";
    }
    return tstr;
}

static void output(const char * str, int length)
{
    int i;
    for (i = 0; i < length; ++i)
        fputc(str[i], stdout);
}

#define dec(ch) \
    ((ch) >= '0' && (ch) <= '9' ? (ch) - '0' : (ch) - 'a' + 10)

#define decode(s, ch) \
    do {\
        ch = (dec((s)[0]) << 4) | dec((s)[1]);\
    } while (0);

static void outputEncStr(const char * str, int length)
{
    const char * end = str + length;
    char ch;
    for (; str < end; str += 2) {
        decode(str, ch);
        fputc(ch, stdout);
    }
}

static int outputStr(const char * str, int length)
{
    const char * end = str + length;
    const char * p = strchr(str, ':');
    int len;
    if (!p || p > end)
        return -1;
    output(str, p - str);
    str = p + 1;
    fputs(" Length:", stdout);
    p = strchr(str, ':');
    if (!p || p > end)
        return -1;
    output(str, p - str);
    fputs(" Truncated-to:", stdout);
    str = p + 1;
    p = strchr(str, ':');
    if (!p || p > end)
        return -1;
    output(str, p - str);
    fputs(" Content:", stdout);
    len = strtol(str, NULL, 10);
    str = p + 1;
    if (end - str != len * 2)
        return -1;
    outputEncStr(str, end - str);
    return 0;
}

static int printVar(const char * str, int length)
{
    const char * tstr = typestr(str[0]);
    if (*tstr == 0)
        return -1;

    fprintf(stdout, "Type:%s \tValue:", tstr);
    switch(str[0]) {
        case 's': {
            if (outputStr(str + 1, length - 1) < 0)
                return -1;
            break;
        }

        case 'n':
        case 'b':
        case 't':
        case 'f':
        case 'u':
        case 'U':
        case 'd':
        {
            output(str + 1, length - 1);
            break;
        }

        case 'l':
            fprintf(stdout, "nil");
            break;
    }
    return 0;
}

int lv(State_lv * st, const char * str, int length)
{
    if (*st == LV_NAME) {
        fprintf(stdout, "Name:");
        output(str, length);
        fputc(' ', stdout);
        fputc('\t', stdout);
        *st = LV_VALUE;
    }
    else {
        if (printVar(str, length) < 0)
            return -3;

        fputc('\n', stdout);
        *st = LV_NAME;
    }
    return 0;
}

typedef enum
{
    PS_FILE,
    PS_LINE,
    PS_NAME,
    PS_WHAT
} State_ps;

static int ps(State_ps * st, const char * word, int length);

int printStack(SocketBuf * sb)
{
    State_ps st = PS_FILE;
    return SB_ReadAndParse(sb, "\n", (UserParser)ps, &st);
}

int ps(State_ps * st, const char * word, int length)
{
    switch (*st) {
        case PS_FILE: {
            fputs("At \"", stdout);
            output(word, length);
            fputc(':', stdout);
            *st = PS_LINE;
            break;
        }
        case PS_LINE: {
            output(word, length);
            fputs("\" \t", stdout);
            *st = PS_NAME;
            break;
        }
        case PS_NAME: {
            output(word, length);
            fputs(" \t", stdout);
            *st = PS_WHAT;
            break;
        }
        case PS_WHAT: {
            output(word, length);
            fputc('\n', stdout);
            *st = PS_FILE;
            break;
        }
    }
    return 0;
}

typedef enum
{
    W_VAR = 1,  //for all
    W_META,     //for all
    W_KEY,      //for table
    W_VAL,      //for table
    W_SIZE,     //for full userdata
    W_WHAT,     //for function
    W_SRC,      //for function
    W_FIRSTLINE,//for function
    W_LASTLINE, //for function
    W_STATUS    //for thread
} State_w;

typedef struct
{
    State_w st;
    State_w st2;    //What state after W_META
} Arg_w;

static int w(Arg_w * args, const char * word, int length);

int watch(SocketBuf * sb)
{
    Arg_w args = { W_VAR, 0 };
    return SB_ReadAndParse(sb, "\n", (UserParser)w, &args);
}

int w(Arg_w * args, const char * word, int length)
{
    switch (args->st) {
        case W_KEY: {
            fputs("--------------------------------------------------\n", stdout);
            if (printVar(word, length) < 0)
                return -3;
            fputc('\n', stdout);
            args->st = W_VAL;
            break;
        }

        case W_VAL: {
            if (printVar(word, length) < 0)
                return -3;
            fputc('\n', stdout);
            args->st = W_KEY;
            break;
        }

        case W_VAR: {
            if (printVar(word, length) < 0)
                return -3;
            fputc('\n', stdout);
            args->st = W_META;
            switch (word[0]) {
                case 't': {
                    args->st2 = W_KEY;
                    break;
                }
                case 'u': {
                    args->st2 = W_SIZE;
                    break;
                }
                case 'f': {
                    args->st2 = W_WHAT;
                    break;
                }
                case 'd': {
                    args->st2 = W_STATUS;
                    break;
                }
                default: {
                    args->st2 = 0;
                }
            }
            break;
        }

        case W_META: {
            if (length != 1)
                return -3;

            if (word[0] == '1') {
                fputs("HasMetatable:Yes\n", stdout);
            }
            else {
                fputs("HasMetatable:No\n", stdout);
            }
            args->st = args->st2;
            break;
        }

        case W_SIZE: {
            fputs("Size:", stdout);
            output(word, length);
            fputc('\n', stdout);
            args->st = 0;
            break;
        }

        case W_WHAT: {
            fputs("What:", stdout);
            output(word, length);
            args->st = W_SRC;
            break;
        }

        case W_SRC: {
            fputs(" \tFile:", stdout);
            output(word, length);
            args->st = W_FIRSTLINE;
            break;
        }

        case W_FIRSTLINE: {
            fputs(" \tLineDefined:", stdout);
            output(word, length);
            args->st = W_LASTLINE;
            break;
        }

        case W_LASTLINE: {
            fputs(" \tLastLine:", stdout);
            output(word, length);
            fputc('\n', stdout);
            args->st = 0;
            break;
        }

        case W_STATUS: {
            fputs("Status:", stdout);
            output(word, length);
            fputc('\n', stdout);
            args->st = 0;
            break;
        }

        default: {
            return -3;
        }
    }
    return 0;
}

typedef enum
{
    LB_FILE,
    LB_LINE
} State_lb;

static int lb(State_lb * st, const char * word, int length);

int listB(SocketBuf * sb)
{
    State_lb st = LB_FILE;
    return SB_ReadAndParse(sb, "\n", (UserParser)lb, &st);
}

int lb(State_lb * st, const char * word, int length)
{
    if (*st == LB_FILE) {
        fputc('"', stdout);
        output(word, length);
        fputc(':', stdout);
        *st = LB_LINE;
    }
    else {
        output(word, length);
        fputs("\"\n", stdout);
        *st = LB_FILE;
    }
    return 0;
}

#define PROVIDER_BUF_SIZE 1024

typedef struct
{
    SOCKET s;
    unsigned int len;
    char buf[PROVIDER_BUF_SIZE];
} Arg_wm;

static int provide(Arg_wm * args, const char ** buf, unsigned int * size);

int watchM(SocketBuf * sb, char * argv[], int argc)
{
    unsigned int addr = strtoul(argv[1], NULL, 0);
    unsigned int len;
    char * end;
    Arg_wm args;

    if (SB_Read(sb, 9) < 0 || sb->end)
        return -1;

    len = strtoul(sb->lbuf, &end, 16);
    if (len == 0 || *end != '\n' || sb->lbuf + 8 != end)
        return -2;

    args.s = sb->s;
    args.len = len;
    return Dump(addr, (DataProvider)provide, &args, stdout, NULL, NULL);
}

int provide(Arg_wm * args, const char ** buf, unsigned int * size)
{
    if (args->len > 0) {
        int l = recv(args->s, args->buf,
            args->len < PROVIDER_BUF_SIZE ? args->len : PROVIDER_BUF_SIZE, 0);
        if (l == SOCKET_ERROR || !l)
            return -1;
        args->len -= l;
        *size = l;
        *buf = args->buf;
        return 1;
    }
    return 0;
}

#define HELP_CONTENT \
"RLdb 2.0.0 Copyright (C) 2011 Robert Ray<louirobert@gmail.com>\n"\
"All rights reserved\n"\
"Debug commands are listed below in alphabetical order. Please refer to online document for details. (If you don't know where to get one, write to me.)\n"\
"\n"\
"db \n"\
"Brief:  Delete a breakpoint.\n"\
"Format: db <file-path> <line-no>\n"\
"\n"\
"lb\n"\
"Brief:  List breakpoints.\n"\
"Format: lb\n"\
"\n"\
"lg\n"\
"Brief:  List globals.\n"\
"Format: lg <stack-level>\n"\
"\n"\
"ll\n"\
"Brief:  List locals.\n"\
"Format: ll <stack-level>\n"\
"\n"\
"lu\n"\
"Brief:  List upvalues.\n"\
"Format: lu <stack-level>\n"\
"\n"\
"m\n"\
"Brief:  Watch memory.\n"\
"Format: m <start-address> <length>\n"\
"\n"\
"o\n"\
"Brief:  Step over.\n"\
"Format: o\n"\
"\n"\
"ps\n"\
"Brief:  Print calling stack.\n"\
"Format: ps\n"\
"\n"\
"r\n"\
"Brief:  Run program until a breakpoint.\n"\
"Format: r\n"\
"\n"\
"s\n"\
"Brief:  Step into.\n"\
"Format: s\n"\
"\n"\
"sb\n"\
"Brief:  Set a breakpoint.\n"\
"Format: sb <file-path> <line-no>\n"\
"\n"\
"w\n"\
"Brief:  Watch a variable.\n"\
"Format1:w <stack-level> <l|u|g> <variable-name>[properties] [r]\n"\
"Format2:w <properties> [r]\n"\

void showHelp()
{
    fputs(HELP_CONTENT, stdout);
}
