/*
 * Lua bytecode-to-C compiler
 */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "ldebug.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lopnames.h"
#include "lstate.h"
#include "lundump.h"

static const char *program_name    = "luaot";
static const char *input_filename  = NULL;
static const char *output_filename = NULL;
static const char *module_name     = NULL;
static FILE * output_file = NULL;
static int nfunctions = 0;
static TString **tmname;

static
void usage_error()
{
    fprintf(stderr, "usage: %s input.lua output.c\n", program_name);
    exit(1);
}

static
void fatal_error(const char *msg)
{
    fprintf(stderr, "%s: %s\n", program_name, msg);
    exit(1);
}

static
__attribute__ ((format (printf, 1, 2)))
void print(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(output_file, fmt, args);
    va_end(args);
}

static
__attribute__ ((format (printf, 1, 2)))
void println(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(output_file, fmt, args);
    va_end(args);
    fprintf(output_file, "\n");
}

static const char *get_module_name(const char *);
static void print_functions();
static void print_source_code();

int main(int argc, char **argv)
{
    program_name = argv[0];

    // Process input options
    if (argc != 3) { usage_error(); }
    input_filename = argv[1];
    output_filename = argv[2];

    module_name = get_module_name(output_filename);

    // Read the input

    lua_State *L = luaL_newstate();
    if (luaL_loadfile(L, input_filename) != LUA_OK) {
        fatal_error(lua_tostring(L,-1));
    }
    Proto *proto = getproto(s2v(L->top-1));
    tmname = G(L)->tmname;

    // Generate the file

    output_file = fopen(output_filename, "w");
    if (output_file == NULL) { fatal_error(strerror(errno)); }

    println("#include \"luaot_header.c\"");
    println(" ");
    print_functions(proto);
    println(" ");
    print_source_code();
    println(" ");
    println("#define LUA_AOT_LUAOPEN_NAME luaopen_%s", module_name);
    println(" ");
    println("#include \"luaot_footer.c\"");
}

/* Deduce the Lua module name given the file name
 * For example:  ./foo/bar/frobnator.c -> frobinator
 */
static
const char *get_module_name(const char *filename)
{
    const char *start = filename;
    for (const char *p = filename; *p != 0; p++) {
        if (*p == '/') {
            start = p+1;
        }
    }

    const char *sep = NULL;
    for (const char *p = start; *p != 0; p++) {
        if (*p == '.') {
            sep = p;
            break;
        }
    }

    if (0 != strcmp(sep+1, "c")) {
        fatal_error("output file is not of a \"c\" file");
    }

    size_t name_size = sep - start;
    char *module_name = malloc(name_size + 1);
    for (size_t i = 0; i < name_size; i++) {
        module_name[i] = start[i];
    }
    module_name[name_size] = '\0';

    return module_name;
}

#define UPVALNAME(x) ((f->upvalues[x].name) ? getstr(f->upvalues[x].name) : "-")
#define VOID(p) ((const void*)(p))
#define eventname(i) (getstr(tmname[i]))

static
void PrintString(const TString* ts)
{
    // Adapted from the PrintString function of luac.c
    const char* s = getstr(ts);
    size_t i,n = tsslen(ts);
    print("\"");
    for (i=0; i<n; i++) {
        int c=(int)(unsigned char)s[i];
        switch (c) {
            case '"':
                print("\\\"");
                break;
            case '\\':
                print("\\\\");
                break;
            case '\a':
                print("\\a");
                break;
            case '\b':
                print("\\b");
                break;
            case '\f':
                print("\\f");
                break;
            case '\n':
                print("\\n");
                break;
            case '\r':
                print("\\r");
                break;
            case '\t':
                print("\\t");
                break;
            case '\v':
                print("\\v");
                break;
            default:
                if (isprint(c)) {
                    print("%c",c);
                } else {
                    print("\\%03d",c);
                }
                break;
        }
    }
    print("\"");
}

