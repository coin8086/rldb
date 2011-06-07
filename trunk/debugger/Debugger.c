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

#include <lua.h>
#include <lauxlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <assert.h>

#ifdef OS_WIN
#include <io.h>     //access
#endif

#ifdef OS_LINUX
#include <unistd.h> //access, getcwd
#define _MAX_PATH PATH_MAX
#define _access access

static char * _fullpath(char * absPath, const char * relPath, size_t maxLen)
{
    char * ret = 0;
    assert(absPath && relPath && maxLen > 1);
    if (*relPath == '/') {
        size_t len = strlen(relPath);
        if (len <= maxLen - 1) {
            strcpy(absPath, relPath);
            ret = absPath;
        }
    }
    else {
        ret = getcwd(absPath, maxLen);
        if (ret) {
            size_t len = strlen(absPath);
            size_t rlen = strlen(relPath);
            if (absPath[len - 1] != '/' && len < maxLen && rlen) {
                absPath[len++] = '/';
            }
            if (len + rlen < maxLen) {
                strcpy(absPath + len, relPath);
                ret = absPath;
            }
        }
    }
    return ret;
}

#endif

#include "Protocol.h"

static const luaL_Reg entries[] = { {0, 0} };

static void hook(lua_State *L, lua_Debug *ar);

typedef enum
{
    STEP = 1,
    OVER,
    FINISH,
    RUN
} CMD;

typedef struct
{
    SOCKET s;
    CMD cmd;    //last cmd from remote controller
    int level;  //relative stack level pertaining to the last "step over" command
} DebuggerInfo;

static int onGC(lua_State * L)
{
    DebuggerInfo * info = (DebuggerInfo *)lua_touserdata(L, -1);
    if (info->s != INVALID_SOCKET) {
        SendQuit(info->s);
        closesocket(info->s);
    }
    return 0;
}

#ifdef OS_WIN
__declspec(dllexport)
#endif
int luaopen_RLdb(lua_State * L)
{
    SOCKET s;
    DebuggerInfo * info;
    unsigned short port;
    const char * addr;
    char env[64];
    char * p;

    //read config and set up connection with a remote controller
    p = getenv("REMOTE_LDB");
    if (p) {    //REMOTE_LDB's value is sth. like "192.168.0.1:6688".
        strncpy(env, p, 63);
        env[63] = 0;
        p = strchr(env, ':');
        if (p && p != env) {
            *p++ = 0;
            if (*p)
                port = (unsigned short)atoi(p);
            else
                port = 2679;
            addr = env;
        }
        else if (p) {   //p == env
            port = (unsigned short)atoi(++p);
            addr = "127.0.0.1";
        }
        else {
            port = 2679;
            addr = env;
        }
    }
    else {
        port = 2679;
        addr = "127.0.0.1";
    }

    if ((s = Connect(addr, port)) == INVALID_SOCKET) {
        fprintf(stderr, "Socket or protocol error!\nFailed connecting remote controller at %s:%d.\n",
            addr, (int)port);
        return 0;
    }

    //store debugger info into a table
    lua_pushliteral(L, "debugger");
    lua_newtable(L);

    lua_pushliteral(L, "breakpoints");
    lua_newtable(L);
    lua_rawset(L, -3);

    lua_pushliteral(L, "info");
    info = (DebuggerInfo *)lua_newuserdata(L, sizeof(DebuggerInfo));
    info->s = s;
    info->cmd = STEP;
    info->level = 0;
    lua_newtable(L);
    lua_pushliteral(L, "__gc");
    lua_pushcfunction(L, onGC);
    lua_rawset(L, -3);
    lua_setmetatable(L, -2);
    lua_rawset(L, -3);

    lua_rawset(L, LUA_REGISTRYINDEX);

    luaL_register(L, "robert.debugger", entries);
    lua_sethook(L, hook, LUA_MASKLINE | LUA_MASKCALL | LUA_MASKRET, 0);
    return 1;
}

static int prompt(lua_State *L, lua_Debug * ar, DebuggerInfo * info);
static int checkBreakPoint(lua_State *L, lua_Debug * ar, DebuggerInfo * info);

