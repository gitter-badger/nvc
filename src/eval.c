//
//  Copyright (C) 2013-2016  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "phase.h"
#include "util.h"
#include "common.h"
#include "vcode.h"

#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <inttypes.h>
#include <math.h>
#include <float.h>

#define MAX_DIMS  4
#define EVAL_HEAP (4 * 1024)

typedef enum {
   VALUE_INVALID,
   VALUE_REAL,
   VALUE_INTEGER,
   VALUE_POINTER,
   VALUE_UARRAY,
   VALUE_CARRAY
} value_kind_t;

typedef struct value value_t;
typedef struct context context_t;

typedef struct {
   struct {
      int64_t      left;
      int64_t      right;
      range_kind_t dir;
   } dim[MAX_DIMS];
   int      ndims;
   value_t *data;
} uarray_t;

struct value {
   value_kind_t kind;
   union {
      double    real;
      int64_t   integer;
      value_t  *pointer;
      uarray_t *uarray;
   };
};

struct context {
   context_t *parent;
   value_t   *regs;
   value_t   *vars;
};

typedef struct {
   context_t   *context;
   int          result;
   tree_t       fcall;
   eval_flags_t flags;
   bool         failed;
   void        *heap;
   size_t       halloc;
} eval_state_t;

#define EVAL_WARN(t, ...) do {                                          \
      if (state->flags & EVAL_WARN)                                     \
         warn_at(tree_loc(t), __VA_ARGS__);                             \
   } while (0)

static int errors = 0;

static void eval_vcode(eval_state_t *state);

static bool eval_possible(tree_t t, eval_flags_t flags)
{
   switch (tree_kind(t)) {
   case T_FCALL:
      {
         if (tree_flags(tree_ref(t)) & TREE_F_IMPURE)
            return false;

         const int nparams = tree_params(t);
         for (int i = 0; i < nparams; i++) {
            tree_t p = tree_value(tree_param(t, i));
            const bool fcall = tree_kind(p) == T_FCALL;
            if ((flags & EVAL_FOLDING) && fcall && type_is_scalar(tree_type(p)))
               return false;   // Would have been folded already if possible
            else if (fcall && !(flags & EVAL_FCALL))
               return false;
            else if (!eval_possible(p, flags))
               return false;
         }

         return true;
      }

   case T_LITERAL:
      return true;

   case T_TYPE_CONV:
      return eval_possible(tree_value(tree_param(t, 0)), flags);

   case T_REF:
      {
         tree_t decl = tree_ref(t);
         switch (tree_kind(decl)) {
         case T_UNIT_DECL:
         case T_ENUM_LIT:
            return true;

         case T_CONST_DECL:
            return eval_possible(tree_value(decl), flags);

         default:
            return false;
         }
      }

   default:
      if (flags & EVAL_WARN)
         warn_at(tree_loc(t), "expression prevents constant folding");
      return false;
   }
}

static void *eval_alloc(size_t nbytes, eval_state_t *state)
{
   if (state->halloc + nbytes > EVAL_HEAP) {
      EVAL_WARN(state->fcall, "evaluation heap exhaustion prevents "
                "constant folding (%zu allocated, %zu requested)",
                state->halloc, nbytes);
      state->failed = true;
      return NULL;
   }

   if (state->heap == NULL)
      state->heap = xmalloc(EVAL_HEAP);

   void *ptr = (char *)state->heap + state->halloc;
   state->halloc += nbytes;
   return ptr;
}

static context_t *eval_new_context(eval_state_t *state)
{
   const int nregs = vcode_count_regs();
   const int nvars = vcode_count_vars();

   void *mem = xcalloc(sizeof(context_t) + sizeof(value_t) * (nregs + nvars));

   context_t *context = mem;
   context->regs = (value_t *)((uint8_t *)mem + sizeof(context_t));
   context->vars = (value_t *)
      ((uint8_t *)mem + sizeof(context_t) + (nregs * sizeof(value_t)));

   for (int i = 0; i < nvars; i++) {
      vcode_var_t var = vcode_var_handle(i);
      vcode_type_t type = vcode_var_type(var);
      switch (vtype_kind(type)) {
      case VCODE_TYPE_CARRAY:
         context->vars[i].kind = VALUE_CARRAY;
         context->vars[i].pointer =
            eval_alloc(sizeof(value_t) * vtype_size(type), state);
         break;

      case VCODE_TYPE_INT:
         context->vars[i].kind = VALUE_INTEGER;
         context->vars[i].integer = 0;
         break;

      case VCODE_TYPE_REAL:
         context->vars[i].kind = VALUE_REAL;
         context->vars[i].real = 0;
         break;

      case VCODE_TYPE_UARRAY:
         context->vars[i].kind = VALUE_UARRAY;
         context->vars[i].uarray = NULL;
         break;

      default:
         fatal_at(tree_loc(state->fcall), "cannot evaluate variables with "
                  "type %d", vtype_kind(type));
      }
   }

   return context;
}

static void eval_free_context(context_t *context)
{
   if (context->parent)
      eval_free_context(context->parent);

   context->regs = NULL;
   context->vars = NULL;
   free(context);
}

static value_t *eval_get_reg(vcode_reg_t reg, eval_state_t *state)
{
   return &(state->context->regs[reg]);
}

