/*
  +----------------------------------------------------------------------+
  | parallel                                                              |
  +----------------------------------------------------------------------+
  | Copyright (c) Joe Watkins 2019                                       |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: krakjoe                                                      |
  +----------------------------------------------------------------------+
 */
#ifndef HAVE_PARALLEL_COPY
#define HAVE_PARALLEL_COPY

/////////////////////////////////////////////////////////////////////////////
// Adjustments to be able to copy-paste this from krakjoe/parallel/src/copy.c

#include "SAPI.h"
#include "php_main.h"
#include "zend_closures.h"
#include "zend_interfaces.h"
#include "zend_exceptions.h"
#include "zend_vm.h"

#include "copy.h"

#define php_parallel_exception_ex(type, m, ...) zend_throw_error(NULL, m, ##__VA_ARGS__)

static zend_class_entry *php_parallel_runtime_error_illegal_function_ce;
static zend_class_entry *php_parallel_runtime_error_illegal_instruction_ce;
static zend_class_entry *php_parallel_runtime_error_illegal_parameter_ce;
static zend_class_entry *php_parallel_runtime_error_illegal_return_ce;

void init_copy()
{
	php_parallel_runtime_error_illegal_function_ce = NULL;
	php_parallel_runtime_error_illegal_instruction_ce = NULL;
	php_parallel_runtime_error_illegal_parameter_ce = NULL;
	php_parallel_runtime_error_illegal_return_ce = NULL;
}

/////////////////////////////////////////////////////////////////////////////

#include "php_streams.h"
#include "php_network.h"

extern zend_string* php_parallel_runtime_main;

static const uint32_t uninitialized_bucket[-HT_MIN_MASK] = {HT_INVALID_IDX, HT_INVALID_IDX};

static zend_always_inline void* php_parallel_copy_mem(void *source, size_t size, zend_bool persistent) {
	void *destination = (void*) pemalloc(size, persistent);

	memcpy(destination, source, size);

	return destination;
}

HashTable *php_parallel_copy_hash(HashTable *source, zend_bool persistent);

static zend_always_inline zend_bool php_parallel_resource_castable(zval *zv) {
	zend_resource *resource = Z_RES_P(zv);

	if (resource->type == php_file_le_stream() ||
	    resource->type == php_file_le_pstream()) {
		return 1;
	}

	return 0;
}

static zend_always_inline void php_parallel_cast_resource(zval *dest, zval *source) {
	zend_resource *resource = Z_RES_P(source);
#ifndef _WIN32
	if (resource->type == php_file_le_stream() || resource->type == php_file_le_pstream()) {
		int fd;
		php_stream *stream = zend_fetch_resource2_ex(
					source, "stream", 
					php_file_le_stream(), 
					php_file_le_pstream());

		if (stream) {
			if (php_stream_cast(stream, PHP_STREAM_AS_FD, (void*)&fd, 0) == SUCCESS) {
				ZVAL_LONG(dest, fd);
				return;
			}
		}
	}
#endif
	ZVAL_NULL(dest);
}

static zend_always_inline zend_string* php_parallel_copy_string(zend_string *source, zend_bool persistent) {
    zend_string *dest = 
        zend_string_alloc(
            ZSTR_LEN(source), persistent);
    
    memcpy(ZSTR_VAL(dest), ZSTR_VAL(source), ZSTR_LEN(source)+1);
    
    ZSTR_LEN(dest) = ZSTR_LEN(source);
    ZSTR_H(dest)   = ZSTR_H(source);
    
    return dest;
}

void php_parallel_copy_zval(zval *dest, zval *source, zend_bool persistent) {
	switch (Z_TYPE_P(source)) {
		case IS_NULL:
		case IS_TRUE:
		case IS_FALSE:
		case IS_LONG:
		case IS_DOUBLE:
		case IS_UNDEF:
			if (source != dest) {
			    *dest = *source;
			}
		break;

		case IS_STRING:
		    ZVAL_STR(dest, php_parallel_copy_string(Z_STR_P(source), persistent));
		break;

		case IS_ARRAY:
			ZVAL_ARR(dest, php_parallel_copy_hash(Z_ARRVAL_P(source), persistent));
		break;

		case IS_RESOURCE:
			if (php_parallel_resource_castable(source)) {
				php_parallel_cast_resource(dest, source);
				break;
			}

		default:
			ZVAL_BOOL(dest, zend_is_true(source));
	}
}