void hook(lua_State * L, lua_Debug * ar)
{
    int event = ar->event;
    int top = lua_gettop(L);
    DebuggerInfo * info;

    lua_pushliteral(L, "debugger");
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_pushliteral(L, "info");
    lua_rawget(L, -2);
    info = lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (event == LUA_HOOKLINE) {
        CMD cmd;
        int rc = 0;

        cmd = info->cmd;

        if (cmd == STEP) {
            info->level = 0;
            rc = prompt(L, ar, info);
        }
        else if (cmd == OVER) {
            if (!info->level)
                rc = prompt(L, ar, info);
            else
                rc = checkBreakPoint(L, ar, info);
        }
        else if (cmd == FINISH) {
            //prompt(L, ar);
        }
        else if (cmd == RUN) {
            rc = checkBreakPoint(L, ar, info);
        }

        //If a socket IO error or a protocol error happened, stop debugging
        //without informing the remote Controller.
        if (rc < 0) {
            lua_sethook(L, hook, 0, 0);
            closesocket(info->s);
            info->s = INVALID_SOCKET;
        }
    }
    else {
        assert(event != LUA_HOOKCOUNT);

        if (event == LUA_HOOKCALL) {
            info->level++;
        }
        else if (event == LUA_HOOKRET || event == LUA_HOOKTAILRET) {
            if (info->level)
                info->level--;
        }
    }
    lua_pop(L, 1);
    assert(top == lua_gettop(L));
}

/*
** Check if the current line contains a breakpoint. If yes, break and prompt
** for user, and reset statck level to 0 preparing for the next "OVER" command.
** On top of L is the "debugger" table stored in LUA_REGISTRYINDEX. L stays
** unchanged after call, but the "debugger" table may be changed.
** Return -1 when a socket io error happens, or 0 when succeed.
*/
int checkBreakPoint(lua_State * L, lua_Debug * ar, DebuggerInfo * info)
{
    int breakpoint;
    char path[_MAX_PATH + 1];

    lua_getinfo(L, "Sl", ar);
    if (_fullpath(path, ar->short_src, _MAX_PATH)) {
#ifdef OS_WIN
        _strlwr(path);
#endif

        lua_pushliteral(L, "breakpoints");
        lua_rawget(L, -2);
        lua_pushstring(L, path);
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            lua_rawgeti(L, -1, ar->currentline);
            breakpoint = lua_isnil(L, -1) ? 0 : 1;
            lua_pop(L, 1);
        }
        else
            breakpoint = 0;
        lua_pop(L, 2);

        if (breakpoint) {
            info->level = 0;
            return prompt(L, ar, info);
        }
    }
    return 0;
}

static int getCmd(SOCKET s, char * buf, int bufLen, char ** argv);
static int listLocals(lua_State * L, lua_Debug * ar, char * argv[], int argc, SOCKET s);
static int listUpVars(lua_State * L, lua_Debug * ar, char * argv[], int argc, SOCKET s);
static int listGlobals(lua_State * L, lua_Debug * ar, char * argv[], int argc, SOCKET s);
static int printStack(lua_State * L, SOCKET s);
static int watch(lua_State * L, lua_Debug * ar, char * argv[], int argc, SOCKET s);
static int exec(lua_State * L, lua_Debug * ar, char * argv[], int argc, SOCKET s);
static int setBreakPoint(lua_State * L, const char * src, char * argv[], int argc, int del, SOCKET s);
static int listBreakPoints(lua_State * L, SOCKET s);
static int watchMemory(char * argv[], int argc, SOCKET s);

