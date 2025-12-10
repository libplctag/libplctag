#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "buf.h"
#include "err.h"
#include "register_storage.h"

/* Modbus TCP Application Protocol (MBAP) Header */
#define MBAP_HEADER_SIZE 7

/* Modbus Function Codes */
#define MODBUS_FC_READ_COILS                0x01
#define MODBUS_FC_READ_DISCRETE_INPUTS      0x02
#define MODBUS_FC_READ_HOLDING_REGISTERS    0x03
#define MODBUS_FC_READ_INPUT_REGISTERS      0x04
#define MODBUS_FC_WRITE_SINGLE_COIL         0x05
#define MODBUS_FC_WRITE_SINGLE_REGISTER     0x06
#define MODBUS_FC_WRITE_MULTIPLE_COILS      0x0F
#define MODBUS_FC_WRITE_MULTIPLE_REGISTERS  0x10

/* Modbus Exception Codes */
#define MODBUS_EXCEPTION_ILLEGAL_FUNCTION        0x01
#define MODBUS_EXCEPTION_ILLEGAL_ADDRESS         0x02
#define MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE      0x03
#define MODBUS_EXCEPTION_DEVICE_FAILURE          0x04

/* Maximum Modbus PDU size (260 bytes) */
#define MODBUS_MAX_PDU_SIZE 260

/* Maximum ADU (MBAP + PDU) size */
#define MODBUS_MAX_ADU_SIZE (MBAP_HEADER_SIZE + MODBUS_MAX_PDU_SIZE)

#define MODBUS_MAX_READ_RESPONSE_BYTES 250

#define MODBUS_MAX_READ_COILS 2000
#define MODBUS_MAX_READ_DISCRETE_INPUTS 2000
#define MODBUS_MAX_READ_REGISTERS 125

#define MODBUS_MAX_WRITE_COILS 1968
#define MODBUS_MAX_WRITE_REGISTERS 123

/* MBAP Header structure */
typedef struct {
    uint16_t transaction_id;
    uint16_t protocol_id;
    uint16_t length;
    uint8_t unit_id;
} mbap_header_t;

/**
 * @brief Parse MBAP header from buffer
 * @param buf Buffer to read from (will advance read cursor)
 * @param header OUT: Parsed MBAP header
 * @return UTIL_OK on success, error code on failure
 */
util_err_t modbus_parse_mbap_header(buf_t *buf, mbap_header_t *header);

/**
 * @brief Build MBAP header and response PDU header
 * @param response Buffer to write to
 * @param req_header Request header (for transaction_id, etc.)
 * @param pdu_length Length of response PDU (not including MBAP)
 * @return UTIL_OK on success, error code on failure
 */
util_err_t modbus_build_response_header(buf_t *response,
                                        const mbap_header_t *req_header,
                                        uint16_t pdu_length);

/**
 * @brief Build Modbus exception response
 * @param response Buffer to write to (must be reset)
 * @param req_header Request header
 * @param function_code Original function code
 * @param error Error code (will be mapped to Modbus exception)
 */
void modbus_build_exception_response(buf_t *response,
                                     const mbap_header_t *req_header,
                                     uint8_t function_code,
                                     util_err_t error);

/**
 * @brief Process a Modbus request and generate response
 * @param function_code Function code from request
 * @param request Buffer with request PDU (positioned after FC)
 * @param response Buffer for response (will be written to)
 * @param req_header Request MBAP header
 * @param storage Register storage
 * @return UTIL_OK on success, error code otherwise
 */
util_err_t modbus_process_request(uint8_t function_code,
                                  buf_t *request,
                                  buf_t *response,
                                  const mbap_header_t *req_header,
                                  register_storage_t *storage);
