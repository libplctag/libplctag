/***************************************************************************
 * ENIP packet budget/packing skeleton.
 ***************************************************************************/

#include <libplctag/protocols/enip/enip.h>
#include <utils/debug.h>

int enip_packetizer_placeholder_symbol = 0;

int enip_packetizer_plan_frame(void) {
    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, 0, "ENIP packetizer placeholder.");
    return PLCTAG_ERR_NOT_IMPLEMENTED;
}