/*
** On top of L is the "debugger" table stored in LUA_REGISTRYINDEX. L stays
** unchanged after call, but the "debugger" table may be changed.
** Return -1 when a socket io error happens, or 0 when succeed.
*/
int prompt(lua_State * L, lua_Debug * ar, DebuggerInfo * info)
{
    SOCKET s = info->s;
    CMD cmd;
    int top = lua_gettop(L);

    lua_getinfo(L, "nSl", ar);
    if (SendBreak(s, ar->short_src, ar->currentline) < 0) {
        fprintf(stderr, "Socket error!\n");
        return -1;
    }

    while (1) {
        char buf[PROT_MAX_CMD_LEN];
        char * argv[PROT_MAX_ARGS];
        int argc;
        char * pCmd;
        char ** pArgv;
        int rc;

        assert(lua_istable(L, -1));
        argc = getCmd(s, buf, PROT_MAX_CMD_LEN, argv);
        if (argc == -1) {
            fprintf(stderr, "Socket or protocol error!\n");
            return -1;
        }
        if (argc == 0) {
            if (SendErr(s, "Invalid command!") < 0) {
                fprintf(stderr, "Socket error!\n");
                return -1;
            }
            continue;
        }
        pCmd = argv[0];
        argc--;
        pArgv = argv + 1;

        if (!strcmp(pCmd, "s")) {
            cmd = STEP;
            break;
        }
        else if (!strcmp(pCmd, "o")) {
            cmd = OVER;
            break;
        }
        else if (!strcmp(pCmd, "f")) {
            cmd = FINISH;
            break;
        }
        else if (!strcmp(pCmd, "r")) {
            cmd = RUN;
            lua_pushliteral(L, "breakpoints");
            lua_rawget(L, -2);
            lua_pushnil(L);
            if (!lua_next(L, -2)) { //When no breakpoints exists, disable the hook.
                lua_sethook(L, hook, 0, 0);
                lua_pop(L, 1);
            }
            else
                lua_pop(L, 3);
            break;
        }
        else if (!strcmp(pCmd, "ll")) {
            rc = listLocals(L, ar, pArgv, argc, s);
        }
        else if (!strcmp(pCmd, "lu")) {
            rc = listUpVars(L, ar, pArgv, argc, s);
        }
        else if (!strcmp(pCmd, "lg")) {
            rc = listGlobals(L, ar, pArgv, argc, s);
        }
        else if (!strcmp(pCmd, "w")) {
            rc = watch(L, ar, pArgv, argc, s);
        }
        else if (!strcmp(pCmd, "ps")) {
            rc = printStack(L, s);
        }
        else if (!strcmp(pCmd, "sb")) {
            rc = setBreakPoint(L, ar->short_src, pArgv, argc, 0, s);
        }
        else if (!strcmp(pCmd, "db")) {
            rc = setBreakPoint(L, ar->short_src, pArgv, argc, 1, s);
        }
        else if (!strcmp(pCmd, "lb")) {
            rc = listBreakPoints(L, s);
        }
        else if (!strcmp(pCmd, "e")) {
            rc = exec(L, ar, pArgv, argc, s);
        }
        else if (!strcmp(pCmd, "m")) {
            rc = watchMemory(pArgv, argc, s);
        }
        else {
            rc = SendErr(s, "Invalid command!");
        }

        if (rc < 0) {
            fprintf(stderr, "Socket or protocol error!\n");
            return -1;
        }
    }

    info->cmd = cmd;
    assert(top == lua_gettop(L));
    return 0;
}

/*
** Get command from via socket s and put it in buf; then extract arguments in
** buf, which are separated by one single space. The result argument array is
** stored in argv, which can hold PROT_MAX_ARGS arguments at most. The actual number
** of arguments is returned. If a socket IO error happens, -1 is returned.
*/
int getCmd(SOCKET s, char * buf, int bufLen, char ** argv)
{
    int argc = 0;
    char * end;
    char * p = buf;
    int received = RecvCmd(s, buf, bufLen);
    if (received < 0) {
        return -1;
    }
    end = buf + received;
    *end = 0;

    while (p < end && argc < PROT_MAX_ARGS) {
        while (*p == ' ' && p < end)
            ++p;

        if (*p != '"') {
            argv[argc++] = p;
            while (*p != ' ' && p < end)
                ++p;
            if (p == end)
                break;
        }
        else {
            argv[argc++] = ++p;
            p = strchr(p, '"');
            if (!p)
                return -2;  //The end '"' is not found!
        }
        *p++ = 0;
    }
    return argc;
}