static value_t *eval_get_var(vcode_var_t var, eval_state_t *state)
{
   if (vcode_var_extern(var)) {
      state->failed = true;
      return NULL;
   }

   const int var_depth = vcode_var_context(var);

   context_t *context = state->context;
   for (int depth = vcode_unit_depth(); depth > var_depth; depth--) {
      if (context->parent == NULL) {
         assert(vcode_unit_kind() != VCODE_UNIT_THUNK);

         vcode_state_t vcode_state;
         vcode_state_save(&vcode_state);

         vcode_select_unit(vcode_unit_context());
         assert(vcode_unit_kind() == VCODE_UNIT_CONTEXT);
         vcode_select_block(0);

         context_t *new_context = eval_new_context(state);
         context->parent = new_context;

         eval_state_t new_state = {
            .context = new_context,
            .result  = -1,
            .fcall   = state->fcall,
            .failed  = false,
            .flags   = state->flags | EVAL_BOUNDS,
            .heap    = state->heap,
            .halloc  = state->halloc
         };

         eval_vcode(&new_state);
         vcode_state_restore(&vcode_state);

         state->heap = new_state.heap;
         state->halloc = new_state.halloc;

         if (new_state.failed) {
            state->failed = true;
            return NULL;
         }
      }

      context = context->parent;
   }

   return &(context->vars[vcode_var_index(var)]);
}

static int eval_value_cmp(value_t *lhs, value_t *rhs)
{
   assert(lhs->kind == rhs->kind);
   switch (lhs->kind) {
   case VALUE_INTEGER:
      return lhs->integer - rhs->integer;

   case VALUE_REAL:
      {
         const double diff = lhs->real - rhs->real;
         if (diff < 0.0)
            return -1;
         else if (diff > 0.0)
            return 1;
         else
            return 0;
      }

   case VALUE_POINTER:
      return lhs->pointer - rhs->pointer;

   default:
      fatal_trace("invalid value type %d in %s", lhs->kind, __func__);
   }
}

static void eval_message(value_t *text, value_t *length, value_t *severity,
                         const loc_t *loc, const char *prefix)
{
   const char *levels[] = {
      "Note", "Warning", "Error", "Failure"
   };

   assert(text->kind == VALUE_POINTER);

   char *copy = NULL ;
   if (length->integer > 0) {
      copy = xmalloc(length->integer + 1);
      for (int i = 0; i < length->integer; i++)
         copy[i] = text->pointer[i].integer;
      copy[length->integer] = '\0';
   }

   void (*fn)(const loc_t *loc, const char *fmt, ...) = fatal_at;

   switch (severity->integer) {
   case SEVERITY_NOTE:    fn = note_at; break;
   case SEVERITY_WARNING: fn = warn_at; break;
   case SEVERITY_ERROR:
   case SEVERITY_FAILURE: fn = error_at; break;
   }

   (*fn)(loc, "%s %s: %s", prefix, levels[severity->integer],
         copy ?: "Assertion violation");
   free(copy);
}

static void eval_op_const(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   dst->kind    = VALUE_INTEGER;
   dst->integer = vcode_get_value(op);
}

static void eval_op_const_real(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   dst->kind = VALUE_REAL;
   dst->real = vcode_get_real(op);
}

static void eval_op_return(int op, eval_state_t *state)
{
   if (vcode_count_args(op) > 0)
      state->result = vcode_get_arg(op, 0);
}

static void eval_op_not(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   value_t *src = eval_get_reg(vcode_get_arg(op, 0), state);
   dst->kind    = VALUE_INTEGER;
   dst->integer = !(src->integer);
}

static void eval_op_add(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   value_t *lhs = eval_get_reg(vcode_get_arg(op, 0), state);
   value_t *rhs = eval_get_reg(vcode_get_arg(op, 1), state);

   switch (lhs->kind) {
   case VALUE_INTEGER:
      dst->kind    = VALUE_INTEGER;
      dst->integer = lhs->integer + rhs->integer;
      break;

   case VALUE_REAL:
      dst->kind = VALUE_REAL;
      dst->real = lhs->real + rhs->real;
      break;

   case VALUE_POINTER:
      assert(rhs->kind == VALUE_INTEGER);
      dst->kind = VALUE_POINTER;
      dst->pointer = lhs->pointer + rhs->integer;
      break;

   default:
      fatal_trace("invalid value type in %s", __func__);
   }
}

static void eval_op_sub(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   value_t *lhs = eval_get_reg(vcode_get_arg(op, 0), state);
   value_t *rhs = eval_get_reg(vcode_get_arg(op, 1), state);

   switch (lhs->kind) {
   case VALUE_INTEGER:
      dst->kind    = VALUE_INTEGER;
      dst->integer = lhs->integer - rhs->integer;
      break;

   case VALUE_REAL:
      dst->kind = VALUE_REAL;
      dst->real = lhs->real - rhs->real;
      break;

   default:
      fatal_trace("invalid value type in %s", __func__);
   }
}

