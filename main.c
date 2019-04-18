#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <ctype.h>
#include <string.h>
#include <assert.h>

#define DEBUG      0
#define DEBUG_GC   0
#define GC_VERBOSE 0

void error(char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(1);
}

#define T_UNUSED       0 // 0000 0000 0000 0000
#define T_NIL          1 // 0000 0000 0000 0001
#define T_IDENT        2 // 0000 0000 0000 0010
#define T_DATA         4 // 0000 0000 0000 0100
#define T_CONS         8 // 0000 0000 0000 1000
#define T_CLOS        16 // 0000 0000 0001 0000
#define T_USRMACRO    32 // 0000 0000 0010 0000
#define T_PRIM        64 // 0000 0000 0100 0000
#define T_MACRO      128 // 0000 0000 1000 0000
#define T_STOP       256 // 0000 0001 0000 0000
#define T_BACK       512 // 0000 0010 0000 0000
#define T_CALL      1024 // 0000 0100 0000 0000
#define T_ARGEND    2048 // 0000 1000 0000 0000
#define T_RET       4096 // 0001 0000 0000 0000
#define T_UNMARK  0x7FFF // 0111 1111 1111 1111
#define T_MARK    0x8000 // 1000 0000 0000 0000

#define MAX_LEN 30

typedef struct _cell {
  int type;
  union {
    int data;
    char ident[MAX_LEN];
    struct {
      struct _cell *car;
      struct _cell *cdr;
    };
    struct {
      struct _cell *env;
      struct _cell *params;
      struct _cell *body;
    };
    struct {
      char name[MAX_LEN];
      void (*func)(struct _cell **, struct _cell**, struct _cell**, struct _cell**);
    };
  };
} cell;

typedef cell* pointer;
typedef void (*primitive_func)(pointer *, pointer *, pointer *, pointer *);

#define TYPE(p, t) ((p)->type & (t))

#define CAR(p)   ((p)->car) 
#define CDR(p)   ((p)->cdr)
#define CAAR(p)  ((p)->car->car)
#define CADR(p)  ((p)->cdr->car)
#define CDAR(p)  ((p)->car->cdr)
#define CDDR(p)  ((p)->cdr->cdr)
#define CAAAR(p) ((p)->car->car->car)
#define CAADR(p) ((p)->cdr->car->car)
#define CADAR(p) ((p)->car->cdr->car)
#define CADDR(p) ((p)->cdr->cdr->car)
#define CDAAR(p) ((p)->car->car->cdr)
#define CDADR(p) ((p)->cdr->car->cdr)
#define CDDAR(p) ((p)->car->cdr->cdr)
#define CDDDR(p) ((p)->cdr->cdr->cdr)

#define CELL_LIMIT 10000
pointer alloc_memory()
{
  return calloc(CELL_LIMIT, sizeof(cell));
}

#define MAX_ROOT 200

// ---- global variables ----
pointer memory, mp;           // momery, and memory pointer
char lex_buf[MAX_LEN];        // temporary buffer for lexer
pointer renv;                 // environment for reader macro
pointer eval_cell;            // for eval call during evaluation of user macros
pointer *root_ptrs[MAX_ROOT]; // root pointers for GC
int rp = 0;                   // pointer of root pointers
// --------

#define BEGIN do {
#define END   } while (0)
#define SAVE(p) BEGIN                    \
                  assert(rp < MAX_ROOT); \
                  p = NULL;              \
                  root_ptrs[rp++] = &p;  \
                END
#define FREE(x) (rp--)

// ---- global constants ----
const pointer nil      = &(cell){ T_NIL  };
const pointer c_stop   = &(cell){ T_STOP  };
const pointer c_call   = &(cell){ T_CALL };
const pointer c_back   = &(cell){ T_BACK };
const pointer c_ret    = &(cell){ T_RET  };
const pointer c_argend = &(cell){ T_ARGEND };
// --------

void print_cell(pointer p);