/*
** Print one line text containing a variable name and its value into sb.
** Variable value is on top of L. L stays unchanged after call.
*/
static void printVar(SocketBuf * sb, const char * name, lua_State * L)
{
    int type = lua_type(L, -1);

    if (name)
        SB_Print(sb, "%s\n", name);

    switch(type) {
        case LUA_TSTRING: {
            int len;
            const char * str = lua_tolstring(L, -1, (unsigned int *)&len);
            int truncLen = len > PROT_MAX_STR_LEN ? PROT_MAX_STR_LEN : len;
            SB_Print(sb, "s0x%08x:%d:%d:%Q\n", str, len, truncLen,
                str, truncLen); //%Q requires two arguments: buf and length
            break;
        }
        case LUA_TNUMBER: {
            /*
            ** LUA_NUMBER may be double or integer, So a runtime check may be required.
            ** Otherwise SB_Print may be crashed.
            */
            SB_Print(sb, "n%N\n", lua_tonumber(L, -1));
            break;
        }
        case LUA_TTABLE: {
            SB_Print(sb, "t0x%08x\n", lua_topointer(L, -1));
            break;
        }
        case LUA_TFUNCTION: {
            SB_Print(sb, "f0x%08x\n", lua_topointer(L, -1));
            break;
        }
        case LUA_TUSERDATA: {
            SB_Print(sb, "u0x%08x\n", lua_touserdata(L, -1));
            break;
        }
        case LUA_TLIGHTUSERDATA: {
            SB_Print(sb, "U0x%08x\n", lua_touserdata(L, -1));
            break;
        }
        case LUA_TBOOLEAN: {
            SB_Print(sb, "b%d\n", lua_toboolean(L, -1) ? 1 : 0);
            break;
        }
        case LUA_TTHREAD: {
            SB_Print(sb, "d0x%08x\n", lua_topointer(L, -1));
            break;
        }
        case LUA_TNIL: {
            SB_Print(sb, "l\n");
            break;
        }
    }
}

typedef struct
{
    lua_State * L;
    lua_Debug * ar;
} Args_ll;

static int ll(Args_ll * args, SocketBuf * sb);

/*
** Input format:
** ll [stack level]
**
** Output format:
** OK
** Name Value
** Name Value
** ...
**
** L stays unchanged.
*/
int listLocals(lua_State * L, lua_Debug * ar, char * argv[], int argc, SOCKET s)
{
    struct lua_Debug AR;
    int level;
    Args_ll args;

    if (argc > 0) {
        level = strtol(argv[0], NULL, 10);
    }
    else {
        level = 1;
    }

    if (--level != 0) {
        if (!lua_getstack(L, level, &AR)) {
            return SendErr(s, "No local variable info available at stack level %d.",
                level + 1);
        }
        ar = &AR;
    }
    args.L = L;
    args.ar = ar;
    return SendOK(s, (Writer)ll, &args);
}

int ll(Args_ll * args, SocketBuf * sb)
{
    int i = 1;
    const char * name;

    while ((name = lua_getlocal(args->L, args->ar, i++))) {
        if (name[0] != '(')   //(*temporary)
            printVar(sb, name, args->L);
        lua_pop(args->L, 1);
    }
    return 0;
}

static int lu(lua_State * L, SocketBuf * sb);

/*
** Input format:
** lu [stack level]
**
** Output format:
** OK
** Name Value
** Name Value
** ...
**
** L stays unchanged.
*/
int listUpVars(lua_State * L, lua_Debug * ar, char * argv[], int argc, SOCKET s)
{
    struct lua_Debug AR;
    int level;
    int rc;

    if (argc > 0) {
        level = strtol(argv[0], NULL, 10);
    }
    else {
        level = 1;
    }

    if (--level != 0) {
        if (!lua_getstack(L, level, &AR)) {
            return SendErr(s, "No up variable info available at stack level %d.",
                level + 1);
        }
        ar = &AR;
    }

    lua_getinfo(L, "f", ar);
    rc = SendOK(s, (Writer)lu, L);
    lua_pop(L, 1);
    return rc;
}

int lu(lua_State * L, SocketBuf * sb)
{
    int i = 1;
    const char * name;

    while ((name = lua_getupvalue(L, -1, i++))) {
        printVar(sb, name, L);
        lua_pop(L, 1);
    }
    return 0;
}

static int lg(lua_State * L, SocketBuf * sb);

/*
** Input format:
** lg [stack level]
**
** Output format:
** OK
** Name Value
** Name Value
** ...
**
** L stays unchanged.
*/
int listGlobals(lua_State * L, lua_Debug * ar, char * argv[], int argc, SOCKET s)
{
    struct lua_Debug AR;
    int level;
    int rc;

    if (argc > 0) {
        level = strtol(argv[0], NULL, 10);
    }
    else {
        level = 1;
    }

    if (--level != 0) {
        if (!lua_getstack(L, level, &AR)) {
            return SendErr(s, "No global variable info available at stack level %d.",
                level + 1);
        }
        ar = &AR;
    }

    lua_getinfo(L, "f", ar);
    lua_getfenv(L, -1);
    assert(lua_istable(L, -1));
    rc = SendOK(s, (Writer)lg, L);
    lua_pop(L, 2);
    return rc;
}