static void eval_op_mul(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   value_t *lhs = eval_get_reg(vcode_get_arg(op, 0), state);
   value_t *rhs = eval_get_reg(vcode_get_arg(op, 1), state);

   switch (lhs->kind) {
   case VALUE_INTEGER:
      dst->kind    = VALUE_INTEGER;
      dst->integer = lhs->integer * rhs->integer;
      break;

   case VALUE_REAL:
      dst->kind = VALUE_REAL;
      dst->real = lhs->real * rhs->real;
      break;

   default:
      fatal_trace("invalid value type in %s", __func__);
   }
}

static void eval_op_div(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   value_t *lhs = eval_get_reg(vcode_get_arg(op, 0), state);
   value_t *rhs = eval_get_reg(vcode_get_arg(op, 1), state);

   switch (lhs->kind) {
   case VALUE_INTEGER:
      if (rhs->integer == 0)
         fatal_at(tree_loc(state->fcall), "division by zero");
      else {
         dst->kind    = VALUE_INTEGER;
         dst->integer = lhs->integer / rhs->integer;
      }
      break;

   case VALUE_REAL:
      dst->kind = VALUE_REAL;
      dst->real = lhs->real / rhs->real;
      break;

   default:
      fatal_trace("invalid value type in %s", __func__);
   }
}

static void eval_op_mod(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   value_t *lhs = eval_get_reg(vcode_get_arg(op, 0), state);
   value_t *rhs = eval_get_reg(vcode_get_arg(op, 1), state);

   switch (lhs->kind) {
   case VALUE_INTEGER:
      if (rhs->integer == 0)
         fatal_at(tree_loc(state->fcall), "division by zero");
      else {
         dst->kind    = VALUE_INTEGER;
         dst->integer = labs(lhs->integer % rhs->integer);
      }
      break;

   default:
      fatal_trace("invalid value type in %s", __func__);
   }
}

static void eval_op_rem(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   value_t *lhs = eval_get_reg(vcode_get_arg(op, 0), state);
   value_t *rhs = eval_get_reg(vcode_get_arg(op, 1), state);

   switch (lhs->kind) {
   case VALUE_INTEGER:
      if (rhs->integer == 0)
         fatal_at(tree_loc(state->fcall), "division by zero");
      else {
         dst->kind    = VALUE_INTEGER;
         dst->integer =
            lhs->integer - (lhs->integer / rhs->integer) * rhs->integer;
      }
      break;

   default:
      fatal_trace("invalid value type in %s", __func__);
   }
}

static void eval_op_exp(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   value_t *lhs = eval_get_reg(vcode_get_arg(op, 0), state);
   value_t *rhs = eval_get_reg(vcode_get_arg(op, 1), state);

   dst->kind = VALUE_REAL;
   dst->real = pow(lhs->real, rhs->real);
}

static void eval_op_cmp(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   value_t *lhs = eval_get_reg(vcode_get_arg(op, 0), state);
   value_t *rhs = eval_get_reg(vcode_get_arg(op, 1), state);

   dst->kind = VALUE_INTEGER;

   switch (vcode_get_cmp(op)) {
   case VCODE_CMP_EQ:
      dst->integer = eval_value_cmp(lhs, rhs) == 0;
      break;
   case VCODE_CMP_NEQ:
      dst->integer = eval_value_cmp(lhs, rhs) != 0;
      break;
   case VCODE_CMP_GT:
      dst->integer = eval_value_cmp(lhs, rhs) > 0;
      break;
   case VCODE_CMP_GEQ:
      dst->integer = eval_value_cmp(lhs, rhs) >= 0;
      break;
   case VCODE_CMP_LT:
      dst->integer = eval_value_cmp(lhs, rhs) < 0;
      break;
   case VCODE_CMP_LEQ:
      dst->integer = eval_value_cmp(lhs, rhs) <= 0;
      break;
   default:
      vcode_dump();
      fatal_trace("cannot handle comparison");
   }
}

static void eval_op_cast(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   value_t *src = eval_get_reg(vcode_get_arg(op, 0), state);

   switch (vtype_kind(vcode_get_type(op))) {
   case VCODE_TYPE_INT:
   case VCODE_TYPE_OFFSET:
      dst->kind = VALUE_INTEGER;
      switch (src->kind) {
      case VALUE_INTEGER: dst->integer = src->integer; break;
      case VALUE_REAL: dst->integer = (int64_t)src->real; break;
      default: break;
      }
      break;

   case VCODE_TYPE_REAL:
      dst->kind = VALUE_REAL;
      switch (src->kind) {
      case VALUE_INTEGER: dst->real = (double)src->integer; break;
      case VALUE_REAL: dst->real = src->real; break;
      default: break;
      }
      break;

   default:
      vcode_dump();
      fatal("cannot handle destination type in cast");
   }
}

static void eval_op_neg(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   value_t *src = eval_get_reg(vcode_get_arg(op, 0), state);

   switch (src->kind) {
   case VALUE_INTEGER:
      dst->kind    = VALUE_INTEGER;
      dst->integer = -(src->integer);
      break;

   case VALUE_REAL:
      dst->kind = VALUE_REAL;
      dst->real = -(src->real);
      break;

   default:
      fatal_trace("invalid value type in %s", __func__);
   }
}

static void eval_op_abs(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   value_t *src = eval_get_reg(vcode_get_arg(op, 0), state);

   switch (src->kind) {
   case VALUE_INTEGER:
      dst->kind    = VALUE_INTEGER;
      dst->integer = llabs(src->integer);
      break;

   case VALUE_REAL:
      dst->kind = VALUE_REAL;
      dst->real = fabs(src->real);
      break;

   default:
      fatal_trace("invalid value type in %s", __func__);
   }
}

