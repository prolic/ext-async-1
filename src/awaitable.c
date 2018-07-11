/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2018 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Martin Schröder <m.schroeder2007@gmail.com>                 |
  +----------------------------------------------------------------------+
*/

#include "php_task.h"

zend_class_entry *concurrent_awaitable_ce;


void concurrent_awaitable_register_continuation(concurrent_awaitable_cb **cont, void *obj, zval *data, concurrent_awaitable_func func)
{
	concurrent_awaitable_cb *current;

	current = emalloc(sizeof(concurrent_awaitable_cb));

	current->object = obj;
	current->func = func;
	current->next = NULL;

	if (data == NULL) {
		ZVAL_UNDEF(&current->data);
	} else {
		ZVAL_COPY(&current->data, data);
	}

	if (*cont == NULL) {
		*cont = current;
	} else {
		(*cont)->next = current;
	}
}

void concurrent_awaitable_trigger_continuation(concurrent_awaitable_cb **cont, zval *result, zend_bool success)
{
	concurrent_awaitable_cb *current;
	concurrent_awaitable_cb *next;

	current = *cont;

	if (current != NULL) {
		do {
			next = current->next;
			*cont = next;

			current->func(current->object, &current->data, result, success);

			zval_ptr_dtor(&current->data);

			efree(current);

			current = next;
		} while (current != NULL);
	}

	*cont = NULL;
}

void concurrent_awaitable_dispose_continuation(concurrent_awaitable_cb **cont)
{
	concurrent_awaitable_cb *current;
	concurrent_awaitable_cb *next;

	current = *cont;

	if (current != NULL) {
		do {
			next = current->next;
			*cont = next;

			current->func(current->object, &current->data, NULL, 0);

			zval_ptr_dtor(&current->data);

			efree(current);

			current = next;
		} while (current != NULL);
	}

	*cont = NULL;
}

static int concurrent_awaitable_implement_interface(zend_class_entry *interface, zend_class_entry *implementor)
{
	if (implementor == concurrent_deferred_awaitable_ce) {
		return SUCCESS;
	}

	if (implementor == concurrent_task_ce) {
		return SUCCESS;
	}

	zend_error_noreturn(
		E_CORE_ERROR,
		"Class %s must not implement interface %s, create an awaitable using %s instead",
		ZSTR_VAL(implementor->name),
		ZSTR_VAL(interface->name),
		ZSTR_VAL(concurrent_deferred_ce->name)
	);

	return FAILURE;
}

static const zend_function_entry awaitable_functions[] = {
	ZEND_FE_END
};


void concurrent_awaitable_ce_register()
{
	zend_class_entry ce;

	INIT_CLASS_ENTRY(ce, "Concurrent\\Awaitable", awaitable_functions);
	concurrent_awaitable_ce = zend_register_internal_interface(&ce);
	concurrent_awaitable_ce->interface_gets_implemented = concurrent_awaitable_implement_interface;
}


/*
 * vim: sw=4 ts=4
 * vim600: fdm=marker
 */