static int isID(const char * name)
{
    if (!(isalpha(*name) || *name == '_'))
        return 0;
    ++name;
    while (isalnum(*name) || *name == '_')
        ++name;
    return !*name ? 1 : 0;
}

int lg(lua_State * L, SocketBuf * sb)
{
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        if (lua_isstring(L, -2)) {
            unsigned int len;
            const char * name = lua_tolstring(L, -2, &len);
            if (strlen(name) == len && isID(name))
                printVar(sb, name, L);
        }
        lua_pop(L, 1);
    }
    return 0;
}

static int lookupVar(lua_State * L, lua_Debug * ar, int level, char scope,
    const char * name, int nameLen);
static int lookupField(lua_State * L, const char * field);
static int w(lua_State * L, SocketBuf * sb);

/*
** Input format:
** w <level> <l|u|g> <name>[fields] [r]
** or:
** w [fields] [r]
**
** in which, fields have the form like |n123.4|b0|s"hello"|s008b917a|f006c4560|...
** Output format:
** OK
** Detail
**
** L stays unchanged.
*/
int watch(lua_State * L, lua_Debug * ar, char * argv[], int argc, SOCKET s)
{
    int remember = 0;
    char * fields;
    int rc;
    int top = lua_gettop(L);

    if (argc >= 3) {
        int level = strtol(argv[0], NULL, 10);
        char scope = argv[1][0];
        char * name = argv[2];
        char * nameEnd = strchr(name, '|');
        int nameLen = nameEnd ? nameEnd - name : strlen(name);

        if (level < 1 || argv[1][1] != 0 || !(scope == 'l' || scope == 'u' || scope == 'g'))
            return SendErr(s, "Invalid argument!");
        if (!lookupVar(L, ar, level, scope, name, nameLen)) {
            assert(lua_gettop(L) == top);
            return SendErr(s, "Variable is not found!");
        }
        if (argc > 3 && !strcmp(argv[3], "r"))
            remember = 1;
        fields = nameEnd;
    }
    else {
        lua_pushliteral(L, "cacheValue");
        lua_rawget(L, -2);
        if (lua_isnil(L, 1)) {
            lua_pop(L, 1);
            assert(lua_gettop(L) == top);
            return SendErr(s, "Variable is not found!");
        }
        if (argc > 0)
            fields = argv[0];
        if (argc > 1 && !strcmp(argv[1], "r"))
            remember = 1;
    }

    if (fields && !lookupField(L, fields)) {
        lua_pop(L, 1);
        assert(lua_gettop(L) == top);
        return SendErr(s, "Field is not found!");
    }

    rc = SendOK(s, (Writer)w, L);
    if (remember) {
        lua_pushliteral(L, "cacheValue");
        lua_insert(L, -2);
        lua_rawset(L, fields ? -4 : -3);
        if (fields)
            lua_pop(L, 1);
    }
    else {
        lua_pop(L, fields ? 2 : 1);
    }

    assert(lua_gettop(L) == top);
    return rc;
}

/*
** Look up a lua variable with specified stack level, scope and name.
** Return 1 when found and push it on top of L; otherwise 0 is returned and
** L stays unchanged.
*/
int lookupVar(lua_State * L, lua_Debug * ar, int level, char scope, const char * name, int nameLen)
{
    lua_Debug AR;
    int found = 0;

    if (level != 1) {
        if (!lua_getstack(L, level - 1, &AR))
            return 0;
        ar = &AR;
    }

    if (scope == 'l') {
        int i = 1;
        const char * p;

        lua_pushnil(L); //place holder
        while ((p = lua_getlocal(L, ar, i++))) {
            if (!strncmp(p, name, nameLen)) {
                found = 1;
                lua_replace(L, -2); //The same name may have multi values(though it's odd!), use the last!
            }
            else {
                lua_pop(L, 1);
            }
        }
        if (!found)
            lua_pop(L, 1);
    }
    else if (scope == 'u') {
        int i = 1;
        const char * p;

        lua_getinfo(L, "f", ar);
        while ((p = lua_getupvalue(L, -1, i++))) {
            if (!strncmp(p, name, nameLen)) {
                found = 1;
                break;
            }
            else {
                lua_pop(L, 1);
            }
        }
        lua_remove(L, found ? -2 : -1);
    }
    else {
        lua_getinfo(L, "f", ar);
        lua_getfenv(L, -1);
        assert(lua_istable(L, -1));
        lua_pushlstring(L, name, nameLen);
        lua_gettable(L, -2);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 3);
        }
        else {
            found = 1;
            lua_replace(L, -3);
            lua_pop(L, 1);
        }
    }

    return found;
}