static void eval_load_vcode(lib_t lib, tree_t unit, tree_rd_ctx_t tree_ctx,
                            eval_state_t *state)
{
   if (tree_has_code(unit))
      return;

   ident_t unit_name = tree_ident(unit);

   if (state->flags & EVAL_VERBOSE)
      notef("loading vcode for %s", istr(unit_name));

   char *name LOCAL = xasprintf("_%s.vcode", istr(unit_name));
   fbuf_t *f = lib_fbuf_open(lib, name, FBUF_IN);
   if (f == NULL) {
      EVAL_WARN(state->fcall, "cannot load vcode for %s", istr(unit_name));
      return;
   }

   vcode_read(f, tree_ctx);
   fbuf_close(f);
}

static void eval_op_fcall(int op, eval_state_t *state)
{
   vcode_state_t vcode_state;
   vcode_state_save(&vcode_state);

   ident_t func_name = vcode_get_func(op);
   vcode_unit_t vcode = vcode_find_unit(func_name);

   const int nparams = vcode_count_args(op);
   value_t *params[nparams];
   for (int i = 0; i < nparams; i++)
      params[i] = eval_get_reg(vcode_get_arg(op, i), state);

   if (vcode == NULL) {
      ident_t unit_name = ident_runtil(vcode_get_func(op), '.');
      ident_t lib_name = ident_until(unit_name, '.');

      lib_t lib;
      if (lib_name != unit_name && (lib = lib_find(lib_name, false)) != NULL) {
         tree_rd_ctx_t tree_ctx;
         tree_t unit = lib_get_ctx(lib, unit_name, &tree_ctx);
         if (unit != NULL) {
            eval_load_vcode(lib, unit, tree_ctx, state);

            if (tree_kind(unit) == T_PACKAGE) {
               ident_t body_name =
                  ident_prefix(unit_name, ident_new("body"), '-');
               tree_t body = lib_get_ctx(lib, body_name, &tree_ctx);
               if (body != NULL)
                  eval_load_vcode(lib, body, tree_ctx, state);
            }

            vcode = vcode_find_unit(func_name);
         }
      }
   }

   if (vcode == NULL) {
      EVAL_WARN(state->fcall, "function call to %s prevents "
                "constant folding", istr(func_name));
      state->failed = true;
      vcode_state_restore(&vcode_state);
      return;
   }

   vcode_select_unit(vcode);
   vcode_select_block(0);

   context_t *context = eval_new_context(state);

   for (int i = 0; i < nparams; i++)
      context->regs[i] = *params[i];

   eval_state_t new = {
      .context = context,
      .result  = -1,
      .fcall   = state->fcall,
      .failed  = false,
      .flags   = state->flags | EVAL_BOUNDS,
      .heap    = state->heap,
      .halloc  = state->halloc
   };

   eval_vcode(&new);
   vcode_state_restore(&vcode_state);

   state->heap = new.heap;
   state->halloc = new.halloc;

   if (new.failed)
      state->failed = true;
   else {
      assert(new.result != -1);
      value_t *dst = eval_get_reg(vcode_get_result(op), state);
      *dst = context->regs[new.result];

      if (state->flags & EVAL_VERBOSE) {
         const char *name = istr(vcode_get_func(op));
         const char *nest = istr(tree_ident(state->fcall));
         if (context->regs[new.result].kind == VALUE_INTEGER)
            notef("%s (in %s) returned %"PRIi64, name, nest,
                  context->regs[new.result].integer);
         else
            notef("%s (in %s) returned %lf", name, nest,
                  context->regs[new.result].real);
      }
   }

   eval_free_context(context);
}

static void eval_op_bounds(int op, eval_state_t *state)
{
   value_t *reg = eval_get_reg(vcode_get_arg(op, 0), state);
   vcode_type_t bounds = vcode_get_type(op);

   switch (reg->kind) {
   case VALUE_INTEGER:
      {
         const int64_t low  = vtype_low(bounds);
         const int64_t high = vtype_high(bounds);
         if (low > high)
            break;
         else if (reg->integer < low || reg->integer > high) {
            if (state->flags & EVAL_BOUNDS) {
               const loc_t *loc = tree_loc(vcode_get_bookmark(op).tree);

               switch ((bounds_kind_t)vcode_get_subkind(op)) {
               case BOUNDS_ARRAY_TO:
                  error_at(loc, "array index %"PRIi64" outside bounds %"PRIi64
                           " to %"PRIi64, reg->integer, low, high);
                  break;

               case BOUNDS_ARRAY_DOWNTO:
                  error_at(loc, "array index %"PRIi64" outside bounds %"PRIi64
                           " downto %"PRIi64, reg->integer, high, low);
                  break;

               default:
                  fatal_trace("unhandled bounds kind %d in %s",
                              vcode_get_subkind(op), __func__);
               }

               errors++;
               note_at(tree_loc(state->fcall), "while evaluating call to %s",
                       istr(tree_ident(state->fcall)));
            }
            state->failed = true;
         }
      }
      break;

   case VALUE_REAL:
      break;

   default:
      fatal_trace("invalid value type in %s", __func__);
   }
}