void mark(pointer *pp)
{
  #define STACK_LENGTH CELL_LIMIT
  #define push(v) (assert(sp < STACK_LENGTH), (stack[sp++] = v))
  #define pop()   (stack[--sp])
  pointer *stack[STACK_LENGTH];
  int sp = 0;
  push(pp);
  while (sp > 0) {
    pp = pop();
    if (*pp == NULL || TYPE(*pp, T_MARK))
      continue;
    if (TYPE(*pp, T_NIL | T_STOP | T_CALL | T_BACK | T_RET | T_ARGEND))
      continue;
    (*pp)->type |= T_MARK;
    if (TYPE(*pp, T_CONS)) {
      push(&(CAR(*pp)));
      push(&(CDR(*pp)));
    } else if (TYPE(*pp, T_CLOS | T_USRMACRO)) {
      push(&((*pp)->env));
      push(&((*pp)->params));
      push(&((*pp)->body));
    }
  }
}

void gc()
{
  int i, cnt;
  for (i = 0; i < rp; i++)
    mark(root_ptrs[i]);
  for (mp = memory, cnt = 0; mp - memory < CELL_LIMIT; mp++) {
    if (TYPE(mp, T_MARK)) {
      mp->type &= T_UNMARK;
    } else {
      memset(mp, 0, sizeof(cell));
      mp->type = T_UNUSED;
      cnt++;
    }
  }
  mp = memory;
#if GC_VERBOSE
  printf("Free: %d\n", cnt);
#endif
}

pointer mk(cell tmp)
{
#if DEBUG_GC
  gc();
#endif
  while (mp - memory < CELL_LIMIT && mp->type != T_UNUSED)
    mp++;
  if (CELL_LIMIT == mp - memory + 1) {
    gc();
    while (mp - memory < CELL_LIMIT && mp->type != T_UNUSED)
      mp++;
    if (CELL_LIMIT == mp - memory + 1)
      error("No memory space is available");
    mp = memory;
    return mk(tmp);
  }
  *mp = tmp;
  return mp++;
}

pointer make_data(int data)
{
  return mk((cell){ T_DATA, .data = data });
}

pointer make_cons(pointer car, pointer cdr)
{
  return mk((cell){ T_CONS, .car = car, .cdr = cdr });
}

#define CONS(a, b) make_cons((a), (b))

pointer CONS3(pointer a, pointer b, pointer c)
{
  pointer tmp;
  SAVE(tmp);
  tmp = CONS(b, c);
  tmp = CONS(a, tmp);
  FREE(tmp);
  return tmp;
}

pointer CONS4(pointer a, pointer b, pointer c, pointer d)
{
  pointer tmp;
  SAVE(tmp);
  tmp = CONS(c, d);
  tmp = CONS(b, tmp);
  tmp = CONS(a, tmp);
  FREE(tmp);
  return tmp;
}

pointer make_ident(char *ident)
{
  pointer p = mk((cell){ T_IDENT });
  strcpy(p->ident, ident);
  return p;
}

pointer make_closure(int type, pointer env, pointer params, pointer body)
{
  return mk((cell){ type, .env = env, .params = params, .body = body });
}

void pc(pointer p);

void plist(pointer p)
{
  pc(CAR(p));
  if (TYPE(CDR(p), T_CONS)) {
    printf(" ");
    plist(CDR(p));
  } else if (!TYPE(CDR(p), T_NIL)) {
    printf(" . ");
    pc(CDR(p));
  }
}

void pc(pointer p)
{
  switch(p->type) {
  case T_NIL:      printf("()");                         break;
  case T_IDENT:    printf("%s", p->ident);               break;
  case T_DATA:     printf("%d", p->data);                break;
  case T_CONS:     printf("("); plist(p); printf(")");   break;
  case T_CLOS:     printf("#<closure>");                 break;
  case T_USRMACRO: printf("#<usrmacro>");                break;
  case T_PRIM:     printf("#<primitive: %s>", p->ident); break;
  case T_MACRO:    printf("#<macro: %s>", p->ident);     break;
  case T_STOP:     printf("stop");                       break;
  case T_BACK:     printf("back");                       break;
  case T_CALL:     printf("call");                       break;
  case T_ARGEND:   printf("argend");                     break;
  case T_RET:      printf("ret");                        break;
  default:         error("Undefined type: %d", p->type);
  }
}

void print_cell(pointer p)
{
  pc(p); printf("\n");
}