static int nextField(const char * fields, const char ** begin, const char ** end)
{
    assert(fields);
    if (*fields++ != '|' || !*fields)
        return 0;

    *begin = fields;
    if (fields[0] == 's' && fields[1] == '\'') {
        while (1) {
            const char * p = strchr(fields + 2, '\'');
            if (!p)
                return 0;
            ++p;
            if (*p != '|' && *p != 0)
                return 0;
            *end = p;
            break;
        }
    }
    else {
        const char * p = strchr(fields + 1, '|');
        *end = p ? p : fields + strlen(fields);
    }
    return 1;
}

/*
** Get a table field by comparing its value's pointer and type with the specified
** one.
** Return 1 and push the value on top of L when success; otherwise return 0 and
** push nothing.
*/
static int getFieldValueByPtr(lua_State * L, void * ptr, int type)
{
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        int t = lua_type(L, -1);

        if (t == LUA_TTABLE || t == LUA_TFUNCTION || t == LUA_TTHREAD) {
            if (lua_topointer(L, -1) == ptr) {
                lua_remove(L, -2);
                return 1;
            }
        }
        else if (t == LUA_TUSERDATA || t == LUA_TLIGHTUSERDATA) {
            if (lua_touserdata(L, -1) == ptr) {
                lua_remove(L, -2);
                return 1;
            }
        }
        else if (t == LUA_TSTRING) {
            if (lua_tostring(L, -1) == ptr) {
                lua_remove(L, -2);
                return 1;
            }
        }

        lua_pop(L, 1);
    }
    return 0;
}

/*
** Get a table field. Return 1 and push the field value on top of L; otherwise
** return 0 and push nothing.
** The table is on top of L. The field is sth. like "n123.456", "f008bae20", etc.
*/
static int getFieldValue(lua_State * L, const char * fieldBegin, const char * fieldEnd)
{
    char * end;

    if (*fieldBegin == 'n') {
        double num = strtod(fieldBegin + 1, &end);
        if (end != fieldEnd)
            return 0;
        lua_pushnumber(L, num);
        lua_gettable(L, -2);
    }
    else if (*fieldBegin == 's' && fieldBegin[1] == '\'') {
        const char * str = fieldBegin + 2;
        assert(*(fieldEnd - 1) == '\'');
        lua_pushlstring(L, str, fieldEnd - str - 1);
        lua_gettable(L, -2);
    }
    else if (*fieldBegin == 'b') {
        int n = strtol(fieldBegin + 1, &end, 0);
        if (end != fieldEnd)
            return 0;
        lua_pushboolean(L, n);
        lua_gettable(L, -2);
    }
    else if (*fieldBegin == 'U') {
        unsigned int ptr = strtoul(fieldBegin + 1, &end, 0);
        if (end != fieldEnd)
            return 0;
        lua_pushlightuserdata(L, (void *)ptr);
        lua_gettable(L, -2);
    }
    else {
        unsigned int ptr;
        int t;

        switch (*fieldBegin) {
            case 't':
                t = LUA_TTABLE;
                break;
            case 'u':
                t = LUA_TTABLE;
                break;
            case 'f':
                t = LUA_TTABLE;
                break;
            case 'd':
                t = LUA_TTABLE;
                break;
            default:
                return 0;
        }

        ptr = strtoul(fieldBegin + 1, &end, 0);
        if (end != fieldEnd)
            return 0;
        return getFieldValueByPtr(L, (void *)ptr, t);
    }
    return 1;
}