static void eval_op_dynamic_bounds(int op, eval_state_t *state)
{
   value_t *reg = eval_get_reg(vcode_get_arg(op, 0), state);
   value_t *low = eval_get_reg(vcode_get_arg(op, 1), state);
   value_t *high = eval_get_reg(vcode_get_arg(op, 2), state);

   switch (reg->kind) {
   case VALUE_INTEGER:
      {
         if (low->integer > high->integer)
            break;
         else if (reg->integer < low->integer || reg->integer > high->integer)
            state->failed = true;
      }
      break;

   case VALUE_REAL:
      break;

   default:
      fatal_trace("invalid value type in %s", __func__);
   }
}

static void eval_op_const_array(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);

   const int nargs = vcode_count_args(op);

   dst->kind = VALUE_POINTER;
   if ((dst->pointer = eval_alloc(sizeof(value_t) * nargs, state))) {
      for (int i = 0; i < nargs; i++)
         dst->pointer[i] = *eval_get_reg(vcode_get_arg(op, i), state);
   }
}

static void eval_op_wrap(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   value_t *src = eval_get_reg(vcode_get_arg(op, 0), state);

   assert(src->kind == VALUE_POINTER);

   dst->kind = VALUE_UARRAY;
   if ((dst->uarray = eval_alloc(sizeof(uarray_t), state)) == NULL)
      return;
   dst->uarray->data = src->pointer;

   const int ndims = (vcode_count_args(op) - 1) / 3;
   if (ndims > MAX_DIMS) {
      state->failed = true;
      EVAL_WARN(state->fcall, "%d dimensional array prevents "
                "constant folding", ndims);
   }
   else {
      dst->uarray->ndims = ndims;
      for (int i = 0; i < ndims; i++) {
         dst->uarray->dim[i].left =
            eval_get_reg(vcode_get_arg(op, (i * 3) + 1), state)->integer;
         dst->uarray->dim[i].right =
            eval_get_reg(vcode_get_arg(op, (i * 3) + 2), state)->integer;
         dst->uarray->dim[i].dir =
            eval_get_reg(vcode_get_arg(op, (i * 3) + 3), state)->integer;
      }
   }
}

static void eval_op_store(int op, eval_state_t *state)
{
   value_t *src = eval_get_reg(vcode_get_arg(op, 0), state);
   value_t *var = eval_get_var(vcode_get_address(op), state);

   if (var != NULL)
      *var = *src;
}

static void eval_op_load(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   value_t *var = eval_get_var(vcode_get_address(op), state);

   if (var != NULL)
      *dst = *var;
}

static void eval_op_unwrap(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   value_t *src = eval_get_reg(vcode_get_arg(op, 0), state);

   dst->kind    = VALUE_POINTER;
   dst->pointer = src->uarray->data;
}

static void eval_op_uarray_len(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   value_t *src = eval_get_reg(vcode_get_arg(op, 0), state);

   const int dim = vcode_get_dim(op);
   const int64_t left = src->uarray->dim[dim].left;
   const int64_t right = src->uarray->dim[dim].right;
   const range_kind_t dir = src->uarray->dim[dim].dir;

   const int64_t len = (dir == RANGE_TO ? right - left : left - right) + 1;

   dst->kind    = VALUE_INTEGER;
   dst->integer = MAX(len, 0);
}

static void eval_op_uarray_dir(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   value_t *src = eval_get_reg(vcode_get_arg(op, 0), state);

   const int dim = vcode_get_dim(op);
   const range_kind_t dir = src->uarray->dim[dim].dir;

   dst->kind    = VALUE_INTEGER;
   dst->integer = dir;
}

static void eval_op_memcmp(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   value_t *lhs = eval_get_reg(vcode_get_arg(op, 0), state);
   value_t *rhs = eval_get_reg(vcode_get_arg(op, 1), state);
   value_t *len = eval_get_reg(vcode_get_arg(op, 2), state);

   dst->kind    = VALUE_INTEGER;
   dst->integer = 1;

   assert(lhs->kind == VALUE_POINTER);
   assert(rhs->kind == VALUE_POINTER);

   for (int i = 0; i < len->integer; i++) {
      if (eval_value_cmp(&(lhs->pointer[i]), &(rhs->pointer[i]))) {
         dst->integer = 0;
         return;
      }
   }
}

static void eval_op_and(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   value_t *lhs = eval_get_reg(vcode_get_arg(op, 0), state);
   value_t *rhs = eval_get_reg(vcode_get_arg(op, 1), state);

   switch (lhs->kind) {
   case VALUE_INTEGER:
      dst->kind    = VALUE_INTEGER;
      dst->integer = lhs->integer & rhs->integer;
      break;

   default:
      fatal_trace("invalid value type in %s", __func__);
   }
}

static void eval_op_or(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   value_t *lhs = eval_get_reg(vcode_get_arg(op, 0), state);
   value_t *rhs = eval_get_reg(vcode_get_arg(op, 1), state);

   switch (lhs->kind) {
   case VALUE_INTEGER:
      dst->kind    = VALUE_INTEGER;
      dst->integer = lhs->integer | rhs->integer;
      break;

   default:
      fatal_trace("invalid value type in %s", __func__);
   }
}

