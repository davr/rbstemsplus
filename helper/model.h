/* model.c: the model through Pioneer's ONNX Runtime, one chunk at a time */
#ifndef RBSP_MODEL_H
#define RBSP_MODEL_H
#include <stddef.h>

typedef struct model model;
int model_load_ort(const char *lib, char *why, size_t whyn);
model *model_open(const char *path, int threads, int ours, char *why, size_t whyn);
void model_close(model *m);
/* x [4][4][BINS][F] and xt [8][L], malloc'd */
int model_run(model *m, const float *mix, size_t L, int ours, float **x, float **xt, size_t *F, char *why, size_t whyn);
#endif