/*
** Look up a field in a lua value on top of L. If the field is found, it's pushed
** on top of L and 1 is returned; otherwise nothing is pushed and 0 is returned.
** field is a descriptive string like '|n123|s"something"|f0088abe8|...'.
** Specially, "|" is a field representing the value itself. And particularly,
** "|m" is a legal field denoting the metatable. So surprisingly, any Lua value
** , not limited to table, can have subfields, because according to offical Lua
** document, "Every value in Lua may have a metatable". For example, consider
** "a = 123", then for variable a, 'a|m|s"__add"' is legal.
*/
int lookupField(lua_State * L, const char * field)
{
    const char * subfieldBegin;
    const char * subfieldEnd;

    assert(field && *field);

    lua_pushvalue(L, -1);
    while (*field && nextField(field, &subfieldBegin, &subfieldEnd)) {
        if (*subfieldBegin == 'm') {
            if (!lua_getmetatable(L, -1))
                break;
        }
        else {
            if (!lua_istable(L, -1))
                break;
            if (!getFieldValue(L, subfieldBegin, subfieldEnd))
                break;
        }
        lua_replace(L, -2);
        field = subfieldEnd;
    }

    if (field[0] && !(field[0] == '|' && field[1] == 0)) {
        lua_pop(L, 1);
        return 0;
    }
    return 1;
}

int w(lua_State * L, SocketBuf * sb)
{
    int t = lua_type(L, -1);
    int meta;
    if (t != LUA_TNIL && (meta = lua_getmetatable(L, -1)))
        lua_pop(L, 1);
    printVar(sb, NULL, L);

    switch (t) {
        case LUA_TTABLE: {
            SB_Print(sb, "%d\n", meta ? 1 : 0);
            lua_pushnil(L);
            while (lua_next(L, -2)) {
                lua_pushvalue(L, -2);
                printVar(sb, NULL, L);
                lua_pop(L, 1);
                printVar(sb, NULL, L);
                lua_pop(L, 1);
            }
            break;
        }
        case LUA_TUSERDATA: {
            int size = lua_objlen(L, -1);
            SB_Print(sb, "%d\n%d\n", meta ? 1 : 0, size);
            break;
        }
        case LUA_TFUNCTION: {
            lua_Debug ar;
            lua_pushvalue(L, -1);
            lua_getinfo(L, ">S", &ar);
            SB_Print(sb, "%d\n%s\n%s\n%d\n%d\n", meta ? 1 : 0, ar.what,
                ar.short_src, ar.linedefined, ar.lastlinedefined);
            break;
        }
        case LUA_TNUMBER: {
            SB_Print(sb, "%d\n", meta ? 1 : 0);
            break;
        }
        case LUA_TSTRING: {
            SB_Print(sb, "%d\n", meta ? 1 : 0);
            break;
        }
        case LUA_TBOOLEAN: {
            SB_Print(sb, "%d\n", meta ? 1 : 0);
            break;
        }
        case LUA_TLIGHTUSERDATA: {
            SB_Print(sb, "%d\n", meta ? 1 : 0);
            break;
        }
        case LUA_TTHREAD: {
            int status = lua_status(lua_tothread(L, -1));
            SB_Print(sb, "%d\n%d\n", meta ? 1 : 0, status);
            break;
        }
    }
    return 0;
}

static int ps(lua_State * L, SocketBuf * sb);

/*
** Input format:
** ps
**
** Output format:
** OK
** File
** Line Number
** Function Name
** Name What
** File
** Line Number
** Function Name
** Name What
** ...
**
** L stays unchanged.
*/
int printStack(lua_State * L, SOCKET s)
{
    return SendOK(s, (Writer)ps, L);
}

int ps(lua_State * L, SocketBuf * sb)
{
    struct lua_Debug ar;
    int i = 0;

    while (lua_getstack(L, i, &ar)) {
        lua_getinfo(L, "nSl", &ar);
        SB_Print(sb, "%s\n%d\n%s\n%s\n", ar.short_src, ar.currentline,
            ar.name ? ar.name : "[N/A]", *ar.what ? ar.what : "[N/A]");
        i++;
    }
    return 0;
}