pointer reverse(pointer p)
{
  pointer acc, tmp, ret;
  SAVE(acc); SAVE(tmp);
  acc = nil;
  tmp = p;
  while (!TYPE(tmp, T_NIL)) {
    acc = CONS(CAR(tmp), acc);
    tmp = CDR(tmp);
  }
  ret = acc;
  FREE(acc); FREE(tmp);
  return ret;
}

pointer append(pointer p, pointer q)
{
  pointer acc, tmp, ret;
  SAVE(acc); SAVE(tmp);
  acc = q;
  tmp = reverse(p);
  while (!TYPE(tmp, T_NIL)) {
    acc = CONS(CAR(tmp), acc);
    tmp = CDR(tmp);
  }
  ret = acc;
  FREE(acc); FREE(tmp);
  return ret;
}

char lex()
{
  char c = getchar();
  if (c == ';') {
    do {
      c = getchar(); // skip a line comment
    } while (c != EOF && c != '\n');
  }
  return c;
}

char space_lex()
{
  char c;
  while (isspace(c = lex()))
    ;
  return c;
}

void unlex(char c)
{
  ungetc(c, stdin);
}

int issymbol(char c)
{
  return !(c == '(' || c == ')' || c == ';');
}

int isident(char c)
{
  return !isspace(c) && issymbol(c);
}

int read_num(char c)
{
  int n = 0;
  do {
    n = (10*n) + (c-'0');
  } while(isdigit(c = lex()));
  unlex(c);
  return n;
}

char *read_ident(char c)
{
  char *ptr = lex_buf;
  do {
    *(ptr++) = c;
  } while(isident(c = lex()));
  unlex(c);
  *ptr = '\0';
  return lex_buf;
}

pointer parse_cell();

pointer parse_list()
{
  char c;
  pointer p, q, ret;
 
  SAVE(p); SAVE(q);
  p = parse_cell();
  if ((c = space_lex()) == '.') {
    q = parse_cell();
    if ((c = space_lex()) != ')')
      error("Unexpected input: '%c'", c);
    ret = CONS(p, q);
  } else if (c == ')') {
    ret = CONS(p, nil);
  } else {
    unlex(c);
    q = parse_list();
    ret = CONS(p, q);
  }
  FREE(p); FREE(q);
  return ret; 
}

char *strrev(char *str)
{
  int i, len = strlen(str);
  for (i = 0; i < len/2; i++) {
    char tmp = str[i];
    str[i] = str[len-i-1];
    str[len-i-1] = tmp;
  }
  return str;
}

pointer parse_cell()
{
  char c;
  while ((c = space_lex()) != EOF) {
    if (isdigit(c)) {
      return make_data(read_num(c));
    } else if (isident(c)) {
      char *ident = read_ident(c);
      {// For reader macro
        int m_len = 0;
        pointer p = renv, macro = NULL;
        while (!TYPE(p, T_NIL)) {
          if (strstr(ident, CAAR(p)->ident) != NULL) {
            int len = strlen(CAAR(p)->ident);
            if (m_len < len) {
              m_len = len;
              macro = CAR(p);
            }
          }
          p = CDR(p);
        }
        if (macro != NULL) {
          pointer tmp, ret; 
          char *postfix;

          SAVE(tmp);
          postfix = strrev(ident+strlen(CAR(macro)->ident));
          while (*postfix != '\0')
            unlex(*(postfix++));
          tmp = parse_cell();
          ret = CONS3(CDR(macro), tmp, nil);
          FREE(tmp);
          return ret;
        }
      }
      return make_ident(ident);
    } else if (c == '(') {
      if ((c = space_lex()) == ')')
        return nil;
      unlex(c);
      return parse_list();
    } else {
      error("Unexpeced input: '%c'", c);
    }
  }
  return NULL;
}

pointer zip(pointer p, pointer q)
{
  // Assumption: |p| <= |q|, where |x| means the length of x.
  pointer tmp, acc, ret;
  SAVE(acc); SAVE(tmp);
  acc = nil;
  for (;;) {
    assert(TYPE(p, T_NIL | T_IDENT | T_CONS));
    if (TYPE(p, T_NIL)) {
      break;
    } else if (TYPE(p, T_IDENT)) {// If |p| < |q|. (Binds a variadic parameter)
      tmp = CONS(p, q);
      acc = CONS(tmp, acc);
      break;
    } else if (TYPE(p, T_CONS)) {
      tmp = CONS(CAR(p), CAR(q));
      acc = CONS(tmp, acc);
      p = CDR(p);
      q = CDR(q);
    }
  }
  ret = reverse(acc);
  FREE(acc); FREE(tmp);
  return ret;
}
 