static void eval_op_jump(int op, eval_state_t *state)
{
   vcode_select_block(vcode_get_target(op, 0));
   eval_vcode(state);
}

static void eval_op_cond(int op, eval_state_t *state)
{
   value_t *test = eval_get_reg(vcode_get_arg(op, 0), state);

   const vcode_block_t next = vcode_get_target(op, !(test->integer));
   vcode_select_block(next);
   eval_vcode(state);
}

static void eval_op_undefined(int op, eval_state_t *state)
{
   EVAL_WARN(state->fcall, "reference to object without defined "
             "value in this phase prevents constant folding");

   state->failed = true;
}

static void eval_op_nested_fcall(int op, eval_state_t *state)
{
   // TODO
   state->failed = true;
}

static void eval_op_index(int op, eval_state_t *state)
{
   value_t *value = eval_get_var(vcode_get_address(op), state);
   if (value == NULL)
      return;

   assert(value->kind == VALUE_CARRAY);

   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   dst->kind = VALUE_POINTER;
   dst->pointer = value->pointer;
}

static void eval_op_load_indirect(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   value_t *src = eval_get_reg(vcode_get_arg(op, 0), state);

   assert(src->kind == VALUE_POINTER);
   *dst = *(src->pointer);
}

static void eval_op_store_indirect(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_arg(op, 1), state);
   value_t *src = eval_get_reg(vcode_get_arg(op, 0), state);

   assert(dst->kind == VALUE_POINTER);
   *(dst->pointer) = *src;
}

static void eval_op_case(int op, eval_state_t *state)
{
   value_t *test = eval_get_reg(vcode_get_arg(op, 0), state);
   vcode_block_t target = vcode_get_target(op, 0);

   const int num_args = vcode_count_args(op);
   for (int i = 1; i < num_args; i++) {
      value_t *cmp = eval_get_reg(vcode_get_arg(op, i), state);
      if (eval_value_cmp(test, cmp) == 0) {
         target = vcode_get_target(op, i);
         break;
      }
   }

   vcode_select_block(target);
   eval_vcode(state);
}

static void eval_op_copy(int op, eval_state_t *state)
{
   value_t *dst = eval_get_reg(vcode_get_arg(op, 0), state);
   value_t *src = eval_get_reg(vcode_get_arg(op, 1), state);
   value_t *count = eval_get_reg(vcode_get_arg(op, 2), state);

   assert(dst->kind = VALUE_POINTER);
   assert(src->kind = VALUE_POINTER);
   assert(count->kind = VALUE_INTEGER);

   for (int i = 0; i < count->integer; i++)
      dst->pointer[i] = src->pointer[i];
}

static void eval_op_report(int op, eval_state_t *state)
{
   value_t *text = eval_get_reg(vcode_get_arg(op, 1), state);
   value_t *length = eval_get_reg(vcode_get_arg(op, 2), state);
   value_t *severity = eval_get_reg(vcode_get_arg(op, 0), state);

   if (state->flags & EVAL_REPORT)
      eval_message(text, length, severity,
                   tree_loc(vcode_get_bookmark(op).tree), "Report");
   else
      state->failed = true;  // Cannot fold as would change runtime behaviour
}

static void eval_op_assert(int op, eval_state_t *state)
{
   value_t *test = eval_get_reg(vcode_get_arg(op, 0), state);
   value_t *text = eval_get_reg(vcode_get_arg(op, 2), state);
   value_t *length = eval_get_reg(vcode_get_arg(op, 3), state);
   value_t *severity = eval_get_reg(vcode_get_arg(op, 1), state);

   if (test->integer == 0) {
      if (state->flags & EVAL_REPORT)
         eval_message(text, length, severity,
                      tree_loc(vcode_get_bookmark(op).tree), "Assertion");
      state->failed = severity->integer >= SEVERITY_ERROR;
   }
}

static void eval_op_select(int op, eval_state_t *state)
{
   value_t *test = eval_get_reg(vcode_get_arg(op, 0), state);
   value_t *left = eval_get_reg(vcode_get_arg(op, 1), state);
   value_t *right = eval_get_reg(vcode_get_arg(op, 2), state);
   value_t *result = eval_get_reg(vcode_get_result(op), state);

   assert(test->kind == VALUE_INTEGER);

   *result = test->integer ? *left : *right;
}

static void eval_op_alloca(int op, eval_state_t *state)
{
   value_t *result = eval_get_reg(vcode_get_result(op), state);

   int length = 1;
   if (vcode_count_args(op) > 0) {
      value_t *length_reg = eval_get_reg(vcode_get_arg(op, 0), state);
      assert(length_reg->kind == VALUE_INTEGER);
      length = length_reg->integer;
   }

   result->kind = VALUE_POINTER;
   result->pointer = eval_alloc(sizeof(value_t) * length, state);
}

