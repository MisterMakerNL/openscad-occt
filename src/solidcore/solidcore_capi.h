#pragma once
// The C ABI of libsolidcore_capi.so (see solidcore/crates/solidcore-capi).
// Ids are 1-based B-rep registry handles owned by the ScKernel; booleans
// consume their operand ids. Errors are negative ids / -1 / NaN; nothing panics
// across the boundary. Meshing (sc_mesh) is unsupported (-1): no tessellator is
// ported, so this backend compares kernels via volume + bbox instead.
#include <cstdint>

extern "C" {

using ScKernel = void;

ScKernel* sc_kernel_new();
void sc_kernel_free(ScKernel* k);

int64_t sc_box(ScKernel* k, double x, double y, double z, double sx, double sy, double sz);
int64_t sc_cylinder(ScKernel* k, double x, double y, double z, double r, double h);
int64_t sc_cone(ScKernel* k, double x, double y, double z, double r1, double r2, double h);
int64_t sc_sphere(ScKernel* k, double x, double y, double z, double r);

int64_t sc_union(ScKernel* k, int64_t a, int64_t b);
int64_t sc_difference(ScKernel* k, int64_t a, int64_t b);
int64_t sc_intersection(ScKernel* k, int64_t a, int64_t b);

int32_t sc_translate(ScKernel* k, int64_t id, double x, double y, double z);
int32_t sc_rotate(ScKernel* k, int64_t id, double angle_deg, double ax, double ay, double az);
int32_t sc_scale(ScKernel* k, int64_t id, double sx, double sy, double sz);

int32_t sc_fillet_all(ScKernel* k, int64_t id, double radius);

// Debug text (NUL-terminated, valid until the next call) of the last error.
const uint8_t* sc_last_error(ScKernel* k);

double sc_volume(ScKernel* k, int64_t id);
// out[0..5] = xmin ymin zmin xmax ymax zmax
int32_t sc_bbox(ScKernel* k, int64_t id, double* out);

int32_t sc_mesh(ScKernel* k, int64_t id, double deflection, float** out_ptr);

void sc_free(void* p);

}  // extern "C"