pointer lookup(pointer env, char *ident)
{
  while (!TYPE(env, T_NIL)) {
    if (strcmp(CAAR(env)->ident, ident) == 0)
      return CDAR(env);
    else
      env = CDR(env);
  }
  error("Unbound variable: \"%s\"", ident);
}

void add_primitive(pointer *env, int type, char *ident, primitive_func func)
{
  pointer p, bind, tmp;
  SAVE(p); SAVE(bind); SAVE(tmp);
  p    = mk((cell){ type, .func = func });
  tmp  = make_ident(ident);
  bind = CONS(tmp, p);
  strcpy(p->ident, ident);
  *env = CONS(bind, *env);
  FREE(p); FREE(bind); FREE(tmp);
} 

void macro_closure(int type, pointer *stk,  pointer *env, pointer *cnt, pointer *dmp)
{
  pointer params, body, cls, tmp;
  SAVE(params); SAVE(body); SAVE(cls); SAVE(tmp);
  params = CAAR(*stk);
  tmp    = make_ident("begin");
  body   = CONS(tmp, CDAR(*stk));
  cls    = make_closure(type, *env, params, body);
  *stk = CONS(cls, CDR(*stk));
  FREE(params); FREE(body); FREE(cls); FREE(tmp);
}

void macro_lambda(pointer *stk,  pointer *env, pointer *cnt, pointer *dmp)
{
  macro_closure(T_CLOS, stk, env, cnt, dmp);
}

void macro_macro(pointer *stk,  pointer *env, pointer *cnt, pointer *dmp)
{
  macro_closure(T_USRMACRO, stk, env, cnt, dmp);
}

void def_reader_macro(pointer ident, pointer value)
{
  pointer bind;
  SAVE(bind);
  bind = CONS(ident, value);
  renv = CONS(bind, renv);
  FREE(bind);
}

void prim_eval(pointer *stk, pointer *env, pointer *cnt, pointer *dmp)
{
  pointer arg;
  SAVE(arg);
  arg = CAAR(*stk);
  *stk = CDR(*stk);
  *cnt = CONS(arg, *cnt);
  // printf("Eval: "); print_cell(arg);
  FREE(arg);
}

void prim_print(pointer *stk, pointer *env, pointer *cnt, pointer *dmp)
{
  pointer args, ret;
  SAVE(args); SAVE(ret);
  for (args = CAR(*stk), ret = nil; args != nil; args = CDR(args))
    print_cell(ret = CAR(args));
  *stk = CONS(ret, CDR(*stk));
  FREE(args); FREE(ret);
}

void macro_quote(pointer *stk, pointer *env, pointer *cnt, pointer *dmp)
{
  pointer arg; SAVE(arg);
  arg = CAAR(*stk);
  *stk = CONS(arg, CDR(*stk));
  FREE(arg);
} 

void prim_define(pointer *stk, pointer *env, pointer *cnt, pointer *dmp)
{
  pointer ident, value, bind;
  SAVE(ident); SAVE(value); SAVE(bind);
  ident = CAAR(*stk);
  value = CADAR(*stk);
  bind  = CONS(ident, value);
  *stk = CONS(value, CDR(*stk));
  *env = CONS(bind, *env);
  FREE(ident); FREE(value); FREE(bind);
}

void prim_def_reader_macro(pointer *stk, pointer *env, pointer *cnt, pointer *dmp)
{
  pointer args;
  SAVE(args);
  args = CAR(*stk);
  def_reader_macro(CAR(args), CADR(args));
  *stk = CDR(*stk);
  FREE(args);
}

void prim_cons(pointer *stk, pointer *env, pointer *cnt, pointer *dmp)
{
  pointer args, tmp;
  SAVE(args); SAVE(tmp);
  args = CAR(*stk);
  tmp  = CONS(CAR(args), CADR(args));
  *stk = CONS(tmp, CDR(*stk));
  FREE(args); FREE(tmp);
}

void prim_car(pointer *stk, pointer *env, pointer *cnt, pointer *dmp)
{
  pointer arg;
  SAVE(arg);
  arg = CAAR(*stk);
  *stk = CONS(CAR(arg), CDR(*stk));
  FREE(arg);
}