static
void PrintConstant(const Proto* f, int i)
{
    // Adapted from the PrintConstant function of luac.c
    const TValue* o=&f->k[i];
    switch (ttypetag(o)) {
        case LUA_TNIL:
            print("nil");
            break;
        case LUA_TBOOLEAN:
            print(bvalue(o) ? "true" : "false");
            break;
        case LUA_TNUMFLT:
            {
                char buff[100];
                sprintf(buff,LUA_NUMBER_FMT,fltvalue(o));
                print("%s",buff);
                if (buff[strspn(buff,"-0123456789")]=='\0') print(".0");
                break;
            }
        case LUA_TNUMINT:
            print(LUA_INTEGER_FMT,ivalue(o));
            break;
        case LUA_TSHRSTR:
        case LUA_TLNGSTR:
            PrintString(tsvalue(o));
            break;
        default:
            /* cannot happen */
            print("?%d",ttypetag(o));
            break;
    }
}

static
void print_opcode_comment(Proto *f, int pc)
{
    // Adapted from the PrintCode function of luac.c
    const Instruction i = f->code[pc];
    OpCode o = GET_OPCODE(i);
    int a=GETARG_A(i);
    int b=GETARG_B(i);
    int c=GETARG_C(i);
    int ax=GETARG_Ax(i);
    int bx=GETARG_Bx(i);
    int sb=GETARG_sB(i);
    int sc=GETARG_sC(i);
    int sbx=GETARG_sBx(i);
    int isk=GETARG_k(i);
    int line=luaG_getfuncline(f,pc);

    #define COMMENT	"\t; "

    print("  //");
    print(" %d\t", pc+1);
    if (line > 0) {
        print("[%d]\t", line);
    } else {
        print("[-]\t");
    }
    print("%-9s\t", opnames[o]);
    switch (o) {
        case OP_MOVE:
            print("%d %d",a,b);
            break;
        case OP_LOADI:
            print("%d %d",a,sbx);
            break;
        case OP_LOADF:
            print("%d %d",a,sbx);
            break;
        case OP_LOADK:
            print("%d %d",a,bx);
            print(COMMENT); PrintConstant(f,bx);
            break;
        case OP_LOADKX:
            print("%d",a);
            break;
        case OP_LOADBOOL:
            print("%d %d %d",a,b,c);
            if (c) print(COMMENT "to %d",pc+2);
            break;
        case OP_LOADNIL:
            print("%d %d",a,b);
            print(COMMENT "%d out",b+1);
            break;
        case OP_GETUPVAL:
            print("%d %d",a,b);
            print(COMMENT "%s", UPVALNAME(b));
            break;
        case OP_SETUPVAL:
            print("%d %d",a,b);
            print(COMMENT "%s", UPVALNAME(b));
            break;
        case OP_GETTABUP:
            print("%d %d %d",a,b,c);
            print(COMMENT "%s", UPVALNAME(b));
            print(" "); PrintConstant(f,c);
            break;
        case OP_GETTABLE:
            print("%d %d %d",a,b,c);
            break;
        case OP_GETI:
            print("%d %d %d",a,b,c);
            break;
        case OP_GETFIELD:
            print("%d %d %d",a,b,c);
            print(COMMENT); PrintConstant(f,c);
            break;
        case OP_SETTABUP:
            print("%d %d %d%s",a,b,c, isk ? "k" : "");
            print(COMMENT "%s",UPVALNAME(a));
            print(" "); PrintConstant(f,b);
            if (isk) { print(" "); PrintConstant(f,c); }
            break;
        case OP_SETTABLE:
            print("%d %d %d%s",a,b,c, isk ? "k" : "");
            if (isk) { print(COMMENT); PrintConstant(f,c); }
            break;
        case OP_SETI:
            print("%d %d %d%s",a,b,c, isk ? "k" : "");
            if (isk) { print(COMMENT); PrintConstant(f,c); }
            break;
        case OP_SETFIELD:
            print("%d %d %d%s",a,b,c, isk ? "k" : "");
            print(COMMENT); PrintConstant(f,b);
            if (isk) { print(" "); PrintConstant(f,c); }
            break;
        case OP_NEWTABLE:
            print("%d %d %d",a,b,c);
            break;
        case OP_SELF:
            print("%d %d %d%s",a,b,c, isk ? "k" : "");
            if (isk) { print(COMMENT); PrintConstant(f,c); }
            break;
        case OP_ADDI:
            print("%d %d %d %s",a,b,sc,isk ? "F" : "");
            break;
        case OP_ADDK:
            print("%d %d %d %s",a,b,c,isk ? "F" : "");
            print(COMMENT); PrintConstant(f,c);
            break;
        case OP_SUBK:
            print("%d %d %d",a,b,c);
            print(COMMENT); PrintConstant(f,c);
            break;
        case OP_MULK:
            print("%d %d %d %s",a,b,c,isk ? "F" : "");
            print(COMMENT); PrintConstant(f,c);
            break;
        case OP_MODK:
            print("%d %d %d",a,b,c);
            print(COMMENT); PrintConstant(f,c);
            break;
        case OP_POWK:
            print("%d %d %d",a,b,c);
            print(COMMENT); PrintConstant(f,c);
            break;
        case OP_DIVK:
            print("%d %d %d",a,b,c);
            print(COMMENT); PrintConstant(f,c);
            break;
        case OP_IDIVK:
            print("%d %d %d",a,b,c);
            print(COMMENT); PrintConstant(f,c);
            break;
        case OP_BANDK:
            print("%d %d %d",a,b,c);
            print(COMMENT); PrintConstant(f,c);
            break;
        case OP_BORK:
            print("%d %d %d",a,b,c);
            print(COMMENT); PrintConstant(f,c);
            break;
        case OP_BXORK:
            print("%d %d %d",a,b,c);
            print(COMMENT); PrintConstant(f,c);
            break;
        case OP_SHRI:
            print("%d %d %d",a,b,sc);
            break;
        case OP_SHLI:
            print("%d %d %d",a,b,sc);
            break;
        case OP_ADD:
            print("%d %d %d",a,b,c);
            break;
        case OP_SUB:
            print("%d %d %d",a,b,c);
            break;
        case OP_MUL:
            print("%d %d %d",a,b,c);
            break;
        case OP_MOD:
            print("%d %d %d",a,b,c);
            break;
        case OP_POW:
            print("%d %d %d",a,b,c);
            break;
        case OP_DIV:
            print("%d %d %d",a,b,c);
            break;
        case OP_IDIV:
            print("%d %d %d",a,b,c);
            break;
        case OP_BAND:
            print("%d %d %d",a,b,c);
            break;
        case OP_BOR:
            print("%d %d %d",a,b,c);
            break;
        case OP_BXOR:
            print("%d %d %d",a,b,c);
            break;
        case OP_SHL:
            print("%d %d %d",a,b,c);
            break;
        case OP_SHR:
            print("%d %d %d",a,b,c);
            break;
        case OP_MMBIN:
            print("%d %d %d",a,b,c);
            print(COMMENT "%s",eventname(c));
            break;
        case OP_MMBINI:
            print("%d %d %d",a,sb,c);
            print(COMMENT "%s",eventname(c));
            break;
        case OP_MMBINK:
            print("%d %d %d",a,b,c);
            print(COMMENT "%s ",eventname(c)); PrintConstant(f,b);
            break;
        case OP_UNM:
            print("%d %d",a,b);
            break;
        case OP_BNOT:
            print("%d %d",a,b);
            break;
        case OP_NOT:
            print("%d %d",a,b);
            break;
        case OP_LEN:
            print("%d %d",a,b);
            break;
        case OP_CONCAT:
            print("%d %d",a,b);
            break;
        case OP_CLOSE:
            print("%d",a);
            break;
        case OP_TBC:
            print("%d",a);
            break;
        case OP_JMP:
            print("%d",GETARG_sJ(i));
            print(COMMENT "to %d",GETARG_sJ(i)+pc+2);
            break;
        case OP_EQ:
            print("%d %d %d",a,b,isk);
            break;
        case OP_LT:
            print("%d %d %d",a,b,isk);
            break;
        case OP_LE:
            print("%d %d %d",a,b,isk);
            break;
        case OP_EQK:
            print("%d %d %d",a,b,isk);
            print(COMMENT); PrintConstant(f,b);
            break;
        case OP_EQI:
            print("%d %d %d",a,sb,isk);
            break;
        case OP_LTI:
            print("%d %d %d",a,sb,isk);
            break;
        case OP_LEI:
            print("%d %d %d",a,sb,isk);
            break;
        case OP_GTI:
            print("%d %d %d",a,sb,isk);
            break;
        case OP_GEI:
            print("%d %d %d",a,sb,isk);
            break;
        case OP_TEST:
            print("%d %d",a,isk);
            break;
        case OP_TESTSET:
            print("%d %d %d",a,b,isk);
            break;
        case OP_CALL:
            print("%d %d %d",a,b,c);
            print(COMMENT);
            if (b==0) print("all in "); else print("%d in ",b-1);
            if (c==0) print("all out"); else print("%d out",c-1);
            break;
        case OP_TAILCALL:
            print("%d %d %d",a,b,c);
            print(COMMENT "%d in",b-1);
            break;
        case OP_RETURN:
            print("%d %d %d",a,b,c);
            print(COMMENT);
            if (b==0) print("all out"); else print("%d out",b-1);
            break;
        case OP_RETURN0:
            break;
        case OP_RETURN1:
            print("%d",a);
            break;
        case OP_FORLOOP:
            print("%d %d",a,bx);
            print(COMMENT "to %d",pc-bx+2);
            break;
        case OP_FORPREP:
            print("%d %d",a,bx);
            print(COMMENT "to %d",pc+bx+2);
            break;
        case OP_TFORPREP:
            print("%d %d",a,bx);
            print(COMMENT "to %d",pc+bx+2);
            break;
        case OP_TFORCALL:
            print("%d %d",a,c);
            break;
        case OP_TFORLOOP:
            print("%d %d",a,bx);
            print(COMMENT "to %d",pc-bx+2);
            break;
        case OP_SETLIST:
            print("%d %d %d",a,b,c);
            break;
        case OP_CLOSURE:
            print("%d %d",a,bx);
            print(COMMENT "%p",VOID(f->p[bx]));
            break;
        case OP_VARARG:
            print("%d %d",a,c);
            print(COMMENT);
            if (c==0) print("all out"); else print("%d out",c-1);
            break;
        case OP_VARARGPREP:
            print("%d",a);
            break;
        case OP_EXTRAARG:
            print("%d",ax);
            print(COMMENT); PrintConstant(f,ax);
            break;
        default:
            print("%d %d %d",a,b,c);
            print(COMMENT "not handled");
            break;
    }
    print("\n");
}

