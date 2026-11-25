/*
 * src/pl/plpython/plpy_plpymodule.h
 */

#ifndef PLPY_PLPYMODULE_H
#define PLPY_PLPYMODULE_H

#include "plpython.h"
#include "utils/hsearch.h"

/* 把 sqlstate 映射到异常的散列表，便于快速查找 */
extern HTAB *PLy_spi_exceptions;


PyMODINIT_FUNC PyInit_plpy(void);
extern void PLy_init_plpy(void);

#endif							/* PLPY_PLPYMODULE_H */
