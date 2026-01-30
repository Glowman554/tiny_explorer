#ifndef QHULL_STUB_H
#define QHULL_STUB_H

#include <stdio.h>
#include <float.h>
#include <unistd.h>

typedef struct qhT {
    int num_facets;
    void* facet_list;
} qhT;
typedef struct vertexT {
    double* point;
} vertexT;
typedef struct facetT {
    void* next;
} facetT;

#define QHULL_LIB_CHECK
#define qh_ERRsingular 1
#define qh_ALL 0

inline void qh_zero(qhT* qh, FILE* err) { (void)qh; (void)err; }
inline int qh_new_qhull(qhT* qh, int dim, int n, double* pts, int ismalloc, char* cmd, FILE* out, FILE* err) { 
    (void)qh; (void)dim; (void)n; (void)pts; (void)ismalloc; (void)cmd; (void)out; (void)err;
    return 1; // Return error so it takes the fallback path
}
inline facetT* qh_nextfacet2d(void* f, vertexT** v) { (void)f; (void)v; return NULL; }
inline void qh_freeqhull(qhT* qh, int all) { (void)qh; (void)all; }
inline void qh_memfreeshort(qhT* qh, int* curlong, int* totlong) { (void)qh; *curlong = 0; *totlong = 0; }

#endif