static
void create_function(Proto *p)
{
    int func_id = nfunctions++;

    println("// source = %s", getstr(p->source));
    if (p->linedefined == 0) {
        println("// main function");
    } else {
        println("// lines: %d - %d", p->linedefined, p->lastlinedefined);
    }

    println("static");
    println("void magic_implementation_%02d(lua_State *L, CallInfo *ci)", func_id);
    println("{");

    println("  LClosure *cl;");
    println("  TValue *k;");
    println("  StkId base;");
    println("  const Instruction *saved_pc;");
    println("  int trap;");
    println("  ");
    println(" tailcall:");
    println("  trap = L->hookmask;");
    println("  cl = clLvalue(s2v(ci->func));");
    println("  k = cl->p->k;");
    println("  saved_pc = ci->u.l.savedpc;  /*no explicit program counter*/ " );
    println("  if (trap) {");
    println("    if (cl->p->is_vararg)");
    println("      trap = 0;  /* hooks will start after VARARGPREP instruction */");
    println("    else if (saved_pc == cl->p->code) /*first instruction (not resuming)?*/");
    println("      luaD_hookcall(L, ci);");
    println("    ci->u.l.trap = 1;  /* there may be other hooks */");
    println("  }");
    println("  base = ci->func + 1;");
    println("  /* main loop of interpreter */");
    println("  Instruction *function_code = cl->p->code;");
    println(" ");

    for (int pc = 0; pc < p->sizecode; pc++) {
        Instruction instr = p->code[pc];
        OpCode op = GET_OPCODE(instr);

        print_opcode_comment(p, pc);


        // While an instruction is executing, the program counter typically
        // points towards the next instruction. There are some corner cases
        // where the program counter getss adjusted mid-instruction, but I
        // am not breaking anything because of those...
        println("  #undef  LUA_AOT_PC");
        println("  #define LUA_AOT_PC (function_code + %d)", pc+1);

        int next = pc + 1;
        println("  #undef  LUA_AOT_NEXT_JUMP");
        if (next < p->sizecode && GET_OPCODE(p->code[next]) == OP_JMP) {
            println("  #define LUA_AOT_NEXT_JUMP label_todo");
        }

        int skip1 = pc + 2;
        println("  #undef  LUA_AOT_SKIP1");
        if (skip1 < p->sizecode) {
            println("  #define LUA_AOT_SKIP1 label_%02d", skip1);
        }

        println("  label_%02d : {", pc);
        println("    Instruction i = 0x%08x;", instr);
        println("    StkId ra = RA(i);");
        println("    (void) ra;");

        switch (op) {
            case OP_MOVE:
                println("    setobjs2s(L, ra, RB(i));");
                break;
            case OP_LOADI: {
                println("    lua_Integer b = GETARG_sBx(i);");
                println("    setivalue(s2v(ra), b);");
                break;
            }
            // case OP_LOADF
            // case OP_LOADK
            // case OP_LOADKX
            // case OP_LOADBOOL
            // case OP_LOADNIL
            // case OP_GETUPVAL
            // case SETUPVAL
            // case OP_GETTABUP
            // case OP_GETTABLE
            // case OP_GETI
            // case OP_GETFIELD
            // case OP_SETTABUP
            // case OP_SETTABLE
            // case OP_SETI
            // case OP_SETFIELD
            // case OP_NEWTABLE
            // case OP_SELF
            // case OP_ADDI
            // case OP_ADDK
            // case OP_SUBK
            // case OP_MULK
            // case OP_MODK
            // case OP_POWK
            // case OP_DIVK
            // case OP_IDIVK
            // case OP_BANDK
            // case OP_BORK
            // case OP_BXORK
            // case OP_SHRI
            // case OP_SHLI
            // case OP_ADD
            // case OP_SUB
            // case OP_MUL
            // case OP_MOD
            // case OP_POW
            // case OP_DIV
            // case OP_IDIV
            // case OP_BAND
            // case OP_BOR
            // case OP_BXOR
            // case OP_SHR
            // case OP_SHL
            // case OP_MMBIN
            // case OP_MMBINI
            // case OP_MMBINK
            // case OP_UNM
            // case OP_BNOT
            // case OP_NOT
            // case OP_LEN
            // case OP_CONCAT
            // case OP_CLOSE
            // case OP_TBC
            // case OP_JMP
            // case OP_EQ
            // case OP_LT
            // case OP_LE
            // case OP_EQK
            // case OP_EQI
            // case OP_LTI
            // case OP_LEI
            // case OP_GTI
            // case OP_GEI
            // case OP_TEST
            // case OP_TESTSET
            // case OP_CALL
            // case OP_TAILCAL
            // case OP_RETURN
            case OP_RETURN: {
                println("    int n = GETARG_B(i) - 1;  /* number of results */");
                println("    int nparams1 = GETARG_C(i);");
                println("    if (n < 0)  /* not fixed? */");
                println("      n = cast_int(L->top - ra);  /* get what is available */");
                println("    savepc(ci);");
                println("    if (TESTARG_k(i)) {  /* may there be open upvalues? */");
                println("      if (L->top < ci->top)");
                println("        L->top = ci->top;");
                println("      luaF_close(L, base, LUA_OK);");
                println("      updatetrap(ci);");
                println("      updatestack(ci);");
                println("    }");
                println("    if (nparams1)  /* vararg function? */");
                println("      ci->func -= ci->u.l.nextraargs + nparams1;");
                println("    L->top = ra + n;  /* set call for 'luaD_poscall' */");
                println("    luaD_poscall(L, ci, n);");
                println("    return;");
                break;
            }
            case OP_RETURN0: {
                println("    if (L->hookmask) {");
                println("      L->top = ra;");
                println("      halfProtectNT(luaD_poscall(L, ci, 0));  /* no hurry... */");
                println("    }");
                println("    else {  /* do the 'poscall' here */");
                println("      int nres = ci->nresults;");
                println("      L->ci = ci->previous;  /* back to caller */");
                println("      L->top = base - 1;");
                println("      while (nres-- > 0)");
                println("        setnilvalue(s2v(L->top++));  /* all results are nil */");
                println("    }");
                println("    return;");
                break;
            }
            case OP_RETURN1: {
                println("    if (L->hookmask) {");
                println("      L->top = ra + 1;");
                println("      halfProtectNT(luaD_poscall(L, ci, 1));  /* no hurry... */");
                println("    }");
                println("    else {  /* do the 'poscall' here */");
                println("      int nres = ci->nresults;");
                println("      L->ci = ci->previous;  /* back to caller */");
                println("      if (nres == 0)");
                println("        L->top = base - 1;  /* asked for no results */");
                println("      else {");
                println("        setobjs2s(L, base - 1, ra);  /* at least this result */");
                println("        L->top = base;");
                println("        while (--nres > 0)  /* complete missing results */");
                println("          setnilvalue(s2v(L->top++));");
                println("      }");
                println("    }");
                println("    return;");
                break;
            }
            case OP_FORLOOP: {
                println("    if (ttisinteger(s2v(ra + 2))) {  /* integer loop? */");
                println("      lua_Unsigned count = l_castS2U(ivalue(s2v(ra + 1)));");
                println("      if (count > 0) {  /* still more iterations? */");
                println("        lua_Integer step = ivalue(s2v(ra + 2));");
                println("        lua_Integer idx = ivalue(s2v(ra));  /* internal index */");
                println("        chgivalue(s2v(ra + 1), count - 1);  /* update counter */");
                println("        idx = intop(+, idx, step);  /* add step to index */");
                println("        chgivalue(s2v(ra), idx);  /* update internal index */");
                println("        setivalue(s2v(ra + 3), idx);  /* and control variable */");
                println("        goto label_%02d; /* jump back */", ((pc+1) - GETARG_Bx(instr))); // (!)
                println("      }");
                println("    }");
                println("    else {  /* floating loop */");
                println("      lua_Number step = fltvalue(s2v(ra + 2));");
                println("      lua_Number limit = fltvalue(s2v(ra + 1));");
                println("      lua_Number idx = fltvalue(s2v(ra));");
                println("      idx = luai_numadd(L, idx, step);  /* increment index */");
                println("      if (luai_numlt(0, step) ? luai_numle(idx, limit)");
                println("                              : luai_numle(limit, idx)) {");
                println("        chgfltvalue(s2v(ra), idx);  /* update internal index */");
                println("        setfltvalue(s2v(ra + 3), idx);  /* and control variable */");
                println("        goto label_%02d; /* jump back */", ((pc+1) - GETARG_Bx(instr))); // (!)
                println("      }");
                println("    }");
                println("    updatetrap(ci);  /* allows a signal to break the loop */");
                break;
            }
            case OP_FORPREP: {
                println("    TValue *pinit = s2v(ra);");
                println("    TValue *plimit = s2v(ra + 1);");
                println("    TValue *pstep = s2v(ra + 2);");
                println("    savestate(L, ci);  /* in case of errors */");
                println("    if (ttisinteger(pinit) && ttisinteger(pstep)) { /* integer loop? */");
                println("      lua_Integer init = ivalue(pinit);");
                println("      lua_Integer step = ivalue(pstep);");
                println("      lua_Integer limit;");
                println("      if (step == 0)");
                println("        luaG_runerror(L, \"'for' step is zero\");");
                println("      setivalue(s2v(ra + 3), init);  /* control variable */");
                println("      if (forlimit(L, init, plimit, &limit, step))");
                println("        goto label_%02d; /* skip the loop */", ((pc + 1) + GETARG_Bx(instr) + 1)); // (!)
                println("      else {  /* prepare loop counter */");
                println("        lua_Unsigned count;");
                println("        if (step > 0) {  /* ascending loop? */");
                println("          count = l_castS2U(limit) - l_castS2U(init);");
                println("          if (step != 1)  /* avoid division in the too common case */");
                println("            count /= l_castS2U(step);");
                println("        }");
                println("        else {  /* step < 0; descending loop */");
                println("          count = l_castS2U(init) - l_castS2U(limit);");
                println("          /* 'step+1' avoids negating 'mininteger' */");
                println("          count /= l_castS2U(-(step + 1)) + 1u;");
                println("        }");
                println("        /* store the counter in place of the limit (which won't be");
                println("           needed anymore */");
                println("        setivalue(plimit, l_castU2S(count));");
                println("      }");
                println("    }");
                println("    else {  /* try making all values floats */");
                println("      lua_Number init; lua_Number limit; lua_Number step;");
                println("      if (unlikely(!tonumber(plimit, &limit)))");
                println("        luaG_forerror(L, plimit, \"limit\");");
                println("      if (unlikely(!tonumber(pstep, &step)))");
                println("        luaG_forerror(L, pstep, \"step\");");
                println("      if (unlikely(!tonumber(pinit, &init)))");
                println("        luaG_forerror(L, pinit, \"initial value\");");
                println("      if (step == 0)");
                println("        luaG_runerror(L, \"'for' step is zero\");");
                println("      if (luai_numlt(0, step) ? luai_numlt(limit, init)");
                println("                               : luai_numlt(init, limit))");
                println("        goto label_%02d; /* skip the loop */", ((pc + 1) + GETARG_Bx(instr) + 1)); // (!)
                println("      else {");
                println("        /* make sure internal values are all float */");
                println("        setfltvalue(plimit, limit);");
                println("        setfltvalue(pstep, step);");
                println("        setfltvalue(s2v(ra), init);  /* internal index */");
                println("        setfltvalue(s2v(ra + 3), init);  /* control variable */");
                println("      }");
                println("    }");
                break;
            }
            //case OP_TFORPREP
            //case OP_TFORCALL
            //case OP_TFORLOOP
            //case OP_SETLIST
            case OP_CLOSURE: {
                println("    Proto *p = cl->p->p[GETARG_Bx(i)];");
                println("    halfProtect(pushclosure(L, p, cl->upvals, base, ra));");
                println("    checkGC(L, ra + 1);");
                break;
            }
            case OP_VARARG: {
                println("    int n = GETARG_C(i) - 1;  /* required results */");
                println("    Protect(luaT_getvarargs(L, ci, ra, n));");
                break;
            }
            case OP_VARARGPREP: {
                println("    luaT_adjustvarargs(L, GETARG_A(i), ci, cl->p);");
                println("    updatetrap(ci);");
                println("    if (trap) {");
                println("      luaD_hookcall(L, ci);");
                println("      L->oldpc = LUA_AOT_PC + 1;  /* next opcode will be seen as a \"new\" line */");
                println("    }");
                break;
            }
            case OP_EXTRAARG: {
                println("    lua_assert(0);");
                break;
            }
            default:
                println("    assert(0); /* TODO */");
                break;
        }

        println("  }");
        println("  ");
    }

    println("}");
    println(" ");

}

