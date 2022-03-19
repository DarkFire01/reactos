/* taken from wine exit.c */
#include <precomp.h>

typedef void (__stdcall *_tls_callback_type)(void*,ULONG,void*);
static _tls_callback_type tls_atexit_callback;

/*********************************************************************
 *		__dllonexit (MSVCRT.@)
 */
_onexit_t CDECL __dllonexit(_onexit_t func, _onexit_t **start, _onexit_t **end)
{
   _onexit_t *tmp;
   size_t len;

  TRACE("(%p,%p,%p)\n", func, start, end);

  if (!start || !*start || !end || !*end)
  {
   FIXME("bad table\n");
   return NULL;
  }

  len = (*end - *start);

  TRACE("table start %p-%p, %d entries\n", *start, *end, len);

  if (++len <= 0)
    return NULL;

  tmp = realloc(*start, len * sizeof(_onexit_t));
  if (!tmp)
    return NULL;
  *start = tmp;
  *end = tmp + len;
  tmp[len - 1] = func;
  TRACE("new table start %p-%p, %d entries\n", *start, *end, len);
  return func;
}

/*********************************************************************
 *              _register_thread_local_exe_atexit_callback (MSVCRT.@)
 */
void CDECL _register_thread_local_exe_atexit_callback(_tls_callback_type callback)
{
    TRACE("(%p)\n", callback);
    tls_atexit_callback = callback;
}

/*********************************************************************
 *              _crt_atexit (MSVCRT.@)
 */
int CDECL _crt_atexit(void (__cdecl *func)(void))
{
  TRACE("(%p)\n", func);
  return _onexit((_onexit_t)func) == (_onexit_t)func ? 0 : -1;
}

/*********************************************************************
 *		_initialize_onexit_table (MSVCRT.@)
 */
int __cdecl _initialize_onexit_table(MSVCRT__onexit_table_t *table)
{
    if (!table)
        return -1;

    if (table->_first == table->_end)
        table->_last = table->_end = table->_first = NULL;
    return 0;
}

/*********************************************************************
 *		_register_onexit_function (MSVCRT.@)
 */
int __cdecl _register_onexit_function(MSVCRT__onexit_table_t *table, _onexit_t func)
{
    if (!table)
        return -1;

    _lock(_EXIT_LOCK1);
    if (!table->_first)
    {
        table->_first = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 32 * sizeof(void *));
        if (!table->_first)
        {
            _unlock(_EXIT_LOCK1);
            return -1;
        }
        table->_last = table->_first;
        table->_end = table->_first + 32;
    }

    /* grow if full */
    if (table->_last == table->_end)
    {
        int len = table->_end - table->_first;
        MSVCRT__onexit_t *tmp = HeapReAlloc(GetProcessHeap(), 0, table->_first, 2 * len * sizeof(void *));
        if (!tmp)
        {
            _unlock(_EXIT_LOCK1);
            return -1;
        }
        table->_first = tmp;
        table->_end = table->_first + 2 * len;
        table->_last = table->_first + len;
    }

    *table->_last = (MSVCRT__onexit_t)func;
    table->_last++;
    _unlock(_EXIT_LOCK1);
    return 0;
}

/*********************************************************************
 *		_execute_onexit_table (MSVCRT.@)
 */
int __cdecl _execute_onexit_table(MSVCRT__onexit_table_t *table)
{
    MSVCRT__onexit_t *func;
    MSVCRT__onexit_table_t copy;

    if (!table)
        return -1;

    _lock(_EXIT_LOCK1);
    if (!table->_first || table->_first >= table->_last)
    {
        _unlock(_EXIT_LOCK1);
        return 0;
    }
    copy._first = table->_first;
    copy._last  = table->_last;
    copy._end   = table->_end;
    memset(table, 0, sizeof(*table));
    _initialize_onexit_table(table);
    _unlock(_EXIT_LOCK1);

    for (func = copy._last - 1; func >= copy._first; func--)
    {
        if (*func)
           (*func)();
    }

    HeapFree(GetProcessHeap(), 0, copy._first);
    return 0;
}