static zend_always_inline HashTable* php_parallel_copy_hash_permanent(HashTable *source) {
	HashTable *ht = php_parallel_copy_mem(source, sizeof(HashTable), 1);
	uint32_t idx;

	GC_SET_REFCOUNT(ht, 1);
	GC_SET_PERSISTENT_TYPE(ht, GC_ARRAY);

	ht->pDestructor = php_parallel_zval_dtor;

#if PHP_VERSION_ID < 70300
	ht->u.flags |= HASH_FLAG_APPLY_PROTECTION|HASH_FLAG_PERSISTENT;
#endif

	ht->u.flags |= HASH_FLAG_STATIC_KEYS;
	if (ht->nNumUsed == 0) {
#if PHP_VERSION_ID >= 70400
		ht->u.flags = HASH_FLAG_UNINITIALIZED;
#else
		ht->u.flags &= ~(HASH_FLAG_INITIALIZED|HASH_FLAG_PACKED);
#endif
		ht->nNextFreeElement = 0;
		ht->nTableMask = HT_MIN_MASK;
		HT_SET_DATA_ADDR(ht, &uninitialized_bucket);
		return ht;
	}

	ht->nNextFreeElement = 0;
	ht->nInternalPointer = HT_INVALID_IDX;
	HT_SET_DATA_ADDR(ht, php_parallel_copy_mem(HT_GET_DATA_ADDR(ht), HT_USED_SIZE(ht), 1));
	for (idx = 0; idx < ht->nNumUsed; idx++) {
		Bucket *p = ht->arData + idx;
		if (Z_TYPE(p->val) == IS_UNDEF) continue;

		if (ht->nInternalPointer == HT_INVALID_IDX) {
			ht->nInternalPointer = idx;
		}

		if (p->key) {
			p->key = php_parallel_copy_string(p->key, 1);
			ht->u.flags &= ~HASH_FLAG_STATIC_KEYS;
		} else if ((zend_long) p->h >= (zend_long) ht->nNextFreeElement) {
			ht->nNextFreeElement = p->h + 1;
		}

		if (Z_REFCOUNTED(p->val)) {
		    php_parallel_copy_zval(&p->val, &p->val, 1);
		}
	}

	return ht;
}

static zend_always_inline HashTable* php_parallel_copy_hash_request(HashTable *source) {
	HashTable *ht = php_parallel_copy_mem(source, sizeof(HashTable), 0);

	GC_TYPE_INFO(ht) = GC_ARRAY;

#if PHP_VERSION_ID < 70300
    ht->u.flags &= ~HASH_FLAG_PERSISTENT;
#endif

	if (ht->nNumUsed == 0) {
		HT_SET_DATA_ADDR(ht, &uninitialized_bucket);
		return ht;
	}

	HT_SET_DATA_ADDR(ht, emalloc(HT_SIZE(ht)));
	memcpy(
	    HT_GET_DATA_ADDR(ht),
	    HT_GET_DATA_ADDR(source),
	    HT_HASH_SIZE(ht->nTableMask));

	if (ht->u.flags & HASH_FLAG_STATIC_KEYS) {
		Bucket *p = ht->arData, 
		       *q = source->arData, 
		       *p_end = p + ht->nNumUsed;
		for (; p < p_end; p++, q++) {
			*p = *q;
			if (Z_REFCOUNTED(p->val)) {
			    php_parallel_copy_zval(&p->val, &p->val, 0);
			}
		}
	} else {
		Bucket *p = ht->arData, 
		       *q = source->arData, 
		       *p_end = p + ht->nNumUsed;
		for (; p < p_end; p++, q++) {
			if (Z_TYPE(q->val) == IS_UNDEF) {
				ZVAL_UNDEF(&p->val);
				continue;
			}

			p->val = q->val;
			p->h = q->h;
			if (q->key) {
				p->key = php_parallel_copy_string(q->key, 0);
			} else {
				p->key = NULL;
			}
			
			if (Z_REFCOUNTED(p->val)) {
			    php_parallel_copy_zval(&p->val, &p->val, 0);
			}
		}
	}

	return ht;
}