/*
** Input format:
** sb <File> <Line>
** or:
** db <File> <Line>
**
** Output format:
** OK
**
** On top of L is the "debugger" table stored in LUA_REGISTRYINDEX. L stays
** unchanged after call, but the "debugger" table may be changed.
*/
int setBreakPoint(lua_State * L, const char * src, char * argv[], int argc, int del, SOCKET s)
{
    int line;
    const char * file;
    char path[_MAX_PATH + 1];

    if (argc < 2 || (line = strtol(argv[1], NULL, 10)) <= 0) {
        return SendErr(s, "Invalid argument!");
    }

    if (!strcmp(argv[0], "."))
        file = src;
    else
        file = argv[0];

    if (!_fullpath(path, file, _MAX_PATH) || _access(path, 0)) {
        return SendErr(s, "Invalid path!");
    }
#ifdef OS_WIN
    _strlwr(path);
#endif

    lua_pushliteral(L, "breakpoints");
    lua_rawget(L, -2);
    lua_pushstring(L, path);
    lua_rawget(L, -2);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushstring(L, path);
        lua_pushvalue(L, -2);
        lua_rawset(L, -4);
    }
    if (del)
        lua_pushnil(L);
    else
        lua_pushboolean(L, 1);
    lua_rawseti(L, -2, line);

    if (del) { //check if the path table is empty
        lua_pushnil(L);
        if (!lua_next(L, -2)) { //remove the entry from breakpoints table if it's empty
            lua_pushstring(L, path);
            lua_pushnil(L);
            lua_rawset(L, -4);
        }
        else
            lua_pop(L, 2);
    }
    lua_pop(L, 2);
    return SendOK(s, NULL, NULL);
}

static int lb(lua_State * L, SocketBuf * sb);

/*
** Input format:
** lb
**
** Output format:
** OK
** File
** Line Number
** File
** Line Number
** ...
**
** On top of L is the "debugger" table stored in LUA_REGISTRYINDEX. L stays
** unchanged after call.
*/
int listBreakPoints(lua_State * L, SOCKET s)
{
    return SendOK(s, (Writer)lb, L);
}

static int sortKey(lua_State * L);

int lb(lua_State * L, SocketBuf * sb)
{
    int i, n;
    int top = lua_gettop(L);

    lua_pushliteral(L, "breakpoints");
    lua_rawget(L, -2);
    n = sortKey(L);

    for (i = 1; i <= n; i++) {
        int j, m;
        const char * path;

        lua_rawgeti(L, -1, i);
        path = lua_tostring(L, -1);
        lua_rawget(L, -3);
        assert(lua_istable(L, -1));

        m = sortKey(L);
        for (j = 1; j <= m; j++) {
            lua_rawgeti(L, -1, j);
            SB_Print(sb, "%s\n%d\n", path, lua_tointeger(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 2);
    }
    lua_pop(L, 2);
    assert(top == lua_gettop(L));
    return 0;
}

/*
** Given a table on top of L, it sorts the keys of that table and stores
** the sorted keys in a new table returned on top of L. Thus L increases by 1.
** Return the length of the new table.
*/
int sortKey(lua_State * L)
{
    int i = 1;
    lua_newtable(L);
    lua_pushnil(L);
    while (lua_next(L, -3)) {
        lua_pushvalue(L, -2);
        lua_rawseti(L, -4, i++);
        lua_pop(L, 1);
    }

    lua_getglobal(L, "table");
    lua_getfield(L, -1, "sort");
    lua_pushvalue(L, -3);
    lua_call(L, 1, 0);
    lua_pop(L, 1);
    return i - 1;
}

/*
** L stays unchanged.
*/
int exec(lua_State * L, lua_Debug * ar, char * argv[], int argc, SOCKET s)
{
    return -2;
}

/*
** Input format:
** m <addr> <len>
**
** Output format:
** OK
** content
**
** Warning:
** This function may read an unreadable address and cause a hard/OS exception!
*/
int watchMemory(char * argv[], int argc, SOCKET s)
{
    void * addr;
    unsigned int len;
    SocketBuf sb;

    if (argc < 2 || (addr = (void *)strtoul(argv[0], NULL, 0)) == 0
        || (len = strtoul(argv[1], NULL, 0)) <= 0
        || (unsigned int)((unsigned int)addr + len) < (unsigned int)addr) //overflow!
    {
        return SendErr(s, "Invalid argument!");
    }

    SB_Init(&sb, s);
    SB_Print(&sb, "OK\n%08x\n", len);
    SB_Add(&sb, addr, len);
    return SB_Send(&sb);
}