static void eval_op_index_check(int op, eval_state_t *state)
{
   value_t *low = eval_get_reg(vcode_get_arg(op, 0), state);
   value_t *high = eval_get_reg(vcode_get_arg(op, 1), state);

   int64_t min, max;
   if (vcode_count_args(op) == 2) {
      vcode_type_t bounds = vcode_get_type(op);
      min = vtype_low(bounds);
      max = vtype_high(bounds);
   }
   else {
      min = eval_get_reg(vcode_get_arg(op, 2), state)->integer;
      max = eval_get_reg(vcode_get_arg(op, 3), state)->integer;
   }

   if (high->integer < low->integer)
      return;
   else if (low->integer < min)
      state->failed = true;    // TODO: report error here if EVAL_BOUNDS
   else if (high->integer > max)
      state->failed = true;
}

static void eval_op_image(int op, eval_state_t *state)
{
   value_t *object = eval_get_reg(vcode_get_arg(op, 0), state);
   tree_t where = vcode_get_bookmark(op).tree;
   type_t type = type_base_recur(tree_type(where));

   char buf[32];
   size_t len = 0;

   switch (type_kind(type)) {
   case T_INTEGER:
      len = snprintf(buf, sizeof(buf), "%"PRIi64, object->integer);
      break;

   case T_ENUM:
      {
         tree_t lit = type_enum_literal(type, object->integer);
         len = snprintf(buf, sizeof(buf), "%s", istr(tree_ident(lit)));
      }
      break;

   case T_REAL:
      len = snprintf(buf, sizeof(buf), "%.*g", DBL_DIG + 3, object->real);
      break;

   case T_PHYSICAL:
      {
         tree_t unit = type_unit(type, 0);
         len = snprintf(buf, sizeof(buf), "%"PRIi64" %s", object->integer,
                        istr(tree_ident(unit)));
      }
      break;

   default:
      error_at(tree_loc(where), "cannot use 'IMAGE with this type");
      state->failed = true;
      return;
   }

   value_t *dst = eval_get_reg(vcode_get_result(op), state);
   if ((dst->uarray = eval_alloc(sizeof(uarray_t), state)) == NULL)
      return;
   if ((dst->uarray->data = eval_alloc(sizeof(value_t) * len, state)) == NULL)
      return;

   dst->uarray->ndims = 1;
   dst->uarray->dim[0].left  = 1;
   dst->uarray->dim[0].right = len;
   dst->uarray->dim[0].dir   = RANGE_TO;

   for (size_t i = 0; i < len; i++) {
      value_t *ch = &(dst->uarray->data[i]);
      ch->kind = VALUE_INTEGER;
      ch->integer = buf[i];
   }
}

static void eval_op_uarray_left(int op, eval_state_t *state)
{
   value_t *array = eval_get_reg(vcode_get_arg(op, 0), state);
   value_t *dst = eval_get_reg(vcode_get_result(op), state);

   assert(array->kind == VALUE_UARRAY);

   dst->kind = VALUE_INTEGER;
   dst->integer = array->uarray->dim[vcode_get_dim(op)].left;
}

static void eval_op_uarray_right(int op, eval_state_t *state)
{
   value_t *array = eval_get_reg(vcode_get_arg(op, 0), state);
   value_t *dst = eval_get_reg(vcode_get_result(op), state);

   assert(array->kind == VALUE_UARRAY);

   dst->kind = VALUE_INTEGER;
   dst->integer = array->uarray->dim[vcode_get_dim(op)].right;
}