HashTable *php_parallel_copy_hash(HashTable *source, zend_bool persistent) {
    if (persistent) {
        return php_parallel_copy_hash_permanent(source);
    }
    return php_parallel_copy_hash_request(source);
}

/* {{{ */
static inline HashTable* php_parallel_copy_statics(HashTable *old, zend_bool persistent) {
	return php_parallel_copy_hash(old, persistent);
} /* }}} */

/* {{{ */
static inline zend_string** php_parallel_copy_variables(zend_string **old, int end, zend_bool persistent) {
	zend_string **variables = pecalloc(end, sizeof(zend_string*), persistent);
	int it = 0;

	while (it < end) {
		variables[it] = 
			php_parallel_copy_string(old[it], persistent);
		if (!persistent) {
			zend_is_auto_global(variables[it]);
		}
		it++;
	}
	
	return variables;
} /* }}} */

/* {{{ */
static inline zend_try_catch_element* php_parallel_copy_try(zend_try_catch_element *old, int end, zend_bool persistent) {	
	zend_try_catch_element *try_catch = pecalloc(end, sizeof(zend_try_catch_element), persistent);
	
	memcpy(
		try_catch, 
		old,
		sizeof(zend_try_catch_element) * end);
	
	return try_catch;
} /* }}} */

static inline zend_live_range* php_parallel_copy_live(zend_live_range *old, int end, zend_bool persistent) { /* {{{ */
	zend_live_range *range = pecalloc(end, sizeof(zend_live_range), persistent);

	memcpy(
		range,
		old,
		sizeof(zend_live_range) * end);

	return range;
} /* }}} */

/* {{{ */
static inline zval* php_parallel_copy_literals(zval *old, int end, zend_bool persistent) {
	zval *literals = (zval*) pecalloc(end, sizeof(zval), persistent);
	int it = 0;

	memcpy(literals, old, sizeof(zval) * end);

	while (it < end) {
		if (!persistent && Z_TYPE(literals[it]) == IS_STRING) {
			zend_is_auto_global(Z_STR(literals[it]));
		}

		php_parallel_copy_zval(&literals[it], &old[it], persistent);
		it++;
	}
	
	return literals;
} /* }}} */

/* {{{ */
static inline zend_op* php_parallel_copy_opcodes(zend_op_array *op_array, zval *literals, zend_bool persistent) {
	zend_op *copy = pecalloc(
		op_array->last, sizeof(zend_op), persistent);

	memcpy(copy, op_array->opcodes, sizeof(zend_op) * op_array->last);

	{
		zend_op *opline = copy;
		zend_op *end    = copy + op_array->last;

		for (; opline < end; opline++) {
#if ZEND_USE_ABS_CONST_ADDR
			if (opline->op1_type == IS_CONST)
			    opline->op1.zv = (zval*)((char*)opline->op1.zv + ((char*)op_array->literals - (char*)literals));
			if (opline->op2_type == IS_CONST) 
				opline->op2.zv = (zval*)((char*)opline->op2.zv + ((char*)op_array->literals - (char*)literals));
#elif PHP_VERSION_ID >= 70300
			if (opline->op1_type == IS_CONST) {
				opline->op1.constant =
					(char*)(op_array->literals +
						((zval*)((char*)(op_array->opcodes + (opline - copy)) +
						(int32_t)opline->op1.constant) - literals)) -
					(char*)opline;
			}
			if (opline->op2_type == IS_CONST) {
				opline->op2.constant =
					(char*)(op_array->literals +
						((zval*)((char*)(op_array->opcodes + (opline - copy)) +
						(int32_t)opline->op2.constant) - literals)) -
					(char*)opline;
			}
#endif

#if ZEND_USE_ABS_JMP_ADDR
			if ((op_array->fn_flags & ZEND_ACC_DONE_PASS_TWO) != 0) {
				switch (opline->opcode) {
					case ZEND_JMP:
					case ZEND_FAST_CALL:
					case ZEND_DECLARE_ANON_CLASS:
					case ZEND_DECLARE_ANON_INHERITED_CLASS:
						 opline->op1.jmp_addr = &copy[opline->op1.jmp_addr - op_array->opcodes];
					break;

					case ZEND_JMPZNZ:
					case ZEND_JMPZ:
					case ZEND_JMPNZ:
					case ZEND_JMPZ_EX:
					case ZEND_JMPNZ_EX:
					case ZEND_JMP_SET:
					case ZEND_COALESCE:
					case ZEND_NEW:
					case ZEND_FE_RESET_R:
					case ZEND_FE_RESET_RW:
					case ZEND_ASSERT_CHECK:
						opline->op2.jmp_addr = &copy[opline->op2.jmp_addr - op_array->opcodes];
					break;
				}
			}
#endif
			zend_vm_set_opcode_handler(opline);
		}
	}

	return copy;
} /* }}} */