void prim_cdr(pointer *stk, pointer *env, pointer *cnt, pointer *dmp)
{
  pointer arg;
  SAVE(arg);
  arg = CAAR(*stk);
  *stk = CONS(CDR(arg), CDR(*stk));
  FREE(arg);
}

void prim_gensym(pointer *stk, pointer *env, pointer *cnt, pointer *dmp)
{
  static int id = 0;
  char ident[MAX_LEN];
  pointer tmp;
  SAVE(tmp);
  sprintf(ident, "_G%03d", id++);
  tmp = make_ident(ident);
  *stk = CONS(tmp, CDR(*stk));
  FREE(tmp);
}

pointer set(pointer env, char *ident, pointer value)
{
  pointer p = env;
  while (!TYPE(p, T_NIL)) {
    if (strcmp(CAAR(p)->ident, ident) == 0)
      return (CDAR(p) = value);
    p = CDR(p);
  }
  error("Unbound variable: \"%s\"", ident);
}

void prim_set(pointer *stk, pointer *env, pointer *cnt, pointer *dmp)
{
  pointer args, ident, value, ret;
  SAVE(args); SAVE(ident); SAVE(value); SAVE(ret);
  args  = CAR(*stk);  
  ident = CAR(args);
  value = CADR(args);
  ret   = set(*env, ident->ident, value);
  *stk = CONS(ret, CDR(*stk));
  FREE(args); FREE(ident); FREE(value); FREE(ret);
}

void prim_begin(pointer *stk, pointer *env, pointer *cnt, pointer *dmp)
{
  pointer p;
  SAVE(p);
  p = CAR(*stk);
  if (!TYPE(p, T_NIL)) {
    while (!TYPE(CDR(p), T_NIL))
      p = CDR(p);
  }
  *stk = CONS(CAR(p), CDR(*stk));
  FREE(p);
}

void prim_if(pointer *stk, pointer *env, pointer *cnt, pointer *dmp)
{
  pointer args, cond_cell, true_cell, false_cell;
  SAVE(args); SAVE(cond_cell); SAVE(true_cell); SAVE(false_cell);
  args       = CAR(*stk);
  cond_cell  = CAR(args);
  true_cell  = CADR(args);
  false_cell = CADDR(args);
  assert(TYPE(cond_cell, T_DATA));
  *stk = CONS(cond_cell->data?true_cell:false_cell, CDR(*stk));
  FREE(args); FREE(cond_cell); FREE(true_cell); FREE(false_cell);
}

void prim_bin(pointer *stk, pointer *env, pointer *cnt, pointer *dmp, int (*op)(int, int))
{
  pointer args, n1, n2, tmp;
  SAVE(args); SAVE(n1); SAVE(n2); SAVE(tmp);
  args = CAR(*stk);
  n1   = CAR(args);
  n2   = CADR(args);
  assert(TYPE(n1, T_DATA) && TYPE(n2, T_DATA));
  tmp  = make_data(op(n1->data, n2->data));
  *stk = CONS(tmp, CDR(*stk));
  FREE(args); FREE(n1); FREE(n2); FREE(tmp);
}