static
void create_functions(Proto *p)
{
    // luaot_footer.c should use the same traversal order as this.
    create_function(p);
    for (int i = 0; i < p->sizep; i++) {
        create_functions(p->p[i]);
    }
}

static
void print_functions(Proto *p)
{
    create_functions(p);

    println("static AotCompiledFunction LUA_AOT_FUNCTIONS[] = {");
    for (int i = 0; i < nfunctions; i++) {
        println("  magic_implementation_%02d,", i);
    }
    println("  NULL");
    println("};");
}

static
void print_source_code()
{
    // Since the code we are generating is lifted from lvm.c, we need it to use
    // Lua functions instead of C functions. And to create the C functions, we
    // have to `load` them from source code or bytecode. To keep it simple, we
    // load it from source code.
    //
    // There is a C99 limit to how long a string literal can be, so instead of
    // using a string literal we use a large char array instead.

    FILE *infile = fopen(input_filename, "r");
    if (!infile) { fatal_error("could not open input file a second time"); }

    println("static const char LUA_AOT_MODULE_SOURCE_CODE[] = {");

    int c;
    int col = 0;
    do {
        if (col == 0) {
            print("  ");
        }

        c = fgetc(infile);
        if (c == EOF) {
            print("%3d", 0);
        } else {
            print("%3d", c);
            print(", ");
        }

        col++;
        if (col == 16 || c == EOF) {
            print("\n");
            col = 0;
        }
    } while (c != EOF);
    println("};");

    fclose(infile);
}