/* {{{ */
static inline zend_arg_info* php_parallel_copy_arginfo(zend_op_array *op_array, zend_arg_info *old, uint32_t end, zend_bool persistent) {
	zend_arg_info *info;
	uint32_t it = 0;

	if (op_array->fn_flags & ZEND_ACC_HAS_RETURN_TYPE) {
		old--;
		end++;
	}

	if (op_array->fn_flags & ZEND_ACC_VARIADIC) {
		end++;
	}

	info = pecalloc
		(end, sizeof(zend_arg_info), persistent);
	memcpy(info, old, sizeof(zend_arg_info) * end);	

	while (it < end) {
		if (info[it].name)
			info[it].name = php_parallel_copy_string(old[it].name, persistent);
		it++;
	}
	
	if (op_array->fn_flags & ZEND_ACC_HAS_RETURN_TYPE) {
		info++;
	}
	
	return info;
} /* }}} */

static zend_always_inline zend_bool php_parallel_copying_lexical(zend_execute_data *execute_data, const zend_function *function, zend_op *bind) { /* {{{ */
	zend_op *opline, *end;

	if (EX(func)->type != ZEND_USER_FUNCTION) {
		return 0;
	}
	
	opline = EX(func)->op_array.opcodes;
	end    = opline + EX(func)->op_array.last;

	while (opline < end) {
		if (opline->opcode == ZEND_BIND_LEXICAL) {
			if (zend_string_equals(
				zend_get_compiled_variable_name((zend_op_array*)function, bind->op1.var), 
				zend_get_compiled_variable_name((zend_op_array*)EX(func), opline->op2.var))) {
				return 1;
			}
		}
		opline++;
	}

	return 0;
} /* }}} */