#define DEFINE_ARITH_PRIMITIVE(name, op)                         \
int bin_##name(int x, int y) { return x op y; }                  \
void prim_##name(pointer *s, pointer *e, pointer *c, pointer *d) \
{                                                                \
  return prim_bin(s, e, c, d, bin_##name);                       \
}
DEFINE_ARITH_PRIMITIVE(add, +)
DEFINE_ARITH_PRIMITIVE(sub, -)
DEFINE_ARITH_PRIMITIVE(mul, *)
DEFINE_ARITH_PRIMITIVE(div, /)
DEFINE_ARITH_PRIMITIVE(mod, %)
DEFINE_ARITH_PRIMITIVE(eq, ==)
DEFINE_ARITH_PRIMITIVE(lt,  <)

void prim_eqp(pointer *stk, pointer *env, pointer *cnt, pointer *dmp)
{
  bool eq;
  pointer args, a1, a2, tmp;
  SAVE(args); SAVE(a1); SAVE(a2); SAVE(tmp);
  args = CAR(*stk);
  a1   = CAR(args);
  a2   = CADR(args);
  if (TYPE(a1, T_IDENT) && TYPE(a2, T_IDENT))
    eq = strcmp(a1->ident, a2->ident) == 0;
  else if (TYPE(a1, T_DATA) && TYPE(a2, T_DATA))
    eq = a1->data == a2->data;
  else
    eq = a1 == a2;
  tmp  = make_data(eq?1:0);
  *stk = CONS(tmp, CDR(*stk));
  FREE(args); FREE(a1); FREE(a2); FREE(tmp);
}

void prim_pairp(pointer *stk, pointer *env, pointer *cnt, pointer *dmp)
{
  pointer arg, tmp;
  SAVE(arg); SAVE(tmp);
  arg = CAAR(*stk);
  tmp = make_data(TYPE(arg, T_CONS)?1:0);
  *stk = CONS(tmp, CDR(*stk));
  FREE(arg); FREE(tmp);
}

pointer init_env()
{
  pointer env, ret;
  SAVE(env);
  env = nil;
  add_primitive(&env, T_MACRO, "lambda", macro_lambda);
  add_primitive(&env, T_MACRO, "macro", macro_macro);
  add_primitive(&env, T_PRIM, "eval", prim_eval);
  add_primitive(&env, T_PRIM, "print", prim_print);
  add_primitive(&env, T_MACRO, "quote", macro_quote);
  add_primitive(&env, T_PRIM, "_define", prim_define);
  add_primitive(&env, T_PRIM, "_def-reader-macro", prim_def_reader_macro);
  add_primitive(&env, T_PRIM, "cons", prim_cons);
  add_primitive(&env, T_PRIM, "car", prim_car);
  add_primitive(&env, T_PRIM, "cdr", prim_cdr);
  add_primitive(&env, T_PRIM, "gensym", prim_gensym);
  add_primitive(&env, T_PRIM, "_set!", prim_set);
  add_primitive(&env, T_PRIM, "begin", prim_begin);
  add_primitive(&env, T_PRIM, "_if", prim_if);
  add_primitive(&env, T_PRIM, "+", prim_add);
  add_primitive(&env, T_PRIM, "-", prim_sub);
  add_primitive(&env, T_PRIM, "*", prim_mul);
  add_primitive(&env, T_PRIM, "/", prim_div);
  add_primitive(&env, T_PRIM, "%", prim_mod);
  add_primitive(&env, T_PRIM, "=", prim_eq);
  add_primitive(&env, T_PRIM, "<", prim_lt);
  add_primitive(&env, T_PRIM, "eq?", prim_eqp);
  add_primitive(&env, T_PRIM, "pair?", prim_pairp);
  ret = env;
  FREE(env);
  return ret;
}

//============================================
// Transition rule of Exntended SECD Machine
//============================================
//  Case: T_STOP
//    < S, E, stop, D > =/=>
//
//    Remark: This means that whole evaluation of a s-exp is completed.
//            The final value is in the top of S.
//
//  Case: Value (T_NIL | T_DATA | T_CLOS | T_USRMACRO | T_PRIM | T_MACRO)
//    < S, E, (v . C), D >
//    => < (v . S), E, C, D >
//
//  Case: T_CONS
//    < S, E, ((e1 e2 ... en) . C), D >
//    => < S, E, (e1 c_back), (((e2 ... en) . C) . D)>
//    Remark: We focus on the computation of the head (i.e., e1),
//            because the remaining computation depends on two cases:
//            (1) e2 ... en will be evaluated if e1 reduces to a function;
//            (2) e2 ... en will not be evaluated if e1 reduces to a macro.
//
//  Case: T_BACK
//    Subcase: When the top of stack, v1, is #<primitive> or #<closure>.
//    < (v1 . S), E, (c_back . nil), (((e2 ... en) . C). D) >
//    => < (c_argend . S), E, (e2 .. en c_call C), D >   if v1 is #<primitive> or #<closure>
//    Remark: The arguments must be evaluated so that
//            we push e2 .. en to cnt-part.
//
//    Subcase: When the top of stack, v1, is #<macro> or #<usrmacro>.
//      < (v1 . S), E, (c_back . nil), (((e2 ... en) . C). D) >
//      => < (en .. e2 c_argend S), E, (c_call . C), D >   if v1 is #<macro> or #<usrmacro>
//    Remark: The arguments must *not* be evaluated so that
//            we push e2 .. en to stk-part immediately.
//
//  Case: T_CALL
//    Subcase: If u is #<primitive>
//      < (vn ... v1 c_argend u S), E, (c_call . C), D >
//      => < ((v1 ... vn) . S), E, C, D >
//      => ... the execution by #<primitive> ...
//      => < S', E', C', D' >
//      where S', E', C', and D' are the result of the primitive function call.
//
//    Subcase: If u is #<macro>
//      < (en ... e1 c_argend u S), E, (c_call . C), D >
//      => < ((e1 ... en) . S), E, C, D >
//      => ... the execution by #<macro> ...
//      => < S', E', C', D' >
//      where S', E', C', and D' are the result of the primitive macro call.
//
//    Subcase: If u is #<closure> (we omit the case of variadic functions)
//      Suppose that "u" is the colosure of (lambda (x1 ... xn) body) 
//      with E' which is its closure environment. Then,
//      < (vn ... v1 c_argend u S), E, (c_call . C), D >
//      => < nil, ( (x1 . v1) (x2 . v2) ... (xn . vn) E' ), (body c_ret), (S E C D)>
//
//    Subcase: If u is #<usrmacro> (we omit the case of variadic macros)
//      Suppose that "u" is the closure of (macro (x1 ... xn) body) 
//      with E' which is its closure environment. Then,
//      < (en ... e1 c_argend u S), E, (c_call . C), D >
//      => < nil, ( (x1 . e1) (x2 . e2) ... (xn . en) E' ), (body c_ret), ((c_argend #<primitive: eval> S) E (c_call . C) D)>
//
//  Case: T_RET
//    < (v . S), E, (c_ret . nil), (S' E' C' D')>
//    => < (v . S'), E', C', D' >
//============================================
void debug_output(pointer stk, pointer env, pointer cnt, pointer dmp)
{
#if DEBUG
  printf("stk: "); print_cell(stk);
  printf("cnt: "); print_cell(cnt);
  printf("dmp: "); print_cell(dmp);
  printf("env: "); print_cell(env);
  puts("----");
#endif
}

void op_value(pointer *stk, pointer *env, pointer *cnt, pointer *dmp)
{
  *stk = CONS(CAR(*cnt), *stk);
  *cnt = CDR(*cnt);
}

void op_ident(pointer *stk, pointer *env, pointer *cnt, pointer *dmp)
{
  *stk = CONS(lookup(*env, CAR(*cnt)->ident), *stk);
  *cnt = CDR(*cnt);
}

void op_cons(pointer *stk, pointer *env, pointer *cnt, pointer *dmp)
{
  pointer tmp;
  SAVE(tmp);
  tmp  = CONS(CDAR(*cnt), CDR(*cnt));
  *dmp = CONS(tmp, *dmp);
  *cnt = CONS3(CAAR(*cnt), c_back, nil);
  FREE(tmp);
}

void op_back(pointer *stk, pointer *env, pointer *cnt, pointer *dmp)
{
  pointer func, args, tail_cnt;
  SAVE(func); SAVE(args); SAVE(tail_cnt);
  func     = CAR(*stk);
  args     = CAAR(*dmp);
  tail_cnt = CDAR(*dmp);
  assert(TYPE(func, T_CLOS | T_MACRO | T_PRIM | T_USRMACRO));
  if (TYPE(func, T_PRIM | T_CLOS)) {
    pointer tmp; SAVE(tmp);
    *stk = CONS(c_argend, *stk);
    tmp  = CONS(c_call, tail_cnt);
    *cnt = append(args, tmp);
    FREE(tmp);
  } else if (TYPE(func, T_MACRO | T_USRMACRO)) {
    pointer tmp1, tmp2;
    SAVE(tmp1); SAVE(tmp2);
    tmp1 = reverse(args);
    tmp2 = CONS(c_argend, *stk);
    *stk = append(tmp1, tmp2);
    *cnt = CONS(c_call, tail_cnt);
    FREE(tmp1); FREE(tmp2);
  }
  *dmp = CDR(*dmp);
  FREE(func); FREE(args); FREE(tail_cnt);
}

pointer take_args(pointer *stk)
{
  pointer args, p;
  SAVE(args); SAVE(p);
  args = nil;
  for (;;) {
    p = CAR(*stk);
    *stk = CDR(*stk);
    if (TYPE(p, T_ARGEND)) break;
    args = CONS(p, args);
  }
  FREE(args); FREE(p);
  return args;
} 

void op_call(pointer *stk, pointer *env, pointer *cnt, pointer *dmp)
{
  pointer args, func;
  SAVE(args); SAVE(func);
  args = take_args(stk);
  func = CAR(*stk);
  *stk = CDR(*stk);
  *cnt = CDR(*cnt);
  assert(TYPE(func, T_PRIM | T_MACRO | T_CLOS | T_USRMACRO)); 
  if (TYPE(func, T_PRIM | T_MACRO)) {
    *stk = CONS(args, *stk);
    func->func(stk, env, cnt, dmp);
  } else if (TYPE(func, T_CLOS | T_USRMACRO)) {
    pointer cls_env, body; 
    SAVE(cls_env); SAVE(body);
    if (TYPE(func->params, T_IDENT)) {// If the closure is a variadic function/macro
      pointer bind; SAVE(bind);
      bind = CONS(func->params, args);
      cls_env = CONS(bind, func->env);
      FREE(bind);
    } else {
      pointer binds; SAVE(binds);
      binds = zip(func->params, args);
      cls_env = append(binds, func->env);
      FREE(binds);
    }
    body = CONS3(func->body, c_ret, nil);
    if (TYPE(func, T_CLOS)) {
      *dmp = CONS4(*stk, *env, *cnt, *dmp);
    } else if (TYPE(func, T_USRMACRO)) {
      pointer tmp1, tmp2;
      SAVE(tmp1); SAVE(tmp2);
      tmp1 = CONS3(c_argend, eval_cell, *stk);
      tmp2 = CONS(c_call, *cnt);
      *dmp = CONS4(tmp1, *env, tmp2, *dmp);
      FREE(tmp1); FREE(tmp2);
    }
    *stk = nil;
    *env = cls_env;
    *cnt = body;
    FREE(cls_env); FREE(body);
  }
  FREE(args); FREE(func);
}

void op_ret(pointer *stk, pointer *env, pointer *cnt, pointer *dmp)
{
  *stk = CONS(CAR(*stk), CAR(*dmp));
  *env = CADR(*dmp);
  *cnt = CADDR(*dmp);
  *dmp = CDDDR(*dmp);
}

void put_prompt() {
  printf("* ");
}

void run()
{
  pointer stk, env, cnt, dmp;
  pointer p;
    
  SAVE(stk); SAVE(env); SAVE(cnt); SAVE(dmp);
  SAVE(renv); SAVE(p);

  memory    = mp = alloc_memory();
  renv      = nil;
  env       = init_env();
  eval_cell = lookup(env, "eval");
  while (put_prompt(), (p = parse_cell()) != NULL) {
    stk = nil;
    cnt = CONS3(p, c_stop, nil);
    dmp = nil;
    do {
      debug_output(stk, env, cnt, dmp);
      if (TYPE(p, T_NIL | T_DATA | T_CLOS | T_USRMACRO | T_PRIM | T_MACRO))
        op_value(&stk, &env, &cnt, &dmp);
      else if (TYPE(p, T_IDENT))
        op_ident(&stk, &env, &cnt, &dmp);
      else if (TYPE(p, T_CONS))
        op_cons(&stk, &env, &cnt, &dmp);
      else if (TYPE(p, T_BACK))
        op_back(&stk, &env, &cnt, &dmp);
      else if (TYPE(p, T_CALL))
        op_call(&stk, &env, &cnt, &dmp);
      else if (TYPE(p, T_RET))
        op_ret(&stk, &env, &cnt, &dmp);
      else
        error("Undefined execution: %d", p->type);
    } while (!TYPE((p = CAR(cnt)), T_STOP));
    print_cell(CAR(stk));
    assert(TYPE(dmp, T_NIL));
    debug_output(stk, env, cnt, dmp);
  }

  FREE(stk); FREE(env); FREE(cnt); FREE(dmp);
  FREE(renv); FREE(p);
}

int main(int argc, char **argv)
{
  run();
  return 0;
}