static void eval_vcode(eval_state_t *state)
{
   const int nops = vcode_count_ops();
   for (int i = 0; i < nops && !(state->failed); i++) {
      switch (vcode_get_op(i)) {
      case VCODE_OP_COMMENT:
         break;

      case VCODE_OP_CONST:
         eval_op_const(i, state);
         break;

      case VCODE_OP_CONST_REAL:
         eval_op_const_real(i, state);
         break;

      case VCODE_OP_RETURN:
         eval_op_return(i, state);
         return;

      case VCODE_OP_NOT:
         eval_op_not(i, state);
         break;

      case VCODE_OP_ADD:
         eval_op_add(i, state);
         break;

      case VCODE_OP_SUB:
         eval_op_sub(i, state);
         break;

      case VCODE_OP_MUL:
         eval_op_mul(i, state);
         break;

      case VCODE_OP_DIV:
         eval_op_div(i, state);
         break;

      case VCODE_OP_CMP:
         eval_op_cmp(i, state);
         break;

      case VCODE_OP_CAST:
         eval_op_cast(i, state);
         break;

      case VCODE_OP_NEG:
         eval_op_neg(i, state);
         break;

      case VCODE_OP_FCALL:
         if (state->flags & EVAL_FCALL)
            eval_op_fcall(i, state);
         else
            state->failed = true;
         break;

      case VCODE_OP_BOUNDS:
         eval_op_bounds(i, state);
         break;

      case VCODE_OP_CONST_ARRAY:
         eval_op_const_array(i, state);
         break;

      case VCODE_OP_WRAP:
         eval_op_wrap(i, state);
         break;

      case VCODE_OP_STORE:
         eval_op_store(i, state);
         break;

      case VCODE_OP_UNWRAP:
         eval_op_unwrap(i, state);
         break;

      case VCODE_OP_UARRAY_LEN:
         eval_op_uarray_len(i, state);
         break;

      case VCODE_OP_MEMCMP:
         eval_op_memcmp(i, state);
         break;

      case VCODE_OP_AND:
         eval_op_and(i, state);
         break;

      case VCODE_OP_OR:
         eval_op_or(i, state);
         break;

      case VCODE_OP_COND:
         eval_op_cond(i, state);
         return;

      case VCODE_OP_JUMP:
         eval_op_jump(i, state);
         return;

      case VCODE_OP_LOAD:
         eval_op_load(i, state);
         break;

      case VCODE_OP_UNDEFINED:
         eval_op_undefined(i, state);
         break;

      case VCODE_OP_NESTED_FCALL:
         eval_op_nested_fcall(i, state);
         break;

      case VCODE_OP_CASE:
         eval_op_case(i, state);
         return;

      case VCODE_OP_MOD:
         eval_op_mod(i, state);
         break;

      case VCODE_OP_REM:
         eval_op_rem(i, state);
         break;

      case VCODE_OP_DYNAMIC_BOUNDS:
         eval_op_dynamic_bounds(i, state);
         break;

      case VCODE_OP_INDEX:
         eval_op_index(i, state);
         break;

      case VCODE_OP_COPY:
         eval_op_copy(i, state);
         break;

      case VCODE_OP_LOAD_INDIRECT:
         eval_op_load_indirect(i, state);
         break;

      case VCODE_OP_STORE_INDIRECT:
         eval_op_store_indirect(i, state);
         break;

      case VCODE_OP_REPORT:
         eval_op_report(i, state);
         break;

      case VCODE_OP_ASSERT:
         eval_op_assert(i, state);
         break;

      case VCODE_OP_SELECT:
         eval_op_select(i, state);
         break;

      case VCODE_OP_ALLOCA:
         eval_op_alloca(i, state);
         break;

      case VCODE_OP_INDEX_CHECK:
         eval_op_index_check(i, state);
         break;

      case VCODE_OP_ABS:
         eval_op_abs(i, state);
         break;

      case VCODE_OP_IMAGE:
         eval_op_image(i, state);
         break;

      case VCODE_OP_HEAP_SAVE:
      case VCODE_OP_HEAP_RESTORE:
         break;

      case VCODE_OP_UARRAY_LEFT:
         eval_op_uarray_left(i, state);
         break;

      case VCODE_OP_UARRAY_RIGHT:
         eval_op_uarray_right(i, state);
         break;

      case VCODE_OP_UARRAY_DIR:
         eval_op_uarray_dir(i, state);
         break;

      case VCODE_OP_EXP:
         eval_op_exp(i, state);
         break;

      default:
         vcode_dump();
         fatal("cannot evaluate vcode op %s", vcode_op_string(vcode_get_op(i)));
      }
   }
}

tree_t eval(tree_t fcall, eval_flags_t flags)
{
   assert(tree_kind(fcall) == T_FCALL);

   type_t type = tree_type(fcall);
   if (!type_is_scalar(type))
      return fcall;

   if (!eval_possible(fcall, flags))
      return fcall;

   if (getenv("NVC_EVAL_VERBOSE"))
      flags |= EVAL_VERBOSE;

   if (flags & EVAL_VERBOSE)
      flags |= EVAL_WARN | EVAL_BOUNDS;

   vcode_unit_t thunk = lower_thunk(fcall);
   if (thunk == NULL)
      return fcall;

   if (flags & EVAL_VERBOSE)
      note_at(tree_loc(fcall), "evaluate thunk for %s",
              istr(tree_ident(fcall)));

   vcode_select_unit(thunk);
   vcode_select_block(0);

   context_t *context = eval_new_context(NULL);

   eval_state_t state = {
      .context = context,
      .result  = -1,
      .fcall   = fcall,
      .failed  = false,
      .flags   = flags,
   };

   eval_vcode(&state);
   free(state.heap);

   if (state.failed) {
      eval_free_context(state.context);
      return fcall;
   }

   assert(state.result != -1);
   value_t result = context->regs[state.result];
   eval_free_context(state.context);

   if (flags & EVAL_VERBOSE) {
      const char *name = istr(tree_ident(fcall));
      if (result.kind == VALUE_INTEGER)
         note_at(tree_loc(fcall), "%s returned %"PRIi64, name, result.integer);
      else
         note_at(tree_loc(fcall), "%s returned %lf", name, result.real);
   }

   switch (result.kind) {
   case VALUE_INTEGER:
      if (type_is_enum(type))
         return get_enum_lit(fcall, result.integer);
      else
         return get_int_lit(fcall, result.integer);
   case VALUE_REAL:
      return get_real_lit(fcall, result.real);
   default:
      fatal_trace("eval result is not scalar");
   }
}

int eval_errors(void)
{
   return errors;
}

static tree_t fold_tree_fn(tree_t t, void *context)
{
   switch (tree_kind(t)) {
   case T_FCALL:
      return eval(t, EVAL_FCALL | EVAL_FOLDING);

   case T_REF:
      {
         tree_t decl = tree_ref(t);
         switch (tree_kind(decl)) {
         case T_CONST_DECL:
            {
               tree_t value = tree_value(decl);
               if (tree_kind(value) == T_LITERAL)
                  return value;
               else
                  return t;
            }

         case T_UNIT_DECL:
            return tree_value(decl);

         default:
            return t;
         }
      }

   default:
      return t;
   }
}

void fold(tree_t top)
{
   tree_rewrite(top, fold_tree_fn, NULL);
}