zend_bool php_parallel_copy_arginfo_check(const zend_function *function) { /* {{{ */
	zend_arg_info *it, *end;
	int argc = 1;

	if (!function->op_array.arg_info) {
		return 1;
	}

	if (function->common.fn_flags & ZEND_ACC_HAS_RETURN_TYPE) {
		it = function->op_array.arg_info - 1;

#if PHP_VERSION_ID >= 70200
		if (ZEND_TYPE_IS_SET(it->type) && (ZEND_TYPE_CODE(it->type) == IS_OBJECT || ZEND_TYPE_IS_CLASS(it->type))) {
#else
		if (it->type_hint == IS_OBJECT || it->class_name) {
#endif
			php_parallel_exception_ex(
			    php_parallel_runtime_error_illegal_return_ce,
				"illegal return (object) from task");
			return 0;
		}

		if (it->pass_by_reference) {
			php_parallel_exception_ex(
			    php_parallel_runtime_error_illegal_return_ce,
				"illegal return (reference) from task");
			return 0;
		}
	}

	it = function->op_array.arg_info;
	end = it + function->op_array.num_args;

	if (function->common.fn_flags & ZEND_ACC_VARIADIC) {
		end++;
	}

	while (it < end) {
#if PHP_VERSION_ID >= 70200
		if (ZEND_TYPE_IS_SET(it->type) && (ZEND_TYPE_CODE(it->type) == IS_OBJECT || ZEND_TYPE_IS_CLASS(it->type))) {
#else
		if (it->type_hint == IS_OBJECT || it->class_name) {
#endif
			php_parallel_exception_ex(
			    php_parallel_runtime_error_illegal_parameter_ce,
				"illegal parameter (object) accepted by task at argument %d", argc);
			return 0;
		}

		if (it->pass_by_reference) {
			php_parallel_exception_ex(
			    php_parallel_runtime_error_illegal_parameter_ce,
				"illegal parameter (reference) accepted by task at argument %d", argc);
			return 0;
		}
		it++;
		argc++;
	}

	return 1;
} /* }}} */

static zend_bool php_parallel_copy_argv_check(zval *args, uint32_t *argc, zval *error) { /* {{{ */
	zval *arg;

	if (*argc == 0) {
		*argc = 1;
	}

	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(args), arg) {
		if (Z_TYPE_P(arg) == IS_OBJECT) {
			ZVAL_COPY_VALUE(error, arg);
			return 0;
		}

		if (Z_TYPE_P(arg) == IS_ARRAY) {
			if (!php_parallel_copy_argv_check(arg, argc, error)) {
				return 0;
			}
		}

		if (Z_TYPE_P(arg) == IS_RESOURCE) {
			if (!php_parallel_resource_castable(arg)) {
				ZVAL_COPY_VALUE(error, arg);
				return 0;
			}
		}

		(*argc)++;
	} ZEND_HASH_FOREACH_END();

	return 1;
} /* }}} */

zend_bool php_parallel_copy_check(zend_execute_data *execute_data, const zend_function * function, zval *argv, zend_bool *returns) { /* {{{ */
	zend_op *it = function->op_array.opcodes,
		*end = it + function->op_array.last;
	uint32_t errat = 0;
	zval errarg;
	
	if (function->type != ZEND_USER_FUNCTION) {
        php_parallel_exception_ex(
            php_parallel_runtime_error_illegal_function_ce,
            "illegal function type (internal)");
	    return 0;
	}

	if (!php_parallel_copy_arginfo_check(function)) {
		return 0;
	}

	if (argv && Z_TYPE_P(argv) == IS_ARRAY && !php_parallel_copy_argv_check(argv, &errat, &errarg)) {
        php_parallel_exception_ex(
            php_parallel_runtime_error_illegal_parameter_ce,
            "illegal parameter (%s) passed to task at argument %d", 
            zend_get_type_by_const(Z_TYPE(errarg)), errat);
		return 0;
	}

	while (it < end) {
		switch (it->opcode) {
			case ZEND_YIELD:
			case ZEND_YIELD_FROM:
                php_parallel_exception_ex(
                    php_parallel_runtime_error_illegal_instruction_ce,
                    "illegal instruction (yield) on line %d of task",
                    it->lineno - function->op_array.line_start);
				return 0;
				
			case ZEND_DECLARE_ANON_CLASS:
                php_parallel_exception_ex(
                    php_parallel_runtime_error_illegal_instruction_ce,
                    "illegal instruction (new class) on line %d of task",
                    it->lineno - function->op_array.line_start);
				return 0;

			case ZEND_DECLARE_LAMBDA_FUNCTION:
				php_parallel_exception_ex(
				    php_parallel_runtime_error_illegal_instruction_ce,
					"illegal instruction (function) on line %d of task",
					it->lineno - function->op_array.line_start);
				return 0;

			case ZEND_DECLARE_FUNCTION:
				php_parallel_exception_ex(
				    php_parallel_runtime_error_illegal_instruction_ce,
				    "illegal instruction (function) on line %d of task",
				    it->lineno - function->op_array.line_start);
				return 0;

			case ZEND_DECLARE_CLASS:
			case ZEND_DECLARE_INHERITED_CLASS:
			case ZEND_DECLARE_INHERITED_CLASS_DELAYED:
				php_parallel_exception_ex(
			        php_parallel_runtime_error_illegal_instruction_ce,
				    "illegal instruction (class) on line %d of task", 
				    it->lineno - function->op_array.line_start);
				return 0;

			case ZEND_BIND_STATIC:	
				if (php_parallel_copying_lexical(execute_data, function, it)) {
				    php_parallel_exception_ex(
					    php_parallel_runtime_error_illegal_instruction_ce,
						"illegal instruction (lexical) in task");
					return 0;
				}
			break;

            case ZEND_THROW:
			case ZEND_RETURN:
				if (!*returns && it->extended_value != -1) {
					if (EX(opline)->result_type == IS_UNUSED) {
						php_parallel_exception_ex(
						    php_parallel_runtime_error_illegal_return_ce,
							"return on line %d of task ignored by caller, "
							"caller must retain reference to Future",
							it->lineno - function->op_array.line_start);
						return 0;
					}
					*returns = 1;
				}
			break;
		}
		it++;
	}

	return 1;
} /* }}} */

zend_function* php_parallel_copy(const zend_function *function, zend_bool persistent) { /* {{{ */
	zend_function  *copy;	
	zend_op_array  *op_array;
	zend_string   **variables;
	zval           *literals;
	zend_arg_info  *arg_info;

	copy = (zend_function*) pecalloc(1, sizeof(zend_op_array), persistent);

	memcpy(copy, function, sizeof(zend_op_array));

	op_array = &copy->op_array;
	variables = op_array->vars;
	literals = op_array->literals;
	arg_info = op_array->arg_info;

	op_array->function_name = zend_string_copy(php_parallel_runtime_main);
	op_array->refcount = (uint32_t*) pemalloc(sizeof(uint32_t), persistent);
	(*op_array->refcount) = 1;
	op_array->filename = php_parallel_copy_string(op_array->filename, persistent);

	op_array->fn_flags &= ~ ZEND_ACC_CLOSURE;
	op_array->fn_flags &= ~ ZEND_ACC_DONE_PASS_TWO;
	op_array->fn_flags |= ZEND_ACC_PUBLIC;
	op_array->scope = NULL;
	op_array->prototype = NULL;
	op_array->doc_comment = NULL;
#if PHP_VERSION_ID >= 70400
	ZEND_MAP_PTR_NEW(op_array->run_time_cache);
#else
	op_array->run_time_cache = NULL;
#endif

	if (op_array->literals) {
		op_array->literals = php_parallel_copy_literals (literals, op_array->last_literal, persistent);
	}

	op_array->opcodes = php_parallel_copy_opcodes(op_array, literals, persistent);

	if (op_array->arg_info) {
		op_array->arg_info = php_parallel_copy_arginfo(op_array, arg_info, op_array->num_args, persistent);
	}

	if (op_array->live_range) {
		op_array->live_range = php_parallel_copy_live(op_array->live_range, op_array->last_live_range, persistent);
	}

	if (op_array->try_catch_array) {
		op_array->try_catch_array = php_parallel_copy_try(op_array->try_catch_array, op_array->last_try_catch, persistent);
	}

	if (op_array->vars) {
		op_array->vars = php_parallel_copy_variables(variables, op_array->last_var, persistent);
	}

	if (op_array->static_variables) {
		op_array->static_variables = php_parallel_copy_statics(op_array->static_variables, persistent);

#if PHP_VERSION_ID >= 70400
		ZEND_MAP_PTR_INIT(op_array->static_variables_ptr, &op_array->static_variables);
		
		op_array->fn_flags |= ZEND_ACC_IMMUTABLE;
#endif
	}
    
	return copy;
} /* }}} */

void php_parallel_copy_free(zend_function *function, zend_bool persistent) { /* {{{ */
	zend_op_array *ops = (zend_op_array*) function;

	if (ops->static_variables) {
		php_parallel_ht_dtor(ops->static_variables, persistent);
	}

	if (ops->vars) {
		int it = 0;
		int end = ops->last_var;

		while (it < end) {
			zend_string_release(ops->vars[it]);
			it++;
		}
		
		pefree(ops->vars, persistent);
	}

	if (ops->try_catch_array) {
		pefree(ops->try_catch_array, persistent);
	}

	if (ops->live_range) {
		pefree(ops->live_range, persistent);
	}

	if (ops->arg_info) {
		zend_arg_info *info = ops->arg_info;
		uint32_t it = 0;
		uint32_t end = ops->num_args;

		if (ops->fn_flags & ZEND_ACC_HAS_RETURN_TYPE) {
			info--;
			end++;
		}

		if (ops->fn_flags & ZEND_ACC_VARIADIC) {
			end++;
		}

		while (it < end) {
			if (info[it].name)
				zend_string_release(info[it].name);
			it++;
		}

		pefree(info, persistent);
	}

	if (ops->literals) {
		int it = 0;
		int end = ops->last_literal;

		while (it < end) {
		    if (!persistent && Z_REFCOUNTED(ops->literals[it])) {
		        GC_REMOVE_FROM_BUFFER(
		            Z_COUNTED(ops->literals[it]));
		    }
			php_parallel_zval_dtor(&ops->literals[it]);
			it++;
		}

		pefree(ops->literals, persistent);
	}
	
	zend_string_release(ops->filename);

	pefree(ops->opcodes, persistent);
	pefree(ops->refcount, persistent);
	pefree(ops, persistent);
} /* }}} */
#endif