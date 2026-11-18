/* Minimal JSON value tree: parse, query, edit, serialize. */
#ifndef CB_JSON_H
#define CB_JSON_H
#include <stddef.h>
#include <stdbool.h>

typedef enum { J_NULL, J_BOOL, J_NUMBER, J_STRING, J_ARRAY, J_OBJECT } JType;

typedef struct JVal JVal;
struct JVal {
    JType type;
    bool b;
    double num;
    char *str;            /* J_STRING */
    JVal **items;         /* J_ARRAY values, or J_OBJECT values */
    char **keys;          /* J_OBJECT keys (parallel to items) */
    size_t n, cap;
};

JVal *json_parse(const char *text, size_t len);      /* NULL on error */
char *json_dump(const JVal *v, int indent);           /* malloc'd; indent 0 = compact */
void  json_free(JVal *v);
JVal *json_copy(const JVal *v);
bool  json_equal(const JVal *a, const JVal *b);

JVal *json_null(void);
JVal *json_bool(bool b);
JVal *json_number(double d);
JVal *json_string(const char *s);
JVal *json_array(void);
JVal *json_object(void);

JVal *json_get(const JVal *obj, const char *key);     /* NULL if absent / not object */
void  json_set(JVal *obj, const char *key, JVal *val); /* takes ownership; replaces */
void  json_del(JVal *obj, const char *key);
void  json_push(JVal *arr, JVal *val);                 /* takes ownership */

const char *json_str(const JVal *obj, const char *key);   /* NULL if absent / not string */
double      json_num(const JVal *obj, const char *key, double dflt);
bool        json_truthy(const JVal *obj, const char *key); /* true only for JSON true */

#endif
