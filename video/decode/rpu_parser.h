#ifndef MPV_RPU_PARSER_H
#define MPV_RPU_PARSER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RpuOpaque RpuOpaque;

typedef struct {
    const uint8_t *data;
    size_t len;
} dovi_data_t;

typedef struct {
    RpuOpaque **list;
    size_t len;
    const char *error;
} RpuOpaqueList;

RpuOpaque *dovi_parse_unspec62_nalu(const uint8_t *buf, size_t len);
void dovi_rpu_free(RpuOpaque *ptr);
const char *dovi_rpu_get_error(const RpuOpaque *ptr);
const dovi_data_t *dovi_write_unspec62_nalu(RpuOpaque *ptr);
void dovi_data_free(const dovi_data_t *data);
int32_t dovi_convert_rpu_with_mode(RpuOpaque *ptr, uint8_t mode);
const RpuOpaqueList *dovi_generate_from_json(const char *json);
void dovi_rpu_list_free(const RpuOpaqueList *ptr);

#ifdef __cplusplus
}
#endif

#endif